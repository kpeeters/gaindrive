package org.gaindrive.android.playback.cast

import android.util.Log
import androidx.media3.common.MediaItem
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.StreamTarget
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.di.MediaHttp
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.playback.CastSource
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Who serves the bytes to the receiver. Three values rather than a `bridged`
 * flag, because the third is not a relay at all and is the one worth telling
 * apart: it is the branch that works with no connectivity, so a user asking
 * "why is this playing when the server is unreachable" has an answer.
 *
 * Note none of these is "the server drives the Chromecast" — that mode is the
 * web client's and is deliberately not ported; see `CAST.md`. The phone always
 * holds the control channel.
 */
enum class CastRoute {
	/** The receiver fetches `stream.view` from the owning server itself. */
	DIRECT,

	/** The receiver fetches from [CastBridge], which fetches from the server. */
	RELAY,

	/** The receiver fetches from [CastBridge], which serves a downloaded copy. */
	LOCAL,
}

/**
 * What to hand a Cast receiver for a track: a URL it can actually fetch, the
 * MIME type to declare for it, and how it got to be that URL.
 *
 * [url] points at the owning server, or at the bridge relaying that server, or
 * at the bridge serving a copy already on the device — and the receiver cannot
 * tell the three apart. The decision is made per track, so a queue that spans
 * servers may mix them, which is the one place several servers make casting
 * easier rather than harder. [route] is the same decision written down, for the
 * one observer that does need to tell them apart: the track info dialog.
 */
data class CastTarget(
	val url: String,
	val mimeType: String?,
	val route: CastRoute,
	/** What the server was asked to send. Null for video, which is never asked. */
	val quality: AudioQuality? = null,
	/**
	 * The grant [url] carries, so that the artwork and subtitles built
	 * afterwards can carry the same one. Null when this route hands the
	 * receiver no server URL at all, and when the server is too old to mint
	 * one — in both cases the ordinary credentials are what travel.
	 *
	 * It rides here because [CastUrls.artworkFor] and [CastUrls.captionsFor]
	 * are separate calls made once this has returned, and minting a second and
	 * third token for one track would burn three of the server's slots to say
	 * the same thing.
	 */
	val castToken: String? = null,
) {
	/** Whether artwork has to travel the same road; see [CastUrls.artworkFor]. */
	val bridged: Boolean get() = route != CastRoute.DIRECT
}

/**
 * Whether a Cast receiver will decode a file of this type as it stands.
 *
 * The set is what the Default Media Receiver documents, intersected with the
 * types `src/codecs.hh` can report. It reports seven for audio and only
 * `audio/x-ms-wma` is missing here — but this is an allowlist rather than a
 * one-entry denylist on purpose: a codec added to the server later would
 * otherwise be sent to a receiver that cannot play it, and demoting to a
 * transcode is the safe direction of a wrong guess.
 *
 * **ALAC is the gap it cannot close.** `songs.codec` is a file extension, so
 * the server reports `audio/mp4` for AAC and for ALAC alike and nothing here
 * can tell them apart. An ALAC `.m4a` cast at original quality will still fail.
 *
 * A null type is a queue the system restored from bare media ids, and is
 * refused for the same reason: not knowing is not the same as knowing it is
 * fine.
 *
 * Top-level rather than a member so it can be exercised without `android.util`
 * being loaded — the class it sits beside logs, this function does not.
 */
internal fun castPlaysNatively(mime: String?): Boolean = mime in CAST_NATIVE_TYPES

/**
 * Marks a URL as one that will be read at playback speed, so the server
 * delivers it at roughly 1x instead of as fast as the socket takes it.
 *
 * The server cannot work this out for itself, which is why it has to be said
 * here. It paces a browser by its `Mozilla/` User-Agent and its *own* cast by
 * knowing it started it; a receiver on our route is neither, since the app
 * holds the control channel itself. That makes it indistinguishable from a
 * third-party Subsonic client, which wants the opposite treatment.
 *
 * Note this is not the same question as [withCastToken] answers, and a URL
 * carrying a cast token still needs pacing: the token says who may fetch,
 * `pace` says how fast to send.
 *
 * What happens without it is not a slow stream but a dead one. A receiver
 * reads at 1x and stops reading once its buffer is full; unpaced, the server
 * writes the whole track into the socket within seconds and then blocks, and
 * from that moment the connection carries nothing. Something on the path is
 * counting — the receiver's own ~60 s no-data timeout, and a reverse proxy's
 * `ProxyTimeout`, which defaults to 60 s — and the track stops about ninety
 * seconds in. See `serve_direct()` in `src/streamer.cc`.
 *
 * Only ever on a URL a *receiver* fetches. Never on an ExoPlayer, download or
 * pin URL: nothing is playing off those in real time and pacing one would make
 * a pinned album take as long as it takes to listen to. That is why this is
 * applied here, at the route, rather than inside [StreamUrls] — whose builders
 * are shared with playback and the download queue.
 *
 * Top-level for the same reason as [castPlaysNatively]: it can then be
 * exercised without Hilt or `android.util`.
 */
internal fun paced(url: String): String {
	val parsed = url.toHttpUrlOrNull() ?: return url
	return parsed.newBuilder().addQueryParameter("pace", "true").build().toString()
}

/**
 * Swaps this account's credentials out of a URL for a token that opens one
 * track and nothing else.
 *
 * Applied at the route rather than in [StreamUrls], for the reason [paced] is:
 * those builders are shared with playback, downloads and pins, where the
 * ordinary credentials are exactly right. This is only ever for a URL a
 * *receiver* fetches.
 *
 * What it replaces is worth stating plainly. `u`/`t`/`s` are the account's
 * password — `t` is md5(password + salt) and `s` is the salt — so a television
 * handed them can read the whole library as this person for as long as the
 * password stands, and does so from its own logs and whatever is between. The
 * grant is one song, twelve hours, and reaches nothing the account could not
 * already read.
 *
 * `v`, `c` and `f` stay. The server ignores them on a grant-authed request and
 * `c` is what names this client in its log, which is worth keeping. Any
 * existing `castToken` is dropped first so applying this twice cannot leave
 * two.
 */
internal fun withCastToken(url: String, token: String): String {
	val parsed = url.toHttpUrlOrNull() ?: return url
	return parsed.newBuilder()
		.removeAllQueryParameters("u")
		.removeAllQueryParameters("t")
		.removeAllQueryParameters("s")
		.removeAllQueryParameters("castToken")
		.addQueryParameter("castToken", token)
		.build()
		.toString()
}

private val CAST_NATIVE_TYPES = setOf(
	"audio/flac",
	"audio/mpeg",
	"audio/mp4",
	"audio/aac",
	"audio/ogg",
	"audio/wav",
)

@Singleton
class CastUrls @Inject constructor(
	private val streamUrls: StreamUrls,
	private val registry: ServerRegistry,
	private val reachability: CastReachability,
	private val bridge: CastBridge,
	private val audioCache: AudioCache,
	// The client whose read timeout a whole-film transcode fits inside; see
	// MediaHttp, which is where this requirement now lives for every caller
	// that waits on one.
	@MediaHttp private val httpClient: OkHttpClient,
	private val settings: SettingsStore,
	private val clients: SubsonicClientFactory,
) {

	/**
	 * Decides who fetches the track, and then at what quality.
	 *
	 * That order is the reverse of what this used to do, and it is the whole
	 * shape of `SettingsStore.castOriginal`: the original file is worth sending
	 * only to a receiver that pulls it off the server itself, so the route has
	 * to be settled first. Asking costs nothing — [CastReachability] caches its
	 * answer per server and network, not per track.
	 *
	 * Every other route resolves exactly as local playback would, same quality
	 * and same per-server bitrate cap.
	 *
	 * [source] is what the queue entry already knows. Its MIME types are used
	 * rather than re-derived so that the type declared in the `LOAD` and the type
	 * the bridge puts on the response are the same answer and cannot drift.
	 */
	suspend fun forCast(ref: ItemRef, source: CastSource): CastTarget? {
		// One load starts here, and everything published for it — the stream,
		// the artwork, each subtitle track — is pinned against eviction until
		// the next one. A caption is published now and fetched only if somebody
		// turns it on, which by use alone makes it the first thing thrown away.
		bridge.beginLoad()
		if (source.isVideo) return forVideo(ref, source)

		val config = registry.get(ref.server) ?: return null

		if (reachability.canReachDirectly(config)) {
			val direct = directTarget(ref, source) ?: return null
			val url = paced(direct.url)
			val token = castToken(ref)
			return CastTarget(
				token?.let { withCastToken(url, it) } ?: url,
				direct.mimeType ?: source.sourceMime,
				CastRoute.DIRECT,
				direct.quality,
				token,
			)
		}

		val target = streamUrls.forPlayback(ref, source.audioOnlyVideo) ?: return null
		val mime = target.mimeType ?: source.sourceMime

		// Downloaded: serve the copy on the device rather than fetching it back
		// from the server through the phone. Not an optimisation — it is the
		// only branch that works with no connectivity at all, which is the state
		// a downloaded library exists for, and without it casting a downloaded
		// track offline handed the receiver a server URL nothing could fetch.
		//
		// Below the direct branch on purpose: a receiver that can reach the
		// server should still fetch for itself, because that survives the phone
		// sleeping, going out of range or running flat.
		storedCopy(target.cacheKey)?.let { length ->
			// **The stored bytes decide the type here, not the request.** This
			// copy was written by local playback, which declares what media3
			// takes as it stands, so an `@opus160` key can hold the original
			// MP3 — while `mime` above was derived from the quality asked for
			// and would say Ogg. Everywhere else that mismatch is absorbed by
			// an extractor sniffing; a receiver cannot, and one told audio/ogg
			// over MP3 refuses the media outright, on a television, with
			// nothing on the phone to say why.
			//
			// Falls back to `mime` when the bytes say nothing recognisable,
			// which is exactly what this line did before.
			val storedMime = audioCache.storedMimeType(target.cacheKey) ?: mime
			bridge.publishLocal(target.cacheKey, storedMime, length)?.let { url ->
				return CastTarget(url, storedMime, CastRoute.LOCAL, target.quality)
			}
		}

		// A bridge that will not start is not a reason to play nothing: the
		// direct URL may still work, since the probe is a guess about the
		// receiver, not a measurement of it. That fallback really does hand
		// over a server URL, so it reports DIRECT — the route says what was
		// done, not what was intended.
		//
		// [paced] covers both uses of it, and the relay needs it for a
		// different reason than the direct route does. The receiver never
		// starves behind the bridge, which forwards at whatever rate the
		// receiver reads — but that is exactly why the *upstream* leg goes
		// idle when the receiver stops reading, and that is the leg crossing
		// a reverse proxy, since the relay exists for precisely the topologies
		// that have one.
		val upstream = paced(target.url)
		val relayed = bridge.publish(upstream)
		if (relayed != null) {
			// No token, and not an oversight: on this route the server URL
			// never leaves the phone. The receiver is given a bridge URL and
			// the bridge fetches upstream itself, so the credentials are on a
			// request this device makes — which is the one place they belong.
			// Minting here would cost a round trip per track and one of the
			// server's grant slots to protect nothing.
			return CastTarget(relayed, mime, CastRoute.RELAY, target.quality)
		}
		// The bridge would not start, so the fallback really does hand a server
		// URL to the receiver — and therefore really does need a credential of
		// its own, exactly as the direct branch above.
		val token = castToken(ref)
		return CastTarget(
			token?.let { withCastToken(upstream, it) } ?: upstream,
			mime,
			CastRoute.DIRECT,
			target.quality,
			token,
		)
	}

	/**
	 * A grant for one track, or null to carry on with the ordinary credentials.
	 *
	 * Null is a normal answer rather than a failure. A server older than the
	 * endpoint answers an error, and the behaviour that leaves — the URL keeps
	 * `u`/`t`/`s` — is exactly what this app did before the endpoint existed,
	 * so there is nothing to tell the user and nothing to abandon the cast
	 * over. It is logged, because "why is the password still going to the
	 * television" deserves an answer in the log rather than a shrug.
	 *
	 * One per track, and not per seek: this app seeks the receiver rather than
	 * reloading it, so the receiver goes on fetching the URL it already has.
	 * That is why the server's grant lasts hours.
	 */
	private suspend fun castToken(ref: ItemRef): String? {
		val config = registry.get(ref.server) ?: return null
		val token = runCatchingCancellable {
			clients.clientFor(config).getCastToken(ref.id).requireOk().castToken
		}.onFailure {
			Log.i(TAG, "no cast token ($it); sending the account's own credentials")
		}.getOrNull()
		return token?.takeIf { it.isNotBlank() }
	}

	/**
	 * What to hand a receiver that will fetch from the server itself.
	 *
	 * Two conditions have to hold before the original is sent, and the second
	 * is not a caution but a fix: a `.wma` handed over as it stands fails the
	 * `LOAD` with `IDLE`/`ERROR`, is retried once by [LoadRetryWatcher], fails
	 * again and stalls — with nothing on screen to say why. Falling back to the
	 * streaming quality plays the track instead, and the log line is what makes
	 * "why is this one still Opus" answerable.
	 */
	private suspend fun directTarget(ref: ItemRef, source: CastSource): StreamTarget? {
		if (!settings.castOriginal.first())
			return streamUrls.forPlayback(ref, source.audioOnlyVideo)
		// "The original file" for a video played as audio is the film, which is
		// the one thing this route must not fetch. It is also the only place
		// the audio-only flag is read: everything else about such an item is
		// already audio, which is why it is not marked as a video at all.
		if (source.audioOnlyVideo) return streamUrls.forPlayback(ref, audioOnlyVideo = true)
		if (!castPlaysNatively(source.sourceMime)) {
			Log.i(
				TAG,
				"original ${source.sourceMime ?: "type unknown"} is not one the" +
					" receiver takes; sending the streaming quality",
			)
			return streamUrls.forPlayback(ref)
		}
		return streamUrls.forCastOriginal(ref)
	}

	/**
	 * Whether a video can be handed to a receiver at all, answered without
	 * building the URL.
	 *
	 * The same rule [forVideo] applies, and deliberately the only other place
	 * it is written down: a seekable file goes over either route, and one the
	 * server can only re-encode goes as HLS, which the bridge cannot carry. A
	 * second copy of that in the UI is exactly how a button and the thing it
	 * does come to disagree.
	 *
	 * It is cheap to ask: [CastReachability] memoises per server and network,
	 * so this costs a map lookup after the first call.
	 */
	suspend fun videoIsCastable(ref: ItemRef, nativeSeek: Boolean): Boolean {
		if (nativeSeek) return true
		val config = registry.get(ref.server) ?: return false
		return reachability.canReachDirectly(config)
	}

	/**
	 * The same decision for a video, which differs from audio in three ways.
	 *
	 * **Two tiers are offered, and which one is available depends on the
	 * route.** A file whose codec pair a browser takes arrives as a real MP4
	 * with a `Content-Length` that answers byte ranges — that is what
	 * `nativeSeek` names, and it is what the bridge already relays. Anything
	 * else the server can only re-encode, and its progressive answer is chunked
	 * with `Accept-Ranges: none`; the seekable form of it is `hls.m3u8`, where
	 * seeking is picking a segment.
	 *
	 * The playlist's segment URIs are **relative**, so they resolve against
	 * whatever base served the playlist. On the direct route that base is the
	 * gaindrive server's own `/rest/`, exactly as for any other client, and
	 * nothing else is needed — the server already sends the CORS headers a
	 * receiver requires for an adaptive stream. Through the bridge, whose
	 * grammar is a flat `/<token>/<key>`, a segment arrives as an unrecognised
	 * key and 404s. So HLS is refused *there and only there*, below the route
	 * decision rather than above it. See "What HLS would take" in `CAST.md`.
	 *
	 * What that refusal used to be is worth knowing, because it is the shape to
	 * avoid going back to: it sat at the top of this function, so it refused
	 * the direct route too — for a limitation belonging only to the relay —
	 * and direct is the common case. The symptom was a film with no cast icon
	 * and no explanation.
	 *
	 * Nothing about quality or bitrate is sent, for the reason set out on
	 * [StreamUrls.forVideo]: any `format` or `maxBitRate` sets `constrained`
	 * server-side and demotes a file that could have been served off disk
	 * untouched.
	 *
	 * And there is no stored-copy branch, because video never enters the byte
	 * cache — `GainDriveMediaSourceFactory` hands it the bare network factory —
	 * so looking would only ever miss.
	 *
	 * No [paced] here, unlike the audio route, because the server honours it
	 * for audio only: `serve_video` passes false at every tier, since a 15 s
	 * window sized for audio starves a player buffering a 6 Mbps film. Sending
	 * it would be a claim the server does not act on. A cast film therefore
	 * still has the shape the audio route was fixed for, and the fix would be a
	 * video-sized pacing window on the server, not this call.
	 */
	private suspend fun forVideo(ref: ItemRef, source: CastSource): CastTarget? {
		// No `playable`, and that omission is the load-bearing one on this
		// route. A receiver demuxes none of them, and [mime] below is
		// `transcodedContentType` — `video/mp4` for exactly the files declaring
		// would change. Adding the argument here announces MP4 and sends Matroska,
		// which a receiver refuses outright: the film never starts and nothing
		// anywhere says why. See `StreamUrls.forVideo`.
		//
		// The flag is passed through rather than pinned true, which is what
		// selects the playlist for a file the server can only re-encode.
		val target = streamUrls.forVideo(ref, source.nativeSeek) ?: return null
		val config = registry.get(ref.server) ?: return null

		// For a playlist the type is the playlist's, which only [target] knows.
		// Otherwise the server's own answer, not one worked out from the codecs:
		// `transcodedMime` is present exactly when the file will be remuxed, and
		// the source type is wrong in precisely that case.
		val mime = if (target.isHls) target.mimeType
		           else source.transcodedMime ?: source.sourceMime

		// Nothing to warm for a playlist: an HLS segment is transcoded per
		// request and no whole-file build blocks the first byte, so this would
		// fetch one byte of playlist text and warm nothing.
		if (!target.isHls) warmTranscode(target.url)

		// **A playlist keeps the ordinary credentials, and must.** The grant
		// does not cover `hls.m3u8`, and could not usefully: the playlist's
		// segment URIs are relative, so each segment is fetched with whatever
		// the playlist request carried, and the server has no way to hand a
		// per-song grant down to them. Tokening the playlist would authorise
		// the one document and leave every segment unauthorised, which is a
		// film that starts and immediately stops.
		if (reachability.canReachDirectly(config)) {
			if (target.isHls) return CastTarget(target.url, mime, CastRoute.DIRECT)
			val token = castToken(ref)
			return CastTarget(
				token?.let { withCastToken(target.url, it) } ?: target.url,
				mime,
				CastRoute.DIRECT,
				castToken = token,
			)
		}
		// See the note above: the bridge cannot resolve a playlist's relative
		// segment URIs, and publishing the playlist alone would hand the
		// receiver a document whose every entry 404s.
		if (target.isHls) {
			Log.i(TAG, "no direct route and this film is HLS-only; refusing")
			return null
		}
		val relayed = bridge.publish(target.url)
		if (relayed != null) return CastTarget(relayed, mime, CastRoute.RELAY)
		val token = castToken(ref)
		return CastTarget(
			token?.let { withCastToken(target.url, it) } ?: target.url,
			mime,
			CastRoute.DIRECT,
			castToken = token,
		)
	}

	/**
	 * Fetches one byte, so that a remux happens while the phone waits rather
	 * than while the receiver does.
	 *
	 * An H.264/AAC `.mkv` is not directly playable — the container is not one a
	 * browser takes — so the server remuxes it, and its transcode cache is
	 * *blocking*: nothing is sent until ffmpeg has written the whole file. For a
	 * multi-gigabyte film that is minutes, and a receiver that has gone that long
	 * without data gives up with a network error. Untreated, the first cast of
	 * every `.mkv` fails and the second works, which reads as a random fault.
	 *
	 * A one-byte range is enough to force the build, and costs nothing on a file
	 * the server serves directly. Failure is ignored on purpose: this is a warm,
	 * not a fetch, and if it went wrong the receiver's own request is still
	 * entitled to try.
	 */
	private suspend fun warmTranscode(url: String) {
		withContext(Dispatchers.IO) {
			runCatching {
				val request = Request.Builder().url(url).header("Range", "bytes=0-0").build()
				httpClient.newCall(request).execute().use { response ->
					Log.i(
						TAG,
						"warm ${response.code}" +
							" transcode=${response.header("X-Gaindrive-Transcode") ?: "none"}",
					)
				}
			}.onFailure { Log.w(TAG, "warm failed: $it") }
		}
	}

	/**
	 * The length of a complete copy held under [cacheKey], or null if there is
	 * not one that can be served.
	 *
	 * Both halves are required. A partial copy would play until the bytes ran
	 * out, and a copy of unknown length cannot be given a `Content-Length` or a
	 * ranged answer — which is most of what a receiver asks for.
	 */
	private suspend fun storedCopy(cacheKey: String): Long? =
		// Off the main thread: this walks the cache index, the same reason
		// StreamUrls.forPlayback does it.
		withContext(Dispatchers.IO) {
			audioCache.storedLength(cacheKey)?.takeIf { audioCache.isFullyCached(cacheKey) }
		}

	/**
	 * Artwork has to travel the same road as the audio. A receiver that cannot
	 * reach the server for one cannot reach it for the other, and a cast session
	 * showing a blank sleeve on the television looks broken.
	 *
	 * And the same credential, which is why [castToken] is threaded here rather
	 * than left to the audio alone: the sleeve goes to the receiver in the
	 * `LOAD`'s metadata and the receiver fetches it itself, so a token that
	 * stopped at the stream would have left the password on the television
	 * regardless. The grant covers the cover art of the song it was minted for.
	 */
	fun artworkFor(url: String?, bridged: Boolean, castToken: String? = null): String? {
		if (url == null) return null
		if (bridged) return bridge.publish(url) ?: url
		return castToken?.let { withCastToken(url, it) } ?: url
	}

	/**
	 * Subtitle tracks for the receiver, which fetches each one itself — so they
	 * take the same road as the picture, for the reason [artworkFor] gives.
	 *
	 * The trackId is the position in [configs] plus one, and that is the whole
	 * contract with `CastPlayer.captionTracks`: the two are built from the same
	 * ordered list so an index in the picker and a trackId on the wire cannot
	 * come to mean different things.
	 *
	 * Nothing is ever dropped, even when the bridge refuses to publish one.
	 * Dropping would renumber everything after it while `CastPlayer` went on
	 * numbering the full list, so a viewer choosing the third subtitle would
	 * silently turn on the fourth. An unbridgeable track falls back to the
	 * server URL — the same concession [artworkFor] makes — which at worst is
	 * one track that does not load, not a picker that lies.
	 */
	fun captionsFor(
		configs: List<MediaItem.SubtitleConfiguration>,
		bridged: Boolean,
		castToken: String? = null,
	): List<CastCaption> =
		configs.mapIndexed { i, config ->
			val source = config.uri.toString()
			CastCaption(
				trackId = i + 1,
				url = when {
					bridged -> bridge.publish(source) ?: source
					// One grant covers every caption of its song, unlike the
					// server's own cast token, which is scoped to the ids one
					// LOAD declared. There is no LOAD of the server's here to
					// scope against, and a subtitle of a track this account may
					// read is no wider a reach than the track.
					castToken != null -> withCastToken(source, castToken)
					else -> source
				},
				label = config.label ?: "Subtitles",
				language = config.language ?: "und",
			)
		}

	private companion object {
		const val TAG = "GainDriveCast"
	}
}

package org.gaindrive.android.data

import androidx.media3.common.MimeTypes
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.cache.CacheKeys
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClientFactory
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Where to fetch a track's audio from, at what quality, and under which cache
 * key.
 *
 * The three travel together on purpose. A URL naming one quality paired with a
 * key naming another stores bytes that will later be served to a request
 * expecting something else, and nothing downstream can detect it. Producing
 * them in one place is what makes that impossible.
 */
data class StreamTarget(
	val url: String,
	val quality: AudioQuality,
	val cacheKey: String,
	val mimeType: String?,
)

/**
 * Where to fetch a video from. No quality and no cache key, because both of
 * those are audio concepts here: the server chooses the video tier itself, and
 * video deliberately never enters the byte cache.
 */
data class VideoTarget(
	val url: String,
	val mimeType: String?,
	/** True when [url] is an HLS playlist rather than a progressive stream. */
	val isHls: Boolean,
)

/**
 * The query a progressive video request carries: the id, and the containers we
 * told the server we demux ourselves.
 *
 * Top-level and `internal` for the reason `CastUrls.paced()` is — it can then
 * be exercised with no Hilt and no `android.util`, which matters here because
 * the failure it guards against is silent and happens on a television. The
 * test that asserts an empty set yields exactly `{id}` is the one standing
 * between a refactor and every cast of an `.mkv` failing.
 *
 * Sorted rather than joined in set order: these end up in log lines and in
 * OkHttp's cache key, and a `Set`'s iteration order is not a promise, so one
 * request would otherwise be able to build two different URLs.
 */
internal fun videoStreamParams(id: String, containers: Set<String>): Map<String, String> =
	buildMap {
		put("id", id)
		if (containers.isNotEmpty())
			put("playable", containers.sorted().joinToString(","))
	}

/**
 * The query an audio request carries: the id, what to convert to, and what not
 * to convert at all.
 *
 * Hoisted out of [StreamUrls.build] for the reason [videoStreamParams] is —
 * `build` needs Hilt and this does not, and the assertion worth having is that
 * an empty [playable] yields no `playable` parameter whatsoever. That is the
 * audio twin of `the cast route declares nothing`: a receiver handed a URL that
 * declares gets the original while its `LOAD` announced the transcode's type,
 * and refuses the media outright.
 *
 * `format` and `maxBitRate` are omitted together for the original, because the
 * server serves the file directly with no ffmpeg involved at all. They are what
 * the declaration is *not*: these say what to produce, [playable] says what to
 * leave alone, and the two are independent.
 *
 * Sorted for the same reason the video list is: a `Set`'s iteration order is not
 * a promise, and this string lands in OkHttp's cache key and the server's log.
 */
internal fun audioStreamParams(
	id: String,
	quality: AudioQuality,
	playable: Set<String>,
): Map<String, String> =
	buildMap {
		put("id", id)
		if (quality.format != AudioFormat.ORIGINAL) {
			put("format", quality.format.param)
			put("maxBitRate", quality.bitRate.toString())
		}
		if (playable.isNotEmpty())
			put("playable", playable.sorted().joinToString(","))
	}

/**
 * What a request may declare it takes as it stands, given the quality that
 * request settled on.
 *
 * A function rather than a set, because the set follows from a quality the
 * caller does not have: `forPlayback` may build for a copy already held rather
 * than for the current setting, and the declaration has to match whatever it
 * lands on. A function rather than a flag, because [StreamUrls] must not be the
 * thing that knows — what media3 decodes is a claim about a player, and the
 * same argument that keeps `MEDIA3_CONTAINERS` out of this file keeps its audio
 * counterpart out too. `PlaybackService` passes `::playableAudioFor`.
 *
 * [DECLARES_NOTHING] is the default, and the default is the safety. It covers
 * every caller that has not thought about it, which is the property that
 * matters: a Cast receiver demuxes none of this and was told in advance what it
 * was about to be sent.
 */
typealias PlayableFor = (AudioQuality) -> Set<String>

/** Declares nothing at all; see [PlayableFor]. */
val DECLARES_NOTHING: PlayableFor = { emptySet() }

/**
 * The single builder of stream URLs, used by both the download queue and the
 * player.
 *
 * Deliberately not reached through `LibraryRepository`: that class writes the
 * mirror `PinRepository` reads, and a dependency back would close the loop —
 * the same reasoning that kept `PinRepository` from using `CoverUrls`.
 */
@Singleton
class StreamUrls @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
	private val settings: SettingsStore,
	private val accounts: Accounts,
	private val audioCache: AudioCache,
) {

	/**
	 * Always the current setting: pinning is the user asking for this track at
	 * the quality they have chosen, so an older copy does not satisfy it.
	 */
	suspend fun forDownload(
		ref: ItemRef,
		audioOnlyVideo: Boolean = false,
		playable: PlayableFor = DECLARES_NOTHING,
	): StreamTarget? =
		build(ref, settings.audioQuality.first(), audioOnlyVideo, playable)

	/**
	 * Prefers a quality already held in full, so a library downloaded at an
	 * earlier setting keeps playing from disk instead of being re-fetched.
	 *
	 * The URL is built for the held quality rather than the current one: if the
	 * copy turns out to need topping up, the bytes that arrive have to match the
	 * bytes already there.
	 *
	 * [playable] decides what the request may declare it takes as it stands,
	 * and the default declares nothing. **This function serves the Cast route as
	 * well as the local player** — `CastUrls.directTarget` reaches the server
	 * through it in three of its branches — so the declaration cannot live in
	 * here, only at the call site that will read the bytes itself.
	 */
	suspend fun forPlayback(
		ref: ItemRef,
		audioOnlyVideo: Boolean = false,
		playable: PlayableFor = DECLARES_NOTHING,
	): StreamTarget? {
		val preferred = settings.audioQuality.first().let {
			if (audioOnlyVideo) it.forVideoAudio() else it
		}
		// Off the main thread: this walks the cache index, and onAddMediaItems
		// runs on the player's thread.
		val held = withContext(Dispatchers.IO) {
			audioCache.heldTagOf(ref.encode(), preferred)
		}?.let(AudioQuality::parse)
		return build(ref, held ?: preferred, audioOnlyVideo, playable)
	}

	/**
	 * The file as it stands, for a Cast receiver that fetches from the server
	 * itself. See `SettingsStore.castOriginal` for why that route alone.
	 *
	 * Deliberately does **not** prefer a quality already held, the way
	 * [forPlayback] does: a copy stored at the streaming quality is the answer
	 * to a different question here, and preferring it would defeat the setting
	 * on precisely the tracks the user listens to most.
	 *
	 * The account ceiling still applies — `AudioQuality.cappedBy` turns a
	 * request for the original into mp3 at the cap — because the server would
	 * enforce it whatever was asked for. So "original" means "as far as the
	 * account allows", which the track info dialog's `Sent` row makes visible.
	 *
	 * The returned `cacheKey` names bytes nobody will store: the receiver
	 * fetches this URL, not the phone. It is carried only because
	 * [StreamTarget] travels as one value, and the direct cast path returns
	 * without ever reading it.
	 */
	suspend fun forCastOriginal(ref: ItemRef): StreamTarget? =
		build(ref, AudioQuality.ORIGINAL)

	/**
	 * Where to fetch a video from, and how it will have to be seeked.
	 *
	 * Two parameters are conspicuously absent, and both omissions are
	 * load-bearing:
	 *
	 *  - **`format`** is validated against the *audio* target table, so a video
	 *    container name is rejected outright and an audio one asks the server
	 *    for the soundtrack alone. That second behaviour is now a feature —
	 *    `SettingsStore.videoAudioOnly` — but it is reached by resolving the
	 *    item through the *audio* path instead, never from here. This builder
	 *    is for someone who wants the picture, and a format here would take it
	 *    away. `web/app.js` splits the same two cases the same way.
	 *  - **`maxBitRate`** sets `constrained` server-side, which forces the
	 *    re-encode tier and demotes a file that could have been served straight
	 *    off disk. The account ceiling is applied by the server regardless of
	 *    what is asked for, so sending one buys nothing and costs the tier.
	 *
	 * [nativeSeek] comes from the entry the caller already has. False means the
	 * server can only re-encode this file, which is chunked with no
	 * `Content-Length` and no `Range` — unseekable as a progressive stream, so
	 * it is played as HLS, where seeking is picking a segment.
	 *
	 * [playable] is a third parameter that is *usually* absent, and the default
	 * is the safety. It tells the server we demux those containers ourselves —
	 * the video half of one parameter that also carries the audio declaration,
	 * see [audioStreamParams] — so it can skip a remux it would otherwise pay.
	 * But the same URL builder serves the Cast route, and a receiver demuxes
	 * none of them. Worse,
	 * the `LOAD` sent to that receiver declared a `contentType` taken from the
	 * entry's `transcodedContentType`, which is `video/mp4` for exactly the files
	 * this affects; a receiver told `video/mp4` and handed Matroska refuses the
	 * media outright, which reads as a broken file rather than a mislabelled one.
	 *
	 * So it is passed at the call site that plays locally and nowhere else,
	 * exactly as `CastUrls.paced()` is applied at the route rather than here.
	 * Empty on the HLS branch too: every segment carries a `duration`, which
	 * makes it an encode whatever the container holds, so declaring there would
	 * be a claim the server cannot act on.
	 */
	suspend fun forVideo(
		ref: ItemRef,
		nativeSeek: Boolean,
		playable: Set<String> = emptySet(),
	): VideoTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			val client = clients.clientFor(config)

			if (nativeSeek) {
				VideoTarget(
					url = client.url(
						"stream",
						videoStreamParams(ref.id, playable),
					),
					// No declared type: sniffing is the only honest answer, the
					// same argument AudioFormat.ORIGINAL makes. The remux tier
					// turns an .mkv into MP4, and a VP9/Opus .mkv is served
					// relabelled video/webm — so the entry's own contentType is
					// wrong in exactly the cases that matter.
					mimeType = null,
					isHls = false,
				)
			} else {
				VideoTarget(
					// The playlist copies this request's auth parameters onto
					// every segment URL, so the player needs no context from
					// here to fetch them.
					url = client.url("hls.m3u8", mapOf("id" to ref.id), suffix = ""),
					mimeType = MimeTypes.APPLICATION_M3U8,
					isHls = true,
				)
			}
		}

	/**
	 * [audioOnlyVideo] says [ref] names a video and only its soundtrack is
	 * wanted. It changes exactly one thing — [AudioQuality.ORIGINAL] cannot be
	 * expressed for a video, since it is spelled by sending no `format` and
	 * that fetches the film — but it has to be threaded all the way here
	 * because the cache key is derived from the same quality value.
	 */
	private suspend fun build(
		ref: ItemRef,
		wanted: AudioQuality,
		audioOnlyVideo: Boolean = false,
		playable: PlayableFor = DECLARES_NOTHING,
	): StreamTarget? =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext null
			// Named rather than folded into the line below, because the
			// declaration reads this value and the capped one would be wrong:
			// a capped account asking for the original is sent mp3 at the cap,
			// so `quality` has already lost the fact that originals were wanted.
			val asked = if (audioOnlyVideo) wanted.forVideoAudio() else wanted
			// The account ceiling is applied here, where this track's own server
			// is in hand. A queue may span servers, so there is no single
			// "current" cap to read.
			val quality = asked.cappedBy(accounts.capFor(config))

			// Nothing for a video played as audio, and not merely because it
			// would be ignored: `plan_transcode` guards the audio declaration on
			// `!song.is_video`, and a parameter the server drops on the floor
			// invites the next reader to believe it does something.
			val declared: Set<String> =
				if (audioOnlyVideo) emptySet() else playable(asked)

			StreamTarget(
				url = clients.clientFor(config).url(
					"stream",
					audioStreamParams(ref.id, quality, declared),
				),
				quality = quality,
				cacheKey = CacheKeys.of(ref, quality),
				// Null the moment anything was declared: the response may be the
				// file as it stands rather than the format asked for, and this
				// value reaches ExoPlayer, the download index and — where it
				// cannot be sniffed away — a Cast receiver. Sniffing is then the
				// only honest answer, the same argument AudioFormat.ORIGINAL
				// already makes for itself.
				mimeType = if (declared.isEmpty()) quality.format.mime else null,
			)
		}
}

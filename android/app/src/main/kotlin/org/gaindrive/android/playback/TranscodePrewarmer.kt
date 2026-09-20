package org.gaindrive.android.playback

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.withContext
import okhttp3.OkHttpClient
import okhttp3.Request
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.cache.AudioCache
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.di.MediaHttp
import java.util.Collections
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Asks the server to build a track's transcode before anyone waits on it.
 *
 * gaindrive transcodes a whole track to a file before sending any of it, which
 * buys a real Content-Length and byte ranges but costs a few seconds on the
 * first request for a given track and quality. Landing that wait on the moment
 * the user tapped play is the worst possible place for it; doing it for the
 * *next* track while the current one plays moves it somewhere nobody is looking.
 *
 * Nothing here touches the audio cache. The response is thrown away — the point
 * is the work the server does on the way to producing it.
 */
@Singleton
class TranscodePrewarmer @Inject constructor(
	private val streamUrls: StreamUrls,
	private val audioCache: AudioCache,
	private val settings: SettingsStore,
	// The point of the warm is to wait out a transcode, so it must be the
	// client that is allowed to; see MediaHttp.
	@MediaHttp private val httpClient: OkHttpClient,
) {

	/**
	 * Cache keys warmed, or being warmed, this process.
	 *
	 * Transitions can fire more than once for the same track — a repeat, a seek
	 * back across a boundary — and a second request would be wasted even though
	 * the server would answer it from its own cache. Synchronised because
	 * [warm] can be entered concurrently for different tracks.
	 */
	private val attempted = Collections.synchronizedSet(mutableSetOf<String>())

	/**
	 * [audioOnlyVideo] says this ref is a video whose soundtrack is what will
	 * be fetched. It is the case this class matters most for: extracting a
	 * film's audio is a blocking transcode over a multi-gigabyte source, so the
	 * wait it moves out of the user's way is a minute rather than a few seconds.
	 */
	suspend fun warm(ref: ItemRef, audioOnlyVideo: Boolean = false) {
		// Offline mode means requests are not to be made at all, not that they
		// are expected to fail.
		if (settings.offlineMode.first()) return
		// Already on the device at some quality: playback will read it from the
		// cache, so there is nothing for the server to prepare.
		if (ref.encode() in audioCache.cachedKeys.value) return

		// The same declaration playback will make, and it has to be: warming an
		// undeclared URL builds an Opus transcode that playback then never
		// fetches, so the server pays for the file twice and the cache entry it
		// wrote is never read.
		val target = streamUrls.forPlayback(
			ref,
			audioOnlyVideo,
			::playableAudioFor,
		) ?: return
		// The original is served straight off disk with no ffmpeg involved, so
		// there is no transcode to build and nothing to wait for. A declared
		// format the server passes through costs the same nothing, but it
		// cannot be recognised from here — whether a `.m4a` holds AAC or ALAC
		// is not on a song entry — so that request goes out and finds a 206
		// waiting for it.
		if (target.quality.format == AudioFormat.ORIGINAL) return

		// Claimed only now that a request is actually going out, and keyed by
		// the *cache key* rather than the ref: what was warmed is a track at a
		// quality, so changing the quality should warm it again. Marking it
		// before the checks above would instead make a track skipped for a
		// passing reason permanently ineligible.
		val claim = target.cacheKey
		if (!attempted.add(claim)) return

		try {
			// One byte is enough: Streamer::serve runs the transcode to
			// completion before it reaches the code that honours the range, so
			// the cache is warm however little of the response we ask for.
			withContext(Dispatchers.IO) {
				val request = Request.Builder()
					.url(target.url)
					.header("Range", "bytes=0-0")
					.build()
				httpClient.newCall(request).execute().use { it.body.bytes() }
			}
		} catch (e: CancellationException) {
			attempted.remove(claim)
			throw e
		} catch (e: Exception) {
			// Nothing here is worth interrupting playback for: the only cost of
			// failing is that the stall happens when the track is reached,
			// which is what used to happen anyway. Dropped from the set so a
			// track missed while the network was down can be warmed later.
			attempted.remove(claim)
		}
	}
}

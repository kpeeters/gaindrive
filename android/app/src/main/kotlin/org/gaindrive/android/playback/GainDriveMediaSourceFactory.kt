package org.gaindrive.android.playback

import androidx.media3.common.MediaItem
import androidx.media3.datasource.DataSource
import androidx.media3.exoplayer.drm.DrmSessionManagerProvider
import androidx.media3.exoplayer.source.DefaultMediaSourceFactory
import androidx.media3.exoplayer.source.MediaSource
import androidx.media3.exoplayer.upstream.LoadErrorHandlingPolicy
import androidx.media3.extractor.text.SubtitleParser

/**
 * Picks the loading path for an item: audio through the byte cache, video
 * straight from the network.
 *
 * **Video must not reach the cache.** It is sized in tens of megabytes for
 * tracks, so a single film would evict the whole stored library on its way past;
 * and a video the server can only re-encode arrives with no `Content-Length` at
 * all, so `AudioCache.isFullyCached` could never call it complete and the
 * evictor would churn the partial spans forever. Neither is a tuning problem -
 * caching video simply does not mean anything here.
 *
 * Both branches are [DefaultMediaSourceFactory], which reads the item's MIME
 * type and picks progressive or HLS itself. That is the whole HLS integration:
 * with `media3-exoplayer-hls` on the classpath there is no second code path to
 * write, and side-loaded subtitle configurations are merged onto either kind.
 */
class GainDriveMediaSourceFactory(
	cached: DataSource.Factory,
	direct: DataSource.Factory,
) : MediaSource.Factory {

	private val audio = DefaultMediaSourceFactory(cached)
	private val video = DefaultMediaSourceFactory(direct)

	private fun factoryFor(mediaItem: MediaItem) =
		if (mediaItem.isVideo()) video else audio

	override fun createMediaSource(mediaItem: MediaItem): MediaSource =
		factoryFor(mediaItem).createMediaSource(mediaItem)

	override fun getSupportedTypes(): IntArray = audio.supportedTypes

	// Configuration applies to both, so neither can be left behind holding an
	// older policy than the player thinks it set.

	override fun setDrmSessionManagerProvider(
		provider: DrmSessionManagerProvider,
	): MediaSource.Factory = apply {
		audio.setDrmSessionManagerProvider(provider)
		video.setDrmSessionManagerProvider(provider)
	}

	override fun setLoadErrorHandlingPolicy(
		policy: LoadErrorHandlingPolicy,
	): MediaSource.Factory = apply {
		audio.setLoadErrorHandlingPolicy(policy)
		video.setLoadErrorHandlingPolicy(policy)
	}

	/**
	 * Forwarded rather than left to the interface's do-nothing default: this is
	 * what parses the side-loaded WebVTT a video carries, and the video branch
	 * is precisely the one that would have been skipped.
	 */
	override fun setSubtitleParserFactory(
		subtitleParserFactory: SubtitleParser.Factory,
	): MediaSource.Factory = apply {
		audio.setSubtitleParserFactory(subtitleParserFactory)
		video.setSubtitleParserFactory(subtitleParserFactory)
	}
}

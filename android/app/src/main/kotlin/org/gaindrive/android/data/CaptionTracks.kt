package org.gaindrive.android.data

import android.net.Uri
import android.util.Log
import androidx.media3.common.C
import androidx.media3.common.MediaItem
import androidx.media3.common.MimeTypes
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.requireOk
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The subtitle tracks a video offers, as side-loaded configurations ExoPlayer
 * can merge onto the stream.
 *
 * The captions themselves are never fetched here. `getCaptions` answers with
 * raw WebVTT rather than a Subsonic envelope, so the honest thing is to hand
 * the player a URL and let it load the track if and when the user selects it —
 * which also means a subtitle file that is slow to convert costs nothing until
 * it is wanted.
 *
 * Both the listing call and the caption URLs go through this server's own
 * client, so the same auth parameters ride along as for cover art and streams.
 */
@Singleton
class CaptionTracks @Inject constructor(
	private val registry: ServerRegistry,
	private val clients: SubsonicClientFactory,
) {

	/**
	 * Never throws and never returns null: a video with no captions and a
	 * `getVideoInfo` that failed are the same thing to a player, and neither is
	 * a reason to refuse to show the film. The call costs an `ffprobe` on the
	 * server, so it belongs on the load path and nowhere else.
	 *
	 * [sidecarOnly] says the player is demuxing the original container rather
	 * than being handed a conversion of it, so it will surface the embedded
	 * subtitle tracks by itself and side-loading them here would list every one
	 * of them twice. The sidecar `.srt` is the half no container carries, so it
	 * still has to come from the server whatever the tier.
	 *
	 * It is false for a remux or a re-encode, and must be: both keep the first
	 * video and audio stream and drop everything else, so there the server's
	 * conversion is the *only* way an embedded track reaches the player.
	 */
	suspend fun configurationsFor(
		ref: ItemRef,
		sidecarOnly: Boolean = false,
	): List<MediaItem.SubtitleConfiguration> =
		withContext(Dispatchers.IO) {
			val config = registry.get(ref.server) ?: return@withContext emptyList()
			val client = clients.clientFor(config)

			val all = runCatching {
				client.getVideoInfo(ref.id).requireOk().videoInfo?.captions
			}.getOrNull().orEmpty()

			// Written as "not container" rather than "is sidecar" so an absent or
			// unrecognised value errs towards *offering* the track: a duplicate
			// entry in the picker is visible and one tap to undo, a subtitle file
			// that silently stopped being listed is neither.
			//
			// That is also what makes an older server safe rather than lucky. One
			// that does not report `source` does not honour `playableContainers`
			// either, so it remuxes — and a remux drops the embedded tracks, which
			// is exactly when every caption has to be side-loaded. The two halves
			// arrived together and degrade together.
			val captions =
				if (sidecarOnly) all.filter { it.source != CONTAINER } else all

			// Ids only, never the URL: it carries the account's password. This
			// is the one place that says whether a track in the picker was
			// side-loaded from here or came out of the container itself.
			Log.d(TAG, "getVideoInfo for ${ref.id}: captions=" +
				captions.joinToString { it.id } +
				(if (sidecarOnly) " (sidecars only, of ${all.size})" else ""))

			captions.map { caption ->
				val url = client.url(
					"getCaptions",
					mapOf("id" to ref.id, "captionId" to caption.id),
				)
				MediaItem.SubtitleConfiguration.Builder(Uri.parse(url))
					// Always WebVTT: the server normalises every source format
					// — embedded streams, .srt, .ass sidecars — through ffmpeg
					// before it answers.
					.setMimeType(MimeTypes.TEXT_VTT)
					.setLabel(caption.name.ifBlank { "Subtitles" })
					// Not SELECTION_FLAG_DEFAULT: nothing here knows the
					// viewer's language, and subtitles nobody asked for are
					// more intrusive than subtitles one tap away.
					.setSelectionFlags(0)
					.setRoleFlags(C.ROLE_FLAG_SUBTITLE)
					.build()
			}
		}

	private companion object {
		const val TAG = "GainDriveVideo"

		/** `getVideoInfo`'s word for a track inside the file. */
		const val CONTAINER = "container"
	}
}

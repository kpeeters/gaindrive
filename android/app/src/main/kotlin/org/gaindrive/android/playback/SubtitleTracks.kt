package org.gaindrive.android.playback

import androidx.media3.common.C
import androidx.media3.common.MimeTypes
import androidx.media3.common.Tracks

/**
 * The subtitle tracks worth offering, in the order the picker numbers them.
 *
 * Shared by [PlayerConnection], which labels them, and [VideoSurface], which
 * turns one on. They filter *different copies* of the same list — one the
 * controller's, one the player's — so a predicate that differed between them
 * would quietly select the wrong track, or none.
 *
 * **CEA-608 and CEA-708 are dropped, and that is the point of this function.**
 * `DefaultHlsExtractorFactory` exposes a CEA-608 track whenever an mpegts HLS
 * playlist declares no closed captions at all — its
 * `exposeCea608WhenMissingDeclarations` flag, which defaults to true — and
 * `hls.m3u8` declares none. The invented track has no label, no language and
 * no data, so before this the re-encode tier put a subtitle picker on *every*
 * film offering one nameless entry that could never produce a cue. Selecting
 * it ticks, tints the icon, and does nothing, which is a worse answer than no
 * button.
 *
 * The cost is a genuine A53 caption carried through the encode: `libx264`
 * passes those on by default, so an MPEG-2 source that had them would now be
 * filtered out along with the phantoms. That is accepted rather than
 * overlooked — nothing distinguishes the two before the stream is read, the
 * files that reach the encode tier are largely codecs that cannot carry A53
 * captions at all, and the alternative is the dead button on everything.
 */
internal fun Tracks.subtitleGroups(): List<Tracks.Group> =
	groups.filter {
		it.type == C.TRACK_TYPE_TEXT &&
			it.mediaTrackGroup.getFormat(0).sampleMimeType !in INVENTED_CAPTION_MIMES
	}

private val INVENTED_CAPTION_MIMES =
	setOf(MimeTypes.APPLICATION_CEA608, MimeTypes.APPLICATION_CEA708)

package org.gaindrive.android.playback

/**
 * The video containers media3 demuxes for itself, which is more than a browser
 * does — and the server has no way to know that unless it is told.
 *
 * `stream.view` picks its tier from `browser_container()` in `src/codecs.hh`:
 * `mp4`, `m4v`, `webm` and nothing else. Everything else carrying codecs a
 * browser *would* take — an H.264/AAC `.mkv` above all, which is what yt-dlp
 * writes and what most TV rips are — is remuxed to MP4 with `-c copy`. That
 * costs nothing in quality and a great deal in time: the transcode cache is
 * blocking, so nothing is sent until ffmpeg has written the whole file, and
 * there is no prewarm on the local video path the way there is for a cast.
 * Declaring the container skips the remux entirely.
 *
 * **Only containers, never codecs.** The server still applies its own codec
 * test, so an HEVC or AC3 file is unaffected by anything here — those are
 * re-encoded, and asking for them would be asking for bytes no tier produces.
 * That restriction is what makes this safe to send per request: the two tiers
 * it moves a file between are both natively seekable, so `nativeSeek` — which
 * decides the transport and whether the file may be cast at all — means the
 * same thing either way.
 *
 * `wmv` is absent because media3 has no ASF extractor. `mpg`/`mpeg` are absent
 * because MPEG-PS essentially always carries MPEG-2, which the server's codec
 * test rejects, so declaring it would change nothing. `vob` is absent and the
 * server refuses it regardless: a DVD titleset is one stream split across
 * numbered VOBs and the stored path names only the first.
 *
 * Top-level for the same reason `SubtitleTracks` beside it is: one media3
 * fact, in the package whose player makes it true, exercisable without Hilt or
 * `android.util`. Not in `StreamUrls` — that builds URLs and has no business
 * holding a claim about a player.
 */
internal val MEDIA3_CONTAINERS = setOf("mkv", "mov", "avi")

/**
 * Whether the server will hand us [suffix] untouched if we declare it, so the
 * bytes arriving are the original container rather than a remux.
 *
 * The caller must also know the file is `nativeSeek`: without it the server
 * can only re-encode, and no declaration changes that.
 *
 * This is what decides whether the container's own subtitle tracks will show
 * up in the picker — see `CaptionTracks.configurationsFor`.
 */
internal fun demuxedLocally(suffix: String?): Boolean =
	suffix?.lowercase() in MEDIA3_CONTAINERS

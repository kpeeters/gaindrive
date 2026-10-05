package org.gaindrive.android.playback

/**
 * What this device can play as it stands, for `playable` on `stream.view`.
 *
 * The audio counterpart of [MEDIA3_CONTAINERS], and a separate file's worth of
 * reasoning because the question is not the same one. Video declares a
 * *container* and leaves the codec test to the server; here the client supplies
 * both halves, because a `.m4a` is AAC or ALAC and a `.ogg` is Vorbis, Opus,
 * FLAC or Speex, and nothing outside the file says which.
 *
 * **Every audio token is a `container/codec` pair**, matched against the
 * container and codec the server's scan *observed* rather than against a
 * filename. That is why there is no bare `mp3`: it would be a second spelling
 * of `mpeg/mp3`, and two spellings of one thing is exactly the bug that made a
 * `.oga` and a `.ogg` fail to agree they were both Ogg. Containers are the real
 * ones - `ogg`, `mp4`, `mpeg` - so one token covers every extension a container
 * is written under.
 *
 * **A capability, declared whatever the quality setting.** Size is the job of
 * `maxBitRate`: the server sends a declared file untouched only when it fits
 * under that ceiling, so declaring FLAC at "up to 160 kbps" costs nothing, and
 * with no ceiling a file that is not declared arrives as FLAC rather than as a
 * second lossy generation. `format` still says what to produce when an encode
 * cannot be avoided, which is why `mp3` can be declared without ever asking the
 * server to *encode* MP3, and why the set has no useful order.
 *
 * Absences, all deliberate:
 *
 *  - `ogg/speex` - media3 has no Speex decoder, so claiming it would be silence.
 *  - `wav` and `wma` - the server refuses both in any form. A `.wav` can hold
 *    ADPCM or mu-law and a `.wma` spans four codecs, and neither is worth
 *    teaching the scanner to read. With no ceiling such a file arrives as FLAC.
 *  - Any bare audio token. The server reads a bare token as a *video*
 *    container, so one here would silently name nothing.
 *
 * Together these come to 73 characters, against the server's 128-character
 * bound on the whole parameter. Video declares separately and is nowhere near
 * it.
 *
 * **Changing this set needs the byte cache cleared.** A track part-cached
 * under the old set has its remainder fetched under the new one, and media3
 * splices the two under one key - the key names the quality *asked for*, which
 * no longer decides what arrives. There is nothing in the cache that would
 * detect it and nothing on the phone that would report it.
 *
 * Top-level and `internal` for the reason [MEDIA3_CONTAINERS] is: one media3
 * fact, in the package whose player makes it true, exercisable with no Hilt and
 * no `android.util`.
 */
internal val MEDIA3_AUDIO = setOf(
	"mpeg/mp3", "adts/aac", "mp4/aac",
	// One token each, covering `.ogg`, `.oga` and `.opus` alike: the server
	// stores the container it saw, and all three are Ogg.
	"ogg/vorbis", "ogg/opus",
	"flac/flac", "mp4/alac", "ogg/flac",
)

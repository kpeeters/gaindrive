package org.gaindrive.android.playback

import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality

/**
 * What this device will take as it stands, for `playable` on `stream.view`.
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
 * ones — `ogg`, `mp4`, `mpeg` — so one token covers every extension a container
 * is written under.
 *
 * **A token is not "I can decode this". It is "this one is acceptable as it
 * stands".** The set says which files to leave alone; what to produce for
 * everything else is `format`, which stays Opus at the chosen bitrate. That is
 * why `mp3` can be declared without ever asking the server to *encode* MP3 —
 * Opus is better at the same rate — and why the set has no useful order: the
 * server weighs it against one candidate, the file itself.
 *
 * The consequence, and the reason [playableAudioFor] takes a quality at all: a
 * client set to Opus 160 that declares `flac` is handed FLAC whenever the file
 * is one, which is the opposite of what the setting asked for. The rule is
 * therefore **never declare a codec that returns more bytes than the setting
 * asked for**, not a special case for the word `flac` — ALAC and FLAC-in-Ogg
 * are lossless in exactly the same way, and would be exactly the same mistake.
 *
 * Absences, all deliberate:
 *
 *  - `ogg/speex` — media3 has no Speex decoder, so claiming it would be silence.
 *  - `wav` and `wma` — the server refuses both in any form. A `.wav` can hold
 *    ADPCM or mu-law and a `.wma` spans four codecs, and neither is worth
 *    teaching the scanner to read: someone who wants a lossless or legacy file
 *    untouched asks for the original, which sends no `format` at all.
 *  - Any bare audio token. The server reads a bare token as a *video*
 *    container, so one here would silently name nothing.
 *
 * Together these come to 73 characters at Original, against the server's
 * 128-character bound on the whole parameter. Video declares separately and is
 * nowhere near it.
 *
 * **Changing either set needs the byte cache cleared.** A track part-cached
 * under the old set has its remainder fetched under the new one, and media3
 * splices the two under one key — the key names the quality *asked for*, which
 * no longer decides what arrives. There is nothing in the cache that would
 * detect it and nothing on the phone that would report it.
 *
 * Top-level and `internal` for the reason [MEDIA3_CONTAINERS] is: one media3
 * fact, in the package whose player makes it true, exercisable with no Hilt and
 * no `android.util`.
 */
internal val MEDIA3_AUDIO_LOSSY = setOf(
	"mpeg/mp3", "adts/aac", "mp4/aac",
	// One token each, covering `.ogg`, `.oga` and `.opus` alike: the server
	// stores the container it saw, and all three are Ogg.
	"ogg/vorbis", "ogg/opus",
)

/**
 * The lossless half, declared only when the setting is Original — see
 * [MEDIA3_AUDIO_LOSSY] for why that gate exists rather than a blanket
 * "everything media3 decodes".
 */
internal val MEDIA3_AUDIO_LOSSLESS =
	setOf("flac/flac", "mp4/alac", "ogg/flac")

/**
 * What to declare for a request that asked for [asked].
 *
 * [asked] is the quality **before** `AudioQuality.cappedBy`, and that is
 * load-bearing: a capped account asking for the original is sent mp3 at the
 * cap, so the capped value has already lost the fact that originals were what
 * was wanted. It is **after** `AudioQuality.forVideoAudio`, which is equally
 * load-bearing in the other direction: "the original" for a film played as
 * audio is the film, so that substitution must have happened first.
 */
internal fun playableAudioFor(asked: AudioQuality): Set<String> =
	if (asked.format == AudioFormat.ORIGINAL)
		MEDIA3_AUDIO_LOSSY + MEDIA3_AUDIO_LOSSLESS
	else MEDIA3_AUDIO_LOSSY

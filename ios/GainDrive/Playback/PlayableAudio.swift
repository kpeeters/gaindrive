//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What this device will take exactly as the server holds it, for `playable` on
/// `stream.view`.
///
/// The audio counterpart of `avfoundationContainers`, and a separate file's
/// worth of reasoning because the question is not the same one. Video declares
/// a *container* and leaves the codec test to the server; here the client
/// supplies both halves.
///
/// **Every token is a `container/codec` pair**, compared against the container
/// and codec the server's scan *observed* - `songs.audio_container` and
/// `songs.audio_codec`, never the filename. That is why there is no bare
/// `mp3`: the server reads a bare token as a *video* container, and `mp3`
/// beside `mpeg/mp3` would be two spellings of one thing. Containers are the
/// real ones, so `mp4/` reaches a `.m4a` and `ogg/` would reach `.ogg`, `.oga`
/// and `.opus` alike.
///
/// **A token means "this one is acceptable as it stands", not "I can decode
/// this".** They are different claims and only the second is about the
/// framework. AVFoundation decodes FLAC perfectly well; declaring it at AAC 160
/// would have the server send a lossless file over mobile data, which is the
/// opposite of what that setting asks for. Hence the split below: never declare
/// a codec that returns more bytes than the setting asked for.
///
/// It never names an output. `format` and `maxBitRate` still say what to
/// produce for everything not declared, so the set is unordered and `mpeg/mp3`
/// can be declared without ever asking the server to *encode* MP3.
///
/// **No `ogg/` token of any kind**, and that is the one place this diverges
/// from Android rather than merely restating it. Apple ships no Ogg demuxer, as
/// `AudioFormat` already records - declaring Vorbis or Opus here would be
/// asking for silence, which is exactly the failure the enum's missing cases
/// exist to prevent.
///
/// Top-level for the reason `avfoundationContainers` is: one framework fact, in
/// the package whose player makes it true, exercisable without building an app.
let avfoundationAudioLossy: Set<String> = ["mpeg/mp3", "adts/aac", "mp4/aac"]

/// The lossless half, declared only for `.original` - see
/// `avfoundationAudioLossy` for why that gate exists rather than a blanket
/// "everything AVFoundation decodes".
///
/// FLAC has been decodable since iOS 11 and ALAC since long before, so both are
/// genuinely playable; the gate is about size, not capability.
let avfoundationAudioLossless: Set<String> = ["flac/flac", "mp4/alac"]

/// What to declare for a request that asked for `wanted`.
///
/// `wanted` is the quality **before** `AudioQuality.cappedBy`, and that is
/// load-bearing: a capped account asking for the original is sent mp3 at the
/// cap, so the capped value has already lost the fact that originals were what
/// was wanted.
func avfoundationPlayable(for wanted: AudioQuality) -> Set<String> {
	wanted.format == .original
		? avfoundationAudioLossy.union(avfoundationAudioLossless)
		: avfoundationAudioLossy
}

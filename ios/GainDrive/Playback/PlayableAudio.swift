//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What this device can play exactly as the server holds it, for `playable` on
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
/// **A capability, declared whatever the quality setting.** Size is the job of
/// `maxBitRate`: the server sends a declared file untouched only when it fits
/// under that ceiling, so declaring FLAC at "up to 160 kbps" costs nothing, and
/// with no ceiling a file that is not declared arrives as FLAC rather than as
/// a second lossy generation. `format` and `maxBitRate` still say what to
/// produce when an encode cannot be avoided, so the set is unordered and
/// `mpeg/mp3` can be declared without ever asking the server to *encode* MP3.
///
/// **No `ogg/` token of any kind**, and that is the one place this diverges
/// from Android rather than merely restating it. Apple ships no Ogg demuxer, as
/// `AudioFormat` already records - declaring Vorbis or Opus here would be
/// asking for silence. FLAC has been decodable since iOS 11 and ALAC since long
/// before.
///
/// Top-level for the reason `avfoundationContainers` is: one framework fact, in
/// the package whose player makes it true, exercisable without building an app.
let avfoundationAudio: Set<String> = [
	"mpeg/mp3", "adts/aac", "mp4/aac", "flac/flac", "mp4/alac",
]

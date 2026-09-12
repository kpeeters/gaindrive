//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The video containers AVFoundation demuxes for itself, which is more than a
/// browser does — and the server has no way to know that unless it is told.
///
/// `stream.view` picks its tier from `browser_container()` in `src/codecs.hh`:
/// `mp4`, `m4v`, `webm` and nothing else. So an H.264/AAC `.mov` — QuickTime,
/// which AVFoundation was built on — is remuxed to MP4 for no reason at all,
/// and the wait is not free: the server's transcode cache is blocking, so
/// nothing is sent until ffmpeg has copied the whole file. Declaring the
/// container skips it outright.
///
/// **`mov` alone, and the omissions are the interesting part.** `mp4` and
/// `m4v` are already served untouched to everyone, so naming them would say
/// nothing. `mkv` and `webm` are Matroska, which AVFoundation cannot demux at
/// all — those genuinely need the remux, and claiming them would trade a wait
/// for a film that does not play. `avi`, `mpg`, `mpeg` and `wmv` likewise.
/// `vob` is refused server-side regardless: a DVD titleset is one stream split
/// across numbered VOBs and the stored path names only the first.
///
/// **Only containers, never codecs.** The server still applies its own codec
/// test, so nothing here can ask for bytes no tier produces. That restriction
/// is what makes the declaration safe to send per request: the two tiers it
/// moves a file between are both natively seekable, so `nativeSeek` — which
/// decides the transport and whether a film may be cast at all — means the
/// same thing either way.
///
/// Note this is *not* the same question `LocalEngine.cannotDecode` asks. That
/// one is about the video **codec** on this particular hardware, answered by
/// the platform; this is about the **container**, and is a fact about the
/// framework rather than about the device. A `.mov` holding something this
/// machine cannot decode still falls through to HLS by that route.
let avfoundationContainers: Set<String> = ["mov"]

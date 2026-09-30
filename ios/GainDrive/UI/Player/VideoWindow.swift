//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// The picture, in a window of its own on the Mac.
///
/// A window rather than a sheet or a cover: on the Mac a film is something to
/// put beside the library, resize, move to another screen or take full screen
/// with the window's own button - none of which a cover over the main window
/// allows. One window, opened by `VideoPresentation` whenever
/// `PlayerConnection.showingVideo` asks for the picture, as the cover is on
/// iOS.
///
/// Closing it only hides the picture - the film keeps playing, as leaving the
/// cover does - and it closes itself when what is playing stops being a video.
struct VideoWindow: View {
	static let id = "video"

	@Environment(PlayerConnection.self) private var player
	@Environment(\.dismissWindow) private var dismissWindow

	var body: some View {
		Group {
			if let song = player.current, player.currentShowsPicture {
				VideoView(song: song)
					.navigationTitle(song.title)
			} else {
				ContentUnavailableView("Nothing to show", systemImage: "film")
			}
		}
		.frame(minWidth: 480, minHeight: 270)
		.background(.black)
		.onDisappear { player.showingVideo = false }
		.onChange(of: player.currentShowsPicture) {
			if !player.currentShowsPicture { dismissWindow(id: Self.id) }
		}
	}
}

#endif

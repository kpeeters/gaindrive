//	GainDrive for macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// The Settings window (Cmd-,): the iOS categories as the tabs along its top,
/// the Mac's own shape for preferences. Each page keeps the header it declares
/// with `.paneHeader`, which is where Servers and Casting keep their add
/// buttons - a Settings window has no toolbar of its own to put them in.
struct MacSettingsView: View {
	let startOnServers: Bool

	var body: some View {
		TabView {
			page(ServersSettingsView(startAdding: startOnServers), "Servers", "server.rack")
			page(LibrarySettingsView(), "Library", "music.mic")
			page(PlaybackSettingsView(), "Playback", "play.circle")
			page(CastSettingsView(), "Casting", "airplayaudio")
			page(StorageSettingsView(), "Storage", "internaldrive")
			page(AppearanceSettingsView(), "Appearance", "paintpalette")
		}
		.frame(width: 560, height: 480)
	}

	private func page<Content: View>(
		_ content: Content, _ title: String, _ symbol: String
	) -> some View {
		content
			.paneHeaderHost()
			.tabItem { Label(title, systemImage: symbol) }
	}
}

#endif

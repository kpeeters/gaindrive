//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What the app's shell says and handles whatever screen is showing: playback
/// problems, refused downloads, and shared track links. Shared by `RootView`
/// on iOS and `MacRootView` on the Mac; `onOpenRoute` is where each shows an
/// album a link resolved to.
///
/// **Alerts, not sheets.** Their content closures capture values already
/// resolved in this view's scope, so nothing performs an environment lookup
/// at presentation time - which a sheet under the sidebar-style `TabView` has
/// been seen to fail.
struct ShellMessages: ViewModifier {
	let onOpenRoute: (Route) -> Void

	@Environment(PlayerConnection.self) private var player
	@Environment(PinRepository.self) private var pins
	@Environment(ServerRegistry.self) private var registry
	/// Why a track link could not be followed.
	@State private var linkMessage: String?

	func body(content: Content) -> some View {
		content
			// **Playback errors belong to the shell, not to a screen.** They
			// arrive from the audio session, from an item that failed to load
			// and from the watchdog - and a track that could not be played
			// leaves no player bar to hang an alert on.
			.alert(
				"Playback problem",
				isPresented: Binding(
					get: { player.errorMessage != nil },
					set: { if !$0 { player.clearError() } })
			) {
				Button("OK") { player.clearError() }
			} message: {
				Text(player.errorMessage ?? "")
			}
			// Pinning is reached from several listings, so a refusal belongs
			// to the shell for the same reason a playback error does.
			.alert(
				"Cannot download that",
				isPresented: Binding(
					get: { pins.message != nil }, set: { if !$0 { pins.clearMessage() } })
			) {
				Button("OK") { pins.clearMessage() }
			} message: {
				Text(pins.message ?? "")
			}
			// A shared track link, arriving as `gaindrive://` from the server's
			// chooser page. The album starts at the track, and at the time the
			// link carried.
			.onOpenURL { url in
				guard let link = TrackLink(url) else { return }
				Task {
					switch await TrackLinkResolver(registry: registry).resolve(link) {
					case .album(let ref, let title, let song, let at):
						onOpenRoute(.album(ref, title: title, autoPlay: song, autoPlayAt: at))
					case .failure(let message):
						linkMessage = message
					}
				}
			}
			.alert(
				"Cannot open that link",
				isPresented: Binding(get: { linkMessage != nil }, set: { if !$0 { linkMessage = nil } })
			) {
				Button("OK") { linkMessage = nil }
			} message: {
				Text(linkMessage ?? "")
			}
	}
}

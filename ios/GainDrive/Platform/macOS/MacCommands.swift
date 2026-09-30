//	GainDrive for macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// The menu bar: a Playback menu, the sections and Now Playing in View, and
/// Settings... selecting the Settings section. Menus only; nothing here
/// changes what the window shows beyond what a click in it could.
///
/// Play/Pause has no key equivalent. Space is the Mac's key for it, but as a
/// menu equivalent it would fire before a text field saw it, and nobody could
/// type a space in the search field; `MacRootView` handles Space with
/// `onKeyPress` instead, which a focused text field gets first.
struct MacCommands: Commands {
	let player: PlayerConnection

	/// The front window's shell, published by `MacRootView`; nil while no main
	/// window is in front (the video window, say).
	@FocusedValue(\.shellSection) private var section
	@FocusedValue(\.showsNowPlaying) private var showsNowPlaying

	var body: some Commands {
		// One window: a second would be a second shell raising the same
		// alerts and asking for the same video window.
		CommandGroup(replacing: .newItem) {}

		CommandGroup(replacing: .appSettings) {
			Button("Settings...") { section?.wrappedValue = .settings }
				.keyboardShortcut(",")
				.disabled(section == nil)
		}

		CommandGroup(before: .sidebar) {
			ForEach(Array(MacRootView.SidebarItem.allCases.enumerated()), id: \.element) {
				index, item in
				Button(item.title) { section?.wrappedValue = item }
					.keyboardShortcut(KeyEquivalent(Character("\(index + 1)")))
					.disabled(section == nil)
			}
			Divider()
			Button(showsNowPlaying?.wrappedValue == true ? "Hide Now Playing" : "Show Now Playing") {
				showsNowPlaying?.wrappedValue.toggle()
			}
			.keyboardShortcut("i", modifiers: [.command, .option])
			.disabled(showsNowPlaying == nil)
			Divider()
		}

		CommandMenu("Playback") {
			Button(player.isPlaying ? "Pause" : "Play") { player.togglePlayPause() }
				.disabled(player.current == nil)
			Button("Next") { player.next() }
				.keyboardShortcut(.rightArrow)
				.disabled(!player.hasNext)
			// Never disabled while something plays: past three seconds in it
			// restarts the track, as the transport's button does.
			Button("Previous") { player.previous() }
				.keyboardShortcut(.leftArrow)
				.disabled(player.current == nil)
		}
	}
}

extension FocusedValues {
	@Entry var shellSection: Binding<MacRootView.SidebarItem?>?
	@Entry var showsNowPlaying: Binding<Bool>?
}

#endif

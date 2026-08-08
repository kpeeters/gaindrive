//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Star, and add to a playlist, on any track row.
///
/// A context menu rather than Android's long-press sheet: it is what iOS users
/// reach for, it needs no state of its own to present, and the same gesture
/// works on Mac Catalyst as a right-click. Attached as a modifier so the four
/// screens that show tracks cannot each grow a slightly different menu — which
/// is the same reason `Rows.swift` exists.
struct TrackActions: ViewModifier {
	let song: Song

	@Environment(StarStore.self) private var stars
	@State private var addingTo: Song?

	func body(content: Content) -> some View {
		content
			.contextMenu {
				Button {
					Task { await stars.toggle(song.ref, kind: .song, currently: song.isStarred) }
				} label: {
					Label(isStarred ? "Unstar" : "Star", systemImage: isStarred ? "star.slash" : "star")
				}
				Button {
					addingTo = song
				} label: {
					Label("Add to playlist…", systemImage: "text.badge.plus")
				}
			}
			.sheet(item: $addingTo) { song in
				AddToPlaylistView(song: song)
			}
	}

	private var isStarred: Bool {
		stars.isStarred(song.ref, fallback: song.isStarred)
	}
}

extension View {
	func trackActions(for song: Song) -> some View {
		modifier(TrackActions(song: song))
	}
}

/// The star as a control, for the places that show one on its face rather than
/// behind a menu — an album header, an artist header.
struct StarButton: View {
	let ref: ItemRef
	let kind: StarKind
	let starredAt: String?

	@Environment(StarStore.self) private var stars

	var body: some View {
		Button {
			Task { await stars.toggle(ref, kind: kind, currently: starredAt != nil) }
		} label: {
			Image(systemName: isStarred ? "star.fill" : "star")
				.foregroundStyle(isStarred ? Color.accentColor : .secondary)
		}
		.buttonStyle(.borderless)
		.disabled(stars.isBusy(ref))
		.accessibilityLabel(isStarred ? "Starred" : "Not starred")
	}

	private var isStarred: Bool {
		stars.isStarred(ref, fallback: starredAt != nil)
	}
}

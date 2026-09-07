//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What this track is, and what was asked of the server for it — the
/// counterpart of `ui/player/TrackInfoDialog.kt`.
///
/// The last row is the reason the screen exists. "Why does this sound different
/// here" is answerable only from the **capped** quality: the account ceiling
/// belongs to that track's own server, is applied per track, and appears on no
/// settings screen. Everything above it is on the row already and is here so
/// the answer arrives with its context.
struct TrackInfoView: View {
	let song: Song

	@Environment(PlayerConnection.self) private var player
	@Environment(StarStore.self) private var stars
	@Environment(\.dismiss) private var dismiss
	@State private var sent: AudioQuality?

	var body: some View {
		NavigationStack {
			Form {
				Section {
					row("Title", song.title)
					row("Artist", song.artistName)
					row("Album", song.albumTitle)
					row("Track", song.track.map(String.init))
					row("Year", song.year.map(String.init))
					row("Length", song.duration > 0 ? formatDuration(song.duration) : nil)
					// Through the store, not off the song: this view is handed a
					// snapshot, and the star may have been toggled from the
					// sheet behind it a second ago.
					row("Starred", stars.isStarred(song.ref, fallback: song.isStarred) ? "Yes" : "No")
				}

				Section {
					row("Stored", stored)
					// nil while the account ceiling is still being asked for,
					// which is a request to that track's own server and can be
					// the slow half of a first play.
					row("Sent", sent?.label)
				} footer: {
					Text(
						"""
						"Sent" is what this app asked the server for. \
						Your account's own bitrate limit applies on top.
						"""
					)
				}
			}
			.navigationTitle("Track info")
			.navigationBarTitleDisplayMode(.inline)
			.toolbar {
				ToolbarItem(placement: .topBarTrailing) {
					Button("Done") { dismiss() }
				}
			}
		}
		.task { sent = await player.streamQuality(for: song) }
	}

	/// The file as the server holds it, which is what "Original" would deliver.
	private var stored: String? {
		var parts: [String] = []
		if let suffix = song.suffix { parts.append(suffix.uppercased()) }
		if let rate = song.bitRate, rate > 0 { parts.append("\(rate) kbps") }
		if song.sizeBytes > 0 {
			parts.append(
				ByteCountFormatter.string(
					fromByteCount: Int64(song.sizeBytes), countStyle: .file))
		}
		return parts.isEmpty ? nil : parts.joined(separator: " · ")
	}

	/// A row with nothing in it is left out entirely rather than drawn empty:
	/// almost every field beyond the title is optional on the wire, and a
	/// column of blanks reads as a failed load.
	@ViewBuilder
	private func row(_ label: String, _ value: String?) -> some View {
		if let value, !value.isEmpty {
			// The view builder rather than `LabeledContent(_:value:)`, whose
			// first parameter is a `LocalizedStringKey` and so takes a literal
			// and not the `String` this helper is handed.
			LabeledContent {
				Text(value)
			} label: {
				Text(label)
			}
		}
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What this track is, and what was asked of the server for it - the
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
	@Environment(ServerRegistry.self) private var registry
	@Environment(\.dismiss) private var dismiss
	@State private var sent: AudioQuality?
	/// Where playback was when this opened, if this is the track playing.
	/// Taken once, so the link offered does not tick along while it is read.
	@State private var openedAt: Double = 0
	@State private var chapters: [Chapter] = []

	var body: some View {
		Group {
			#if os(macOS)
				dialog
			#else
				form
			#endif
		}
		.task { sent = await player.streamQuality(for: song) }
		.task {
			if player.current?.ref == song.ref { openedAt = player.position.rounded(.down) }
			chapters = await ChapterTracks(registry: registry).chapters(for: song.ref).chapters
		}
	}

	// MARK: - The facts, shared by both layouts

	private struct Fact {
		let label: String
		let value: String?
		init(_ label: String, _ value: String?) {
			self.label = label
			self.value = value
		}
	}

	private struct Shown: Identifiable {
		let label: String
		let value: String
		var id: String { label }
	}

	private var details: [Fact] {
		[
			Fact("Title", song.title),
			Fact("Artist", song.artistName),
			Fact("Album", song.albumTitle),
			Fact("Track", song.track.map(String.init)),
			Fact("Year", song.year.map(String.init)),
			Fact("Length", song.duration > 0 ? formatDuration(song.duration) : nil),
			// Through the store, not off the song: this view is handed a
			// snapshot, and the star may have been toggled from the
			// sheet behind it a second ago.
			Fact("Starred", stars.isStarred(song.ref, fallback: song.isStarred) ? "Yes" : "No"),
		]
	}

	private var delivery: [Fact] {
		var facts: [Fact] = [
			Fact("Stored", stored),
			// **Where, before what.** With a receiver in the picture "Sent" is
			// answering a different question - what went over the network to
			// something else - and reading it without knowing that is how "why
			// does this sound different" acquires a wrong answer.
			Fact("Playing on", player.castDevice?.name),
			// nil while the account ceiling is still being asked for, which is
			// a request to that track's own server and can be the slow half of
			// a first play.
			Fact("Sent", sent?.label),
		]
		if player.castSendsSoundOnly { facts.append(Fact("Picture", "Not sent")) }
		return facts
	}

	/// A fact with nothing in it is left out entirely rather than drawn empty:
	/// almost every field beyond the title is optional on the wire, and a
	/// column of blanks reads as a failed load.
	private func shown(_ facts: [Fact]) -> [Shown] {
		facts.compactMap { fact in
			guard let value = fact.value, !value.isEmpty else { return nil }
			return Shown(label: fact.label, value: value)
		}
	}

	// MARK: - iOS

	#if !os(macOS)
		private var form: some View {
			NavigationStack {
				Form {
					Section {
						rows(details)
					}
					Section {
						rows(delivery)
					} footer: {
						Text(footer)
					}
					if serverURL != nil {
						Section("Share") { shareLinks }
					}
				}
				.navigationTitle("Track info")
				.inlineTitle()
				.toolbar {
					ToolbarItem(placement: .trailingBar) {
						Button("Done") { dismiss() }
					}
				}
			}
		}

		private func rows(_ facts: [Fact]) -> some View {
			ForEach(shown(facts)) { fact in
				// The view builder rather than `LabeledContent(_:value:)`, whose
				// first parameter is a `LocalizedStringKey` and so takes a
				// literal and not the `String` this is handed.
				LabeledContent {
					Text(fact.value)
				} label: {
					Text(fact.label)
				}
			}
		}
	#endif

	// MARK: - macOS

	#if os(macOS)
		/// A Mac dialog rather than a phone screen in a sheet. **Built from
		/// stacks and a `Grid`, which have an ideal height**, so the sheet takes
		/// its height from the content; a `Form` or `List` scrolls and has none.
		/// The width is set, as an alert's is: wrapped text whose height
		/// depends on a content-driven width can loop AppKit's window layout,
		/// which crashed the cast picker.
		private var dialog: some View {
			VStack(alignment: .leading, spacing: 16) {
				Text("Track info").font(.headline)
				Grid(alignment: .leadingFirstTextBaseline, horizontalSpacing: 12, verticalSpacing: 6) {
					grid(details)
					GridRow {
						Divider().gridCellColumns(2)
					}
					grid(delivery)
				}
				Text(footer)
					.font(.footnote)
					.foregroundStyle(.secondary)
					.fixedSize(horizontal: false, vertical: true)
				if serverURL != nil {
					HStack(spacing: 8) {
						Text("Share").foregroundStyle(.secondary)
						shareLinks
					}
					.buttonStyle(.bordered)
					.controlSize(.small)
				}
				HStack {
					Spacer()
					Button("Done") { dismiss() }
						.keyboardShortcut(.defaultAction)
				}
			}
			.padding(20)
			.frame(width: 440)
		}

		@ViewBuilder
		private func grid(_ facts: [Fact]) -> some View {
			ForEach(shown(facts)) { fact in
				GridRow {
					Text(fact.label)
						.foregroundStyle(.secondary)
						.gridColumnAlignment(.trailing)
					Text(fact.value)
						.textSelection(.enabled)
						.fixedSize(horizontal: false, vertical: true)
				}
			}
		}
	#endif

	// MARK: - Sharing

	/// The track's server address, when this song's server is still known.
	private var serverURL: String? {
		registry.servers.first(where: { $0.id == song.ref.server })?.urlString
	}

	/// The web client's link, `<server>/?track=<id>[&t=<s>]`, so it lands in
	/// the same chooser page and web player as one copied from a browser. It
	/// names a server and a track and **carries no credentials** by
	/// construction, unlike the stream URL, which is why it may be shown.
	@ViewBuilder
	private var shareLinks: some View {
		if let server = serverURL,
			let plain = TrackLink.shareURL(serverURL: server, trackId: song.ref.id)
		{
			ShareLink(item: plain) { Label("Link to this track", systemImage: "link") }
			if openedAt >= 1,
				let here = TrackLink.shareURL(
					serverURL: server, trackId: song.ref.id, seconds: openedAt)
			{
				ShareLink(item: here) {
					Label("From \(formatDuration(Int(openedAt)))", systemImage: "clock")
				}
			}
			// The chapter under the position, for a concert or a mix: the
			// start of the song rather than wherever the playhead happened to
			// be.
			if let chapter = TrackLink.chapter(in: chapters, at: openedAt),
				let atChapter = TrackLink.shareURL(
					serverURL: server, trackId: song.ref.id, seconds: chapter.start)
			{
				ShareLink(item: atChapter) {
					Label("From “\(chapter.displayName)”", systemImage: "list.bullet")
				}
			}
		}
	}

	/// The last row is the reason the screen exists, so the footer is what makes
	/// it readable - and while casting there are two facts to state rather than
	/// one, because the second is the likelier surprise.
	private var footer: String {
		let base =
			"\"Sent\" is what this app asked the server for. "
			+ "Your account's own bitrate limit applies on top."
		guard player.castSendsSoundOnly else { return base }
		return base
			+ "\n\nThis device has no screen, so the film's soundtrack was sent "
			+ "instead of the picture."
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
}

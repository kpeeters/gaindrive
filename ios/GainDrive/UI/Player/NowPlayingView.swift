//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVKit
import SwiftUI
import UIKit

/// The expanded player, raised by tapping the mini player.
///
/// **It holds its own `NavigationStack`**, so the album it opens is a
/// destination inside the sheet and closing the sheet returns the reader to
/// whatever it covered. That is `android/SCREENS.md`'s rule for the same
/// screen, reached here without the alternative: the five tabs each own a
/// private `path`, so pushing into one from the shell would mean lifting them
/// all into a shared navigator for this one case.
struct NowPlayingView: View {
	@Environment(PlayerConnection.self) private var player
	@Environment(\.dismiss) private var dismiss

	@State private var path: [Route] = []
	@State private var addingTo: Song?
	@State private var showingInfo: Song?

	var body: some View {
		NavigationStack(path: $path) {
			Group {
				if let song = player.current {
					content(song)
				} else {
					// Reached by the last queue entry being removed while the
					// sheet is open. Not an error, and nothing to explain.
					ContentUnavailableView("Nothing playing", systemImage: "music.note")
				}
			}
			.navigationBarTitleDisplayMode(.inline)
			.navigationDestination(for: Route.self) { destination($0) }
			.toolbar {
				ToolbarItem(placement: .topBarLeading) {
					Button("Done") { dismiss() }
				}
				ToolbarItem(placement: .topBarTrailing) {
					// Dragging a row in a `List` needs edit mode; swiping one
					// away does not. So the button is what reordering costs,
					// and removal is free either way.
					EditButton()
				}
			}
		}
		.sheet(item: $addingTo) { AddToPlaylistView(song: $0) }
		.sheet(item: $showingInfo) { TrackInfoView(song: $0) }
	}

	private func content(_ song: Song) -> some View {
		List {
			Section {
				VStack(spacing: 16) {
					CoverHero(source: player.coverSource(for: song, size: CoverSize.hero))
					titles(song)
					NowPlayingScrubber()
					transport
					secondaryControls(song)
				}
				.padding(.vertical, 8)
			}
			.listRowSeparator(.hidden)

			QueueList()
		}
		.listStyle(.plain)
	}

	@ViewBuilder
	private func titles(_ song: Song) -> some View {
		VStack(spacing: 4) {
			Text(song.title)
				.font(.title3.weight(.semibold))
				.multilineTextAlignment(.center)
			if !song.artistName.isEmpty {
				Text(song.artistName)
					.font(.subheadline)
					.foregroundStyle(.secondary)
			}
			if !song.albumTitle.isEmpty {
				if let album = song.albumRef {
					NavigationLink(value: Route.album(album, title: song.albumTitle)) {
						Text(song.albumTitle)
							.font(.subheadline)
							.foregroundStyle(Color.accentColor)
					}
					.buttonStyle(.plain)
				} else {
					Text(song.albumTitle)
						.font(.subheadline)
						.foregroundStyle(.secondary)
				}
			}
		}
		.frame(maxWidth: .infinity)
	}

	/// Previous is never disabled: under three seconds in it steps back, and
	/// past that it restarts the track, which is what every physical transport
	/// does and what `PlayerConnection.previous` implements.
	private var transport: some View {
		HStack(spacing: 44) {
			Button {
				player.previous()
			} label: {
				Image(systemName: "backward.fill").font(.title2)
			}
			.accessibilityLabel("Previous")

			Button {
				player.togglePlayPause()
			} label: {
				Image(systemName: player.isPlaying ? "pause.fill" : "play.fill")
					.font(.system(size: 40))
					.frame(width: 52, height: 52)
			}
			.accessibilityLabel(player.isPlaying ? "Pause" : "Play")

			Button {
				player.next()
			} label: {
				Image(systemName: "forward.fill").font(.title2)
			}
			.disabled(!player.hasNext)
			.accessibilityLabel("Next")
		}
		.buttonStyle(.plain)
	}

	private func secondaryControls(_ song: Song) -> some View {
		HStack(spacing: 32) {
			StarButton(ref: song.ref, kind: .song, starredAt: song.starredAt)
			Button {
				addingTo = song
			} label: {
				Image(systemName: "music.note.list")
			}
			.accessibilityLabel("Add to playlist")
			// Always offered, and the reason is Android's: what it answers —
			// how the audio is reaching the speaker and in what format — is
			// nowhere else in the UI.
			Button {
				showingInfo = song
			} label: {
				Image(systemName: "info.circle")
			}
			.accessibilityLabel("Track info")
			AirPlayButton()
				.frame(width: 30, height: 30)
		}
		.font(.title3)
		.buttonStyle(.plain)
		.foregroundStyle(.secondary)
	}

	@ViewBuilder
	private func destination(_ route: Route) -> some View {
		switch route {
		case .albums(let artists, let name, let fromCategories):
			AlbumsView(refs: artists, artistName: name, fromCategories: fromCategories)
		case .album(let ref, let title, let autoPlay):
			AlbumDetailView(ref: ref, albumTitle: title, autoPlay: autoPlay)
		case .playlist(let ref, let name):
			PlaylistDetailView(ref: ref, playlistName: name)
		}
	}
}

/// **Its own view because it reads `position`.**
///
/// `@Observable` invalidates per *view*, not per property access, so a scrubber
/// written inline in `NowPlayingView.body` would re-render the queue list twice
/// a second along with itself. `PlayerConnection`'s header states the same rule
/// for `trackState(of:)`; this is the other half of it, and the symptom is
/// identical — a list that stutters with nothing to say why.
struct NowPlayingScrubber: View {
	@Environment(PlayerConnection.self) private var player

	/// Non-nil only while a finger is down. The player's own position keeps
	/// arriving during a drag, and without somewhere else to draw from the
	/// thumb would fight the clock.
	@State private var scrubbing: Double?

	var body: some View {
		VStack(spacing: 2) {
			Slider(
				value: Binding(
					get: { min(scrubbing ?? player.position, bound) },
					set: { scrubbing = $0 }),
				in: 0...bound,
				onEditingChanged: { editing in
					guard !editing, let target = scrubbing else { return }
					player.seek(to: target)
					scrubbing = nil
				}
			)
			// A chunked response cannot be seeked and a seek into one silently
			// does nothing, so the control says so rather than appearing to
			// work. `canSeek` comes from the item's own `seekableTimeRanges`,
			// never from a guess about the format.
			.disabled(!player.canSeek)
			.accessibilityLabel("Playback position")
			// Seconds read as a percentage are useless. The value is spoken as
			// the same clock the labels below show.
			.accessibilityValue(clock(scrubbing ?? player.position))

			HStack {
				Text(clock(scrubbing ?? player.position))
				Spacer()
				Text(clock(player.duration))
			}
			.font(.caption.monospacedDigit())
			.foregroundStyle(.secondary)
		}
	}

	/// Never zero: a `Slider` over an empty range traps.
	private var bound: Double { max(player.duration, 1) }

	/// `formatDuration` answers "--:--" for nothing, which is right in a track
	/// listing and wrong on a clock that is about to start counting.
	private func clock(_ seconds: Double) -> String {
		guard seconds.isFinite, seconds > 0 else { return "0:00" }
		return formatDuration(Int(seconds))
	}
}

/// The system route picker.
///
/// AirPlay itself comes free with `AVPlayer` — the audio session routes itself
/// — but the button does not, and `AVRoutePickerView` is UIKit. `PLAN.md` lists
/// this as the one capability iOS gets that Android does not.
struct AirPlayButton: UIViewRepresentable {
	func makeUIView(context: Context) -> AVRoutePickerView {
		let view = AVRoutePickerView()
		// Audio only. Video is its own surface in phase 6, and prioritising
		// video devices here would put a TV above the speaker someone is
		// listening on.
		view.prioritizesVideoDevices = false
		view.tintColor = .secondaryLabel
		return view
	}

	func updateUIView(_ view: AVRoutePickerView, context: Context) {}
}

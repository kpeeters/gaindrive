//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The transport along the bottom of the Mac window and of a wide iPad: what
/// is playing, the controls, the position, and where the sound goes. The web
/// client's player bar, with no Now Playing panel behind it: track info is a
/// button, and the cover opens the album.
struct PlayerBar: View {
	@Environment(PlayerConnection.self) private var player
	@Environment(StarStore.self) private var stars
	@Environment(ServerRegistry.self) private var registry
	@Environment(\.openRoute) private var openRoute
	@State private var castPicker = false
	@State private var showingInfo: Song?

	var body: some View {
		HStack(spacing: 16) {
			nowPlaying
				.frame(width: 240, alignment: .leading)
			transport
			NowPlayingScrubber()
				.frame(maxWidth: .infinity)
				.disabled(player.current == nil)
			routes
		}
		.padding(.horizontal, 16)
		.padding(.vertical, 8)
		.background(.bar)
		// In a VStack for the reason `PaneHeaderBar` gives.
		.overlay(alignment: .top) { VStack(spacing: 0) { Divider() } }
		.sheet(isPresented: $castPicker) {
			CastDeviceSheet()
				.environment(player)
		}
		// Handed over explicitly: on the Mac a sheet raised from here has been
		// presented without its environment.
		.sheet(item: $showingInfo) {
			TrackInfoView(song: $0)
				.environment(player)
				.environment(stars)
				.environment(registry)
		}
	}

	/// A click opens the album, as the web's cover does.
	@ViewBuilder
	private var nowPlaying: some View {
		if let song = player.current {
			HStack(spacing: 10) {
				CoverThumb(
					source: player.coverSource(for: song, size: CoverSize.thumb), size: 40)
				VStack(alignment: .leading, spacing: 1) {
					Text(song.title)
						.font(.callout.weight(.medium))
						.lineLimit(1)
					Text(song.artistName)
						.font(.caption)
						.foregroundStyle(.secondary)
						.lineLimit(1)
				}
			}
			.contentShape(.rect)
			.onTapGesture {
				guard let album = song.albumRef else { return }
				openRoute(.album(album, title: song.albumTitle))
			}
			.accessibilityAddTraits(.isButton)
			.accessibilityHint("Opens the album")
		} else {
			Text("Nothing playing")
				.foregroundStyle(.secondary)
		}
	}

	private var transport: some View {
		HStack(spacing: 14) {
			Button {
				player.previous()
			} label: {
				Image(systemName: "backward.fill")
			}
			.accessibilityLabel("Previous")
			Button {
				player.togglePlayPause()
			} label: {
				Image(systemName: player.isPlaying ? "pause.fill" : "play.fill")
					.font(.title2)
					.frame(width: 28)
			}
			.accessibilityLabel(player.isPlaying ? "Pause" : "Play")
			Button {
				player.next()
			} label: {
				Image(systemName: "forward.fill")
			}
			.disabled(!player.hasNext)
			.accessibilityLabel("Next")
		}
		.buttonStyle(.borderless)
		.disabled(player.current == nil)
	}

	private var routes: some View {
		HStack(spacing: 12) {
			// The way back to the picture once its window was closed, as on
			// the iOS mini player.
			if player.currentShowsPicture {
				Button {
					player.showingVideo = true
				} label: {
					Image(systemName: "film")
				}
				.accessibilityLabel("Watch")
			}
			// Info before cast, as on Android: the button that is always
			// there is the one that must not move.
			Button {
				showingInfo = player.current
			} label: {
				Image(systemName: "info.circle")
			}
			.disabled(player.current == nil)
			.accessibilityLabel("Track info")
			// Beside the cast button, as on the web: the receiver it adjusts is
			// the one that button chose.
			CastTuneButton()
			CastButton(showing: $castPicker, size: 16)
			AirPlayButton(player: player.videoPlayer)
				.frame(width: 24, height: 24)
		}
		.buttonStyle(.borderless)
	}
}

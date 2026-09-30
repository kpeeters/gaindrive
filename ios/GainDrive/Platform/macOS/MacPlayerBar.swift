//	GainDrive for macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// The transport along the bottom of the Mac window: what is playing, the
/// controls, the position, and where the sound goes. The iOS mini player is a
/// doorway into a sheet; on the Mac there is room for the controls themselves,
/// and Now Playing is an inspector the bar toggles.
struct MacPlayerBar: View {
	@Binding var showsInspector: Bool

	@Environment(PlayerConnection.self) private var player
	@Environment(CastDeviceStore.self) private var castDevices
	@State private var castPicker = false

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
				.environment(castDevices)
		}
	}

	/// A click opens or closes the inspector, as Music's artwork does.
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
			.onTapGesture { showsInspector.toggle() }
			.accessibilityAddTraits(.isButton)
			.accessibilityHint("Shows Now Playing")
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
			CastButton(showing: $castPicker, size: 16)
			AirPlayButton(player: player.videoPlayer)
				.frame(width: 24, height: 24)
			Button {
				showsInspector.toggle()
			} label: {
				Image(systemName: "sidebar.trailing")
			}
			.accessibilityLabel(showsInspector ? "Hide Now Playing" : "Show Now Playing")
		}
		.buttonStyle(.borderless)
	}
}

#endif

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The bar above the tab bar.
///
/// It lives in the shell rather than in any navigation stack, so it survives
/// navigation the way the web client's fixed footer does.
struct MiniPlayer: View {
	@Environment(PlayerConnection.self) private var player
	@State private var expanded = false

	var body: some View {
		if let song = player.current {
			VStack(spacing: 0) {
				progress
				HStack(spacing: 12) {
					// The tap target is the cover and the text, not the whole
					// bar: wrapping the row in a `Button` would swallow the
					// transport buttons inside it.
					HStack(spacing: 12) {
						CoverThumb(source: cover(for: song), size: 40)
						VStack(alignment: .leading, spacing: 1) {
							Text(song.title)
								.font(.footnote.weight(.medium))
								.lineLimit(1)
							Text(song.artistName)
								.font(.caption)
								.foregroundStyle(.secondary)
								.lineLimit(1)
						}
						Spacer(minLength: 0)
					}
					.contentShape(.rect)
					.onTapGesture { expanded = true }
					.accessibilityAddTraits(.isButton)
					.accessibilityHint("Opens the player")

					Button {
						player.togglePlayPause()
					} label: {
						Image(systemName: player.isPlaying ? "pause.fill" : "play.fill")
							.font(.title3)
							.frame(width: 32, height: 32)
					}
					Button {
						player.next()
					} label: {
						Image(systemName: "forward.fill")
							.font(.body)
							.frame(width: 32, height: 32)
					}
					.disabled(!player.hasNext)
				}
				.padding(.horizontal, 12)
				.padding(.vertical, 8)
			}
			.background(.bar)
			.overlay(alignment: .top) {
				Divider()
			}
			.accessibilityElement(children: .contain)
			.sheet(isPresented: $expanded) { NowPlayingView() }
		}
	}

	@ViewBuilder
	private var progress: some View {
		if player.isBuffering {
			// Indeterminate while filling the buffer: a bar frozen at zero
			// looks like a stall rather than like work in progress.
			ProgressView().progressViewStyle(.linear)
		} else {
			ProgressView(value: fraction)
				.progressViewStyle(.linear)
		}
	}

	/// Zero rather than NaN before the duration is known.
	private var fraction: Double {
		guard player.duration > 0 else { return 0 }
		return min(max(player.position / player.duration, 0), 1)
	}

	private func cover(for song: Song) -> CoverSource? {
		player.coverSource(for: song, size: CoverSize.thumb)
	}
}

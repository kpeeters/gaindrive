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
/// It is applied to a tab's *content* rather than to any navigation stack
/// inside it, so it survives navigation the way the web client's fixed footer
/// does — `miniPlayerInset` is the only thing that should attach it.
struct MiniPlayer: View {
	/// Raising the Now Playing sheet is the **shell's** job. There is one bar
	/// per tab, and there must be only one sheet.
	let onExpand: () -> Void

	@Environment(PlayerConnection.self) private var player

	var body: some View {
		if let song = player.current {
			VStack(spacing: 0) {
				progress
					.accessibilityHidden(true)
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
					.onTapGesture { onExpand() }
					.accessibilityAddTraits(.isButton)
					.accessibilityHint("Opens the player")

					Button {
						player.togglePlayPause()
					} label: {
						Image(systemName: player.isPlaying ? "pause.fill" : "play.fill")
							.font(.title3)
							.frame(width: 32, height: 32)
					}
					.accessibilityLabel(player.isPlaying ? "Pause" : "Play")
					Button {
						player.next()
					} label: {
						Image(systemName: "forward.fill")
							.font(.body)
							.frame(width: 32, height: 32)
					}
					.disabled(!player.hasNext)
					.accessibilityLabel("Next")
				}
				.padding(.horizontal, 12)
				.padding(.vertical, 8)
			}
			.background(.bar)
			.overlay(alignment: .top) {
				Divider()
			}
			.accessibilityElement(children: .contain)
		}
	}

	/// Hidden from VoiceOver by its caller: it is a hairline with no label
	/// worth reading, and the position it stands for is announced properly by
	/// the scrubber in the Now Playing sheet.
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

extension View {
	/// The mini player, above the tab bar.
	///
	/// **Applied to each tab's content, never to the `TabView`.** That is the
	/// whole point of this modifier existing, and the trap it exists to
	/// document: a bottom `safeAreaInset` on a `TabView` is inserted at the
	/// bottom of the *TabView's own frame*, which is where the tab bar is — so
	/// the bar lands on top of the navigation icons and hides them completely.
	/// Applied to a tab's content it lands above the tab bar, which is where a
	/// mini player belongs.
	///
	/// `safeAreaInset` rather than an overlay, so every list's content inset
	/// grows by the bar's height and the last row is still reachable.
	///
	/// The cost is one `MiniPlayer` per tab. They are pure presentation —
	/// each reads the same `PlayerConnection`, and the sheet they raise belongs
	/// to the shell — so the copies cannot disagree with one another.
	func miniPlayerInset(onExpand: @escaping () -> Void) -> some View {
		safeAreaInset(edge: .bottom, spacing: 0) {
			MiniPlayer(onExpand: onExpand)
		}
	}
}

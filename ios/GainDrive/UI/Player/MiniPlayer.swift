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
	@Environment(PlayerConnection.self) private var player
	/// **The sheet is raised from here, not from the shell.**
	///
	/// It was on the `TabView` for a while, on the reasoning that one bar per
	/// tab should not mean one sheet per tab. That reasoning was tidiness and
	/// it crashed: `.sidebarAdaptable` hands presentation to UIKit, and a sheet
	/// presented from the `TabView` does not carry the SwiftUI environment into
	/// its content — so `NowPlayingView`'s own `@Environment` lookup found no
	/// `PlayerConnection` and trapped.
	///
	/// The two alerts still on the shell are safe for a reason worth knowing:
	/// their content closures capture values already resolved in `RootView`'s
	/// scope, so nothing looks anything up at presentation time. A sheet whose
	/// content is a *view* does.
	///
	/// One sheet per bar is not one sheet on screen: only the selected tab's
	/// bar is in the hierarchy, so only one of them can ever be tapped.
	@State private var expanded = false

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
			.sheet(isPresented: $expanded) { NowPlayingView() }
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
	/// The cost is one `MiniPlayer` per tab, each with its own Now Playing
	/// sheet. They are pure presentation — every one reads the same
	/// `PlayerConnection` — and only the selected tab's bar is in the
	/// hierarchy, so the copies can neither disagree nor both present.
	func miniPlayerInset() -> some View {
		safeAreaInset(edge: .bottom, spacing: 0) {
			MiniPlayer()
		}
	}
}

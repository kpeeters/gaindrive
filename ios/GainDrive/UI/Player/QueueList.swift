//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The queue, as a section of the Now Playing list.
///
/// A view of its own rather than a method on `NowPlayingView`, so that reading
/// `queue`, `queueIndex` and `autoFrom` here does not put those reads in the
/// same view as the scrubber's `position` - see `NowPlayingScrubber`. **It must
/// never read `position`.**
///
/// Reordering is a capability Android's queue does not have; `PlayQueue.move`
/// is already here and already tested, so what was missing was only a way to
/// ask for it.
struct QueueList: View {
	@Environment(PlayerConnection.self) private var player

	var body: some View {
		Section {
			// Indices, because every edit below is positional and because a
			// track queued twice appears twice - the ref alone is not a unique
			// identity here, the same reason Recents enumerates.
			ForEach(Array(player.queue.enumerated()), id: \.offset) { index, song in
				row(index: index, song: song)
			}
			.onDelete { offsets in
				// Descending, because each removal renumbers what is below it:
				// applied in ascending order the second offset would name a
				// different track than the one the user swiped.
				for index in offsets.sorted(by: >) { player.remove(at: index) }
			}
			.onMove { source, offset in
				guard let from = source.first else { return }
				player.move(from: from, to: QueueMove.destination(from: from, insertingBefore: offset))
			}
		} header: {
			SectionHeading(text: "Queue").pinnedHeaderBackground()
		}
	}

	private func row(index: Int, song: Song) -> some View {
		VStack(alignment: .leading, spacing: 4) {
			if index == player.autoFrom, index > 0 {
				// Everything from here down is the album's own tail, which
				// "add to queue" **replaces** rather than appends behind.
				// Saying so is what stops that replacement reading as tracks
				// going missing - which is the complaint the boundary exists
				// to answer in the first place.
				Text("Continuing from the album")
					.font(.caption2)
					.foregroundStyle(.secondary)
			}
			// Keyed on the index rather than on `trackState(of:)`, which
			// matches by ref: the same track queued twice would light up both
			// rows. It is also what keeps `position` out of this view.
			TrackRow(
				song: song,
				state: index == player.queueIndex ? .current : .idle,
				number: index + 1)
		}
		.contentShape(.rect)
		.onTapGesture { player.jump(to: index) }
		// A tap gesture is invisible to VoiceOver, unlike the `NavigationLink`
		// and `Button` every other listing's rows are built from.
		.accessibilityAddTraits(.isButton)
	}
}

/// The one piece of arithmetic in the queue list, extracted so it can be
/// tested by calling it.
enum QueueMove {
	/// SwiftUI's `onMove` hands over the offset the row is inserted
	/// **before**, counted in the list as it stands *with the row still in
	/// it*. `PlayQueue.move` takes the index the row ends up at. For a move
	/// downwards those differ by one, and getting it wrong drops the track one
	/// place short of where it was let go - which looks like a laggy gesture
	/// rather than like a bug.
	static func destination(from source: Int, insertingBefore offset: Int) -> Int {
		source < offset ? offset - 1 : offset
	}
}

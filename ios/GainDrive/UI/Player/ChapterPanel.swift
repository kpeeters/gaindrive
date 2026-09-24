//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The chapter jump list drawn over the picture. See `VideoView.chapterPanel`
/// for why it sits there and why it is opaque.
struct ChapterPanel: View {
	let list: ChapterList
	/// The marker being played, or nil before the first. Passed in rather than
	/// derived, so the position is read once for the whole screen.
	let currentIndex: Int?
	/// Seconds. The panel seeks inside the recording; it never queues anything,
	/// because every marker here belongs to the item already playing.
	let onSeek: (Double) -> Void
	let onClose: () -> Void

	var body: some View {
		VStack(spacing: 0) {
			header
			Divider()
			rows
			// The one thing the source is worth saying out loud: these markers
			// are inside the file rather than in a sidecar beside it, which is
			// also why they do not appear in the album listing - that reads the
			// scan's index, and only sidecars are indexed.
			if list.source == .container {
				Divider()
				Text("From the video file")
					.font(.footnote)
					.foregroundStyle(.secondary)
					.frame(maxWidth: .infinity, alignment: .leading)
					.padding(.horizontal, 12)
					.padding(.vertical, 8)
			}
		}
		.background(.background)
	}

	private var header: some View {
		HStack(spacing: 4) {
			// Chevrons rather than skip-previous/skip-next: those already mean
			// "the next item in the queue" in the transport a few points away,
			// and stepping between markers is not that.
			Button {
				onSeek(list.chapters.previousTarget(from: position))
			} label: {
				Image(systemName: "chevron.up")
			}
			.accessibilityLabel("Previous chapter")

			Button {
				if let next = list.chapters.next(after: position) { onSeek(next.start) }
			} label: {
				Image(systemName: "chevron.down")
			}
			.accessibilityLabel("Next chapter")
			.disabled(list.chapters.next(after: position) == nil)

			Text("Chapters")
				.font(.subheadline.weight(.semibold))
				.frame(maxWidth: .infinity, alignment: .leading)
				.padding(.leading, 8)

			Button(action: onClose) {
				Image(systemName: "xmark")
			}
			.accessibilityLabel("Close")
		}
		.buttonStyle(.borderless)
		.padding(8)
	}

	/// **The position the two step buttons work from is the marker's own
	/// start**, not the player's clock, and that is deliberate: this view is
	/// handed `currentIndex` rather than a live position, so it cannot read one
	/// - and it must not, or the panel would redraw twice a second while it is
	/// being scrolled. Stepping from the start of the marker being played gives
	/// the same answer everywhere except within `chapterRestartWindow` of it,
	/// where "previous" would restart the marker instead of leaving it; that is
	/// the one behaviour a transport button has that a *list* does not need.
	private var position: Double {
		currentIndex.map { list.chapters[$0].start } ?? 0
	}

	private var rows: some View {
		ScrollViewReader { proxy in
			// Enumerated because "which one is playing" is a *position* in the
			// list, while `index` is the server's own numbering - which is the
			// same thing today and need not be if a list ever arrives with a
			// gap in it.
			List(Array(list.chapters.enumerated()), id: \.element.index) { entry in
				Button {
					onSeek(entry.element.start)
				} label: {
					row(entry.element, playing: entry.offset == currentIndex)
				}
				.buttonStyle(.plain)
				.listRowInsets(EdgeInsets(top: 8, leading: 12, bottom: 8, trailing: 12))
				.id(entry.element.index)
			}
			.listStyle(.plain)
			// A concert can carry dozens of markers, so the one being played is
			// usually off-screen; without this the highlight is invisible for
			// most of the film. Only on a *change* of marker, or a scroll would
			// be yanked back under the reader's finger.
			.onChange(of: currentIndex) { _, now in
				guard let now, list.chapters.indices.contains(now) else { return }
				withAnimation { proxy.scrollTo(list.chapters[now].index, anchor: .center) }
			}
		}
	}

	private func row(_ chapter: Chapter, playing: Bool) -> some View {
		HStack(spacing: 8) {
			Text(formatChapterTime(chapter.start))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
			Text(chapter.displayName)
				.lineLimit(1)
				.foregroundStyle(playing ? Color.accentColor : Color.primary)
			Spacer(minLength: 0)
		}
		.contentShape(.rect)
	}
}

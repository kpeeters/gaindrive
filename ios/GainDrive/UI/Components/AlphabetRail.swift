//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The fast-scroll index down the side of the artist list.
///
/// Hand-rolled because iOS 18 exposes no SwiftUI equivalent of
/// `UITableView.sectionIndexTitles` - this is not a case of ignoring a
/// built-in.
///
/// **Trailing edge**, for the same reason Android puts it there and a different
/// cause: on iOS the *leading* edge is the interactive-pop region, and a rail
/// under it would fight the back gesture on every touch.
struct AlphabetRail: View {
	let labels: [String]
	let onSelect: (String) -> Void

	@State private var active: String?

	var body: some View {
		// Below two letters every tap would be a no-op, so there is nothing to
		// show.
		if labels.count > 1 {
			GeometryReader { geometry in
				VStack(spacing: 0) {
					ForEach(labels, id: \.self) { label in
						Text(label)
							.font(.system(size: 11, weight: .semibold))
							.foregroundStyle(label == active ? Color.accentColor : .secondary)
							.frame(maxWidth: .infinity, maxHeight: .infinity)
					}
				}
				// Without this the gaps between letters swallow nothing and the
				// drag lands on the list underneath.
				.contentShape(.rect)
				// A tap and a drag are the *same* gesture, with a zero minimum
				// distance - which is what makes scrubbing feel continuous
				// rather than needing a separate tap handler that behaves
				// subtly differently.
				.highPriorityGesture(
					DragGesture(minimumDistance: 0)
						.onChanged { value in select(at: value.location.y, in: geometry.size.height) }
						.onEnded { _ in active = nil }
				)
			}
			.frame(width: 22)
			// 26 single letters read aloud one at a time is noise; the list
			// itself is the accessible way to navigate.
			.accessibilityElement(children: .ignore)
			.accessibilityLabel("Jump to letter")
			// A free win Android does not have.
			.sensoryFeedback(.selection, trigger: active)
		}
	}

	private func select(at y: CGFloat, in height: CGFloat) {
		guard height > 0, !labels.isEmpty else { return }
		let step = height / CGFloat(labels.count)
		let index = min(labels.count - 1, max(0, Int(y / step)))
		let label = labels[index]
		guard label != active else { return }
		active = label
		onSelect(label)
	}
}

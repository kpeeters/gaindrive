//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// How many panes fit, and which levels of a path they show. Pure, so the
/// thresholds and the window are tested without a screen.
enum PaneMath {
	/// The web client's thresholds, as Android uses them: three panes from
	/// 900 points, two from 650, else one. Measured on the **pane area** - the
	/// width this tab's content is given, beside any sidebar - and not on the
	/// size class, which is `regular` at every width on Mac Catalyst and so
	/// never allowed two panes, or one in a narrow window.
	static let twoPanes: CGFloat = 650
	static let threePanes: CGFloat = 900

	static func count(width: CGFloat) -> Int {
		width >= threePanes ? 3 : width >= twoPanes ? 2 : 1
	}

	/// The first level shown when `levels` are open (the root included) in
	/// `panes` panes: the deepest ones, as Android's `leadingWindow` does. A
	/// path shallower than the panes starts at the root and leaves the rest as
	/// placeholders.
	static func firstLevel(levels: Int, panes: Int) -> Int {
		max(0, levels - panes)
	}
}

/// A tab's navigation, as one stack on a narrow screen and as side-by-side
/// panes on a wide one - Android's `PaneLayout` and the web client's pane
/// view, driven by the same path the stack uses.
///
/// **One pane is a plain `NavigationStack`**, so a phone behaves exactly as
/// before: push, swipe back, the lot. **Two or three** show the deepest levels
/// of the path side by side, each in a `NavigationStack` of its own so it keeps
/// the bar its view declares.
///
/// **A pane never pushes.** Its path binding always reads empty, and a write
/// to it - which is what a `NavigationLink(value:)` inside it does - lands in
/// the shared path, cut at that pane's level. So the row that opened the next
/// level opens it in the next pane, and every existing `NavigationLink` keeps
/// working unchanged in both layouts. The leftmost pane gets a back button
/// when it is not the root; the web client's rule, since at two or three
/// panes the level above is on screen and an arrow at it would be noise.
///
/// `maxLevels` caps the panes at what the tab can ever show - Playlists never
/// has a third level, so it never draws a third pane.
struct PaneNavigator<R: Hashable, Root: View, Destination: View, Placeholder: View>: View {
	@Binding var path: [R]
	let maxLevels: Int
	@ViewBuilder let root: () -> Root
	@ViewBuilder let destination: (R) -> Destination
	/// What an empty pane says, by level: "Choose an artist" rather than a
	/// blank rectangle, which on a tablet reads as a rendering fault.
	@ViewBuilder let placeholder: (Int) -> Placeholder

	var body: some View {
		GeometryReader { geometry in
			let panes = min(PaneMath.count(width: geometry.size.width), maxLevels)
			if panes <= 1 {
				NavigationStack(path: $path) {
					root().navigationDestination(for: R.self) { destination($0) }
				}
			} else {
				row(panes)
			}
		}
	}

	/// Keyed by what each pane shows, not by its position: a level inserted
	/// in front - Recents filling in the artist once the album has loaded -
	/// moves the album one pane along, and keyed by position it would be
	/// rebuilt and start playing again.
	private func row(_ panes: Int) -> some View {
		let first = PaneMath.firstLevel(levels: path.count + 1, panes: panes)
		let shown = (first..<(first + panes)).map { Shown(level: $0, key: identity($0)) }
		return HStack(spacing: 0) {
			ForEach(shown, id: \.key) { pane in
				if pane.level > first { Divider() }
				column(pane.level, leading: pane.level == first)
					.environment(\.paneCount, panes)
			}
		}
	}

	/// Clipped, because a pane flush against a safe-area edge is extended
	/// into it by SwiftUI's full-bleed rule for scrollables - a selection
	/// highlight drew under the sidebar through its translucent material.
	private func column(_ level: Int, leading: Bool) -> some View {
		NavigationStack(path: columnPath(level)) {
			content(level)
				// Registered so a link inside resolves; never shown, since this
				// stack's path always reads empty.
				.navigationDestination(for: R.self) { _ in EmptyView() }
				.toolbar {
					if leading, level > 0 {
						ToolbarItem(placement: .topBarLeading) {
							Button {
								path.removeLast()
							} label: {
								Label("Back", systemImage: "chevron.backward")
							}
						}
					}
				}
		}
		.frame(maxWidth: .infinity)
		.clipped()
	}

	@ViewBuilder
	private func content(_ level: Int) -> some View {
		if level == 0 {
			// Inline like its neighbours: a large title beside inline bars is
			// bars of two heights.
			root().navigationBarTitleDisplayMode(.inline)
		} else if level <= path.count {
			destination(path[level - 1])
		} else {
			// The empty title keeps an inline bar over the placeholder, so the
			// bars stay one height before anything is chosen.
			placeholder(level).navigationTitle("")
		}
	}

	private struct Shown {
		let level: Int
		let key: AnyHashable
	}

	/// A fresh identity per thing shown, so a pane's `@State` view model does
	/// not survive a change of what it is showing and go on drawing the
	/// previous artist's albums - and a stable one while it shows the same.
	private func identity(_ level: Int) -> AnyHashable {
		if level == 0 { return AnyHashable("root") }
		if level <= path.count { return AnyHashable(path[level - 1]) }
		return AnyHashable("empty-\(level)")
	}

	/// Reads empty, and writes into the shared path below this level.
	private func columnPath(_ level: Int) -> Binding<[R]> {
		Binding(
			get: { [] },
			set: { pushed in path = Array(path.prefix(level)) + pushed })
	}
}

extension EnvironmentValues {
	/// How many panes the enclosing `PaneNavigator` is showing; 1 in a stack.
	/// For a screen that behaves differently beside others than on its own -
	/// Recents fills in the artist only when there is a pane to show it in.
	@Entry var paneCount = 1
}


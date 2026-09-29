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
/// **Rows select; they never link.** Each level's list takes
/// `PaneSelection.selection(path, level:)`, so a tap extends the path: the
/// next pane changes, or on one pane the stack pushes. A pane's own
/// `NavigationStack` is there for its bar and never pushes. An earlier
/// version let `NavigationLink`s push into a pane whose path refused them,
/// which relied on undocumented behaviour and showed as a slide out and back
/// on every tap. The leftmost pane gets a back button when it is not the
/// root; the web client's rule, since at two or three panes the level above
/// is on screen and an arrow at it would be noise.
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
		let panesRow = HStack(spacing: 0) {
			ForEach(shown, id: \.key) { pane in
				if pane.level > first { Divider() }
				column(pane.level, leading: pane.level == first)
					.environment(\.paneCount, panes)
			}
		}
		#if targetEnvironment(macCatalyst)
			// **An experiment, 2026-09-29.** With the system title hidden (see
			// `RootView`), the Mac keeps a ~27 pt strip at the top as a safe-area
			// inset, and the panes' bars sat below it with an empty band above
			// each title. Ignoring the top safe area is meant to move the bars and
			// the dividers up into that strip. Whether a UIKit navigation bar
			// follows SwiftUI here is exactly what is being tried; if nothing
			// moves, this goes again.
			return panesRow.ignoresSafeArea(.container, edges: .top)
		#else
			return panesRow
		#endif
	}

	/// Clipped, because a pane flush against a safe-area edge is extended
	/// into it by SwiftUI's full-bleed rule for scrollables - a selection
	/// highlight drew under the sidebar through its translucent material.
	private func column(_ level: Int, leading: Bool) -> some View {
		// A stack for the bar alone: nothing in a pane pushes.
		NavigationStack {
			content(level)
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
}

/// How a level's list opens the next one. Not a member of `PaneNavigator`,
/// whose generic parameters a call site could not spell.
enum PaneSelection {
	/// The selection of a list at `level`: what the next level is showing.
	///
	/// **This is how every level opens the next one**, and the reason no row
	/// is a `NavigationLink`. A tap selects; the path grows by what was
	/// selected; the next pane changes, or on one pane the stack pushes. A
	/// pop or a back shrinks the path, and the selection reads nil again.
	/// Only documented behaviour - `List(selection:)` and a stack's path -
	/// and the list highlights what is open, as a pane layout should.
	static func selection<R>(_ path: Binding<[R]>, level: Int) -> Binding<R?> {
		Binding(
			get: { level < path.wrappedValue.count ? path.wrappedValue[level] : nil },
			set: { picked in
				var next = Array(path.wrappedValue.prefix(level))
				if let picked { next.append(picked) }
				path.wrappedValue = next
			})
	}
}

extension EnvironmentValues {
	/// How many panes the enclosing `PaneNavigator` is showing; 1 in a stack.
	/// For a screen that behaves differently beside others than on its own -
	/// Recents fills in the artist only when there is a pane to show it in.
	@Entry var paneCount = 1
}

extension View {
	/// A disclosure chevron on a row that opens a level, **on one pane
	/// only**: there the row pushes and the chevron says so, as a
	/// `NavigationLink` row would. Beside other panes it opens the next one
	/// and the highlight is the cue, so nothing is drawn.
	func paneDisclosure() -> some View {
		modifier(PaneDisclosure())
	}
}

private struct PaneDisclosure: ViewModifier {
	@Environment(\.paneCount) private var paneCount

	func body(content: Content) -> some View {
		if paneCount > 1 {
			content
		} else {
			HStack {
				content
				Image(systemName: "chevron.forward")
					.font(.footnote.weight(.semibold))
					.foregroundStyle(.tertiary)
			}
		}
	}
}

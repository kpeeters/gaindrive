//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// A screen's title and its own actions, as the pane it sits in shows them.
///
/// **One declaration, two presentations.** On iOS this is the ordinary
/// navigation title and trailing bar buttons, exactly what the screens
/// declared before. On the Mac the panes have no navigation bars - the window
/// has one toolbar, which holds only what belongs to the whole window - so
/// the title and actions travel up as a preference to the header row
/// `MacPaneRow` draws at the top of each pane, as Finder's columns and Xcode's
/// editors label themselves.
///
/// Window-wide controls - the library selector, refresh, uploads - stay in
/// `.toolbar`, which is the window toolbar on the Mac and the bar on iOS.
extension View {
	func paneHeader(_ title: String) -> some View {
		paneHeader(title) { EmptyView() }
	}

	func paneHeader<Actions: View>(
		_ title: String, @ViewBuilder actions: () -> Actions
	) -> some View {
		#if os(macOS)
			preference(
				key: PaneHeaderKey.self,
				value: PaneHeaderValue(title: title, actions: AnyView(actions())))
		#else
			navigationTitle(title)
				.toolbar {
					ToolbarItemGroup(placement: .trailingBar) { actions() }
				}
		#endif
	}
}

struct PaneHeaderValue {
	let title: String
	let actions: AnyView
}

/// The innermost declaration wins: a pane holds one screen, and that screen
/// is what names it.
struct PaneHeaderKey: PreferenceKey {
	static var defaultValue: PaneHeaderValue? { nil }

	static func reduce(value: inout PaneHeaderValue?, nextValue: () -> PaneHeaderValue?) {
		value = nextValue() ?? value
	}
}

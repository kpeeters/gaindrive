//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Which slice of the library is showing — the content types a server names on
/// its roots, plus the account's own uploads.
///
/// **Drawn only when there is more than one**, which is what keeps a server
/// with a single untyped library looking exactly as it did: a row of one chip
/// is a control that does nothing. `LibraryRoots.chips` is where that rule is
/// really decided; this only declines to draw a row it was handed one of.
///
/// It scrolls horizontally because the row unions every server in scope, so its
/// length is not something the app controls.
struct LibraryModeChips: View {
	let modes: [LibraryMode]
	let selected: LibraryMode
	let onSelect: (LibraryMode) -> Void

	var body: some View {
		if modes.count > 1 {
			ScrollView(.horizontal) {
				HStack(spacing: 8) {
					ForEach(modes) { mode in
						Button(mode.label) { onSelect(mode) }
							.tint(mode == selected ? Color.accentColor : Color.secondary)
							.fontWeight(mode == selected ? .semibold : .regular)
							.accessibilityAddTraits(mode == selected ? [.isSelected] : [])
					}
				}
				.padding(.horizontal, 16)
				.padding(.vertical, 6)
			}
			.buttonStyle(.bordered)
			.controlSize(.small)
			.scrollIndicators(.hidden)
			// Opaque, because it sits over scrolling content as a safe-area
			// inset rather than inside the list.
			.background(.bar)
		}
	}
}

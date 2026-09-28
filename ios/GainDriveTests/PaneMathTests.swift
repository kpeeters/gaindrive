//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import CoreGraphics
import Testing

@testable import GainDrive

/// How many panes, and which levels they show - the web client's rules.
struct PaneMathTests {
	@Test func theThresholdsAreTheWebClients() {
		#expect(PaneMath.count(width: 400) == 1)
		#expect(PaneMath.count(width: 649) == 1)
		#expect(PaneMath.count(width: 650) == 2)
		#expect(PaneMath.count(width: 899) == 2)
		#expect(PaneMath.count(width: 900) == 3)
		#expect(PaneMath.count(width: 1400) == 3)
	}

	/// The deepest levels are the ones shown.
	@Test func theWindowShowsTheDeepestLevels() {
		// Root, artist, album in two panes: artist and album.
		#expect(PaneMath.firstLevel(levels: 3, panes: 2) == 1)
		// The same in three: everything.
		#expect(PaneMath.firstLevel(levels: 3, panes: 3) == 0)
	}

	/// Fewer levels than panes starts at the root; the rest are placeholders.
	@Test func aShallowPathStartsAtTheRoot() {
		#expect(PaneMath.firstLevel(levels: 1, panes: 3) == 0)
		#expect(PaneMath.firstLevel(levels: 2, panes: 3) == 0)
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import CoreGraphics

/// Which of the two things a vertical swipe down the side of a film drives.
enum SideControl: Sendable {
	case brightness, volume
}

/// What a side swipe is doing right now, for the indicator to draw.
struct SideAdjustment: Equatable, Sendable {
	let control: SideControl
	/// 0 to 1.
	let level: Double
}

/// The arithmetic of the side swipes, ported from Android's
/// `VideoGestureMath.kt`. Separate from the gesture because the two things
/// most easily got wrong - which side is which, and where the zones stop - are
/// silent when wrong and cheap to test.
enum VideoGestureMath {
	/// How much of the width each side claims, leaving a dead middle fifth so
	/// a stray vertical drag across the picture does nothing at all.
	static let zoneFraction: CGFloat = 0.4

	/// The share of the height one sweep covers, short of the whole so the
	/// full range is reachable without starting or ending at an edge.
	static let fullTravel: CGFloat = 0.7

	/// The control a touch at `x` starts, or nil where a touch starts nothing.
	///
	/// The outer `edgeInset` is left to the system: the screen edges are where
	/// its own gestures begin, and anything there competes for every drag.
	static func control(atX x: CGFloat, width: CGFloat, edgeInset: CGFloat) -> SideControl? {
		guard width > 0 else { return nil }
		if x < edgeInset || x > width - edgeInset { return nil }
		if x < width * zoneFraction { return .brightness }
		if x > width * (1 - zoneFraction) { return .volume }
		return nil
	}

	/// How much of a control's range `dy` points of travel is worth. **Up
	/// increases**, hence the sign: y grows downwards and every physical
	/// control of this shape goes the other way.
	static func travelFraction(dy: CGFloat, height: CGFloat) -> Double {
		guard height > 0 else { return 0 }
		return Double(-dy / (height * fullTravel))
	}
}

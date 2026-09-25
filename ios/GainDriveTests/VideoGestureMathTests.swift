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

/// Which side is which and where the zones stop: both silent when wrong.
struct VideoGestureMathTests {
	private let width: CGFloat = 1000
	private let inset: CGFloat = 24

	@Test func theLeftSideIsBrightnessAndTheRightIsVolume() {
		#expect(VideoGestureMath.control(atX: 100, width: width, edgeInset: inset) == .brightness)
		#expect(VideoGestureMath.control(atX: 900, width: width, edgeInset: inset) == .volume)
	}

	/// A stray drag across the picture must do nothing.
	@Test func theMiddleIsDead() {
		#expect(VideoGestureMath.control(atX: 500, width: width, edgeInset: inset) == nil)
		#expect(VideoGestureMath.control(atX: 401, width: width, edgeInset: inset) == nil)
		#expect(VideoGestureMath.control(atX: 599, width: width, edgeInset: inset) == nil)
	}

	/// The edges belong to the system's own gestures.
	@Test func theEdgesAreLeftAlone() {
		#expect(VideoGestureMath.control(atX: 10, width: width, edgeInset: inset) == nil)
		#expect(VideoGestureMath.control(atX: 990, width: width, edgeInset: inset) == nil)
	}

	@Test func noWidthMeansNoControl() {
		#expect(VideoGestureMath.control(atX: 0, width: 0, edgeInset: inset) == nil)
	}

	/// Up increases, and a sweep of 70 per cent of the height is the full range.
	@Test func upIncreasesAndAPartialSweepIsTheWholeRange() {
		// Within a hair, since 100 * 0.7 is not exactly 70 in binary.
		#expect(abs(VideoGestureMath.travelFraction(dy: -70, height: 100) - 1) < 1e-9)
		#expect(abs(VideoGestureMath.travelFraction(dy: 35, height: 100) + 0.5) < 1e-9)
		#expect(VideoGestureMath.travelFraction(dy: 10, height: 0) == 0)
	}
}

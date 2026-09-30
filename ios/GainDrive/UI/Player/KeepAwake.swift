//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

#if os(macOS)
	import IOKit.pwr_mgt
#endif

extension View {
	/// Keeps the display on while this view is showing. Nothing touches the
	/// screen while a film plays, so the system has no other reason to believe
	/// anybody is there.
	func keepsDisplayAwake() -> some View {
		modifier(KeepAwake())
	}
}

/// The idle timer on iOS; a power assertion on the Mac, which has no idle timer
/// to switch off but lets an app say "a video is playing" for as long as it
/// holds the assertion.
private struct KeepAwake: ViewModifier {
	#if os(macOS)
		@State private var assertion: IOPMAssertionID = 0
	#endif

	func body(content: Content) -> some View {
		content
			.onAppear { begin() }
			.onDisappear { end() }
	}

	private func begin() {
		#if os(macOS)
			guard assertion == 0 else { return }
			var id: IOPMAssertionID = 0
			let result = IOPMAssertionCreateWithName(
				kIOPMAssertionTypePreventUserIdleDisplaySleep as CFString,
				IOPMAssertionLevel(kIOPMAssertionLevelOn),
				"GainDrive is playing a video" as CFString, &id)
			if result == kIOReturnSuccess { assertion = id }
		#else
			UIApplication.shared.isIdleTimerDisabled = true
		#endif
	}

	private func end() {
		#if os(macOS)
			guard assertion != 0 else { return }
			IOPMAssertionRelease(assertion)
			assertion = 0
		#else
			UIApplication.shared.isIdleTimerDisabled = false
		#endif
	}
}

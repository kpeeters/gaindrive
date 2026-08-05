//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Placeholder root, replaced by the tab shell in phase 1. It renders the
/// accent colour on purpose: the asset catalogue is wired up here or not at
/// all, and a blank screen would not show which.
struct RootView: View {
	var body: some View {
		VStack(spacing: 12) {
			Image(systemName: "waveform")
				.font(.system(size: 56))
				.foregroundStyle(Color.accentColor)
			Text("GainDrive")
				.font(.largeTitle.weight(.semibold))
			Text("No servers configured yet.")
				.font(.subheadline)
				.foregroundStyle(.secondary)
		}
		.frame(maxWidth: .infinity, maxHeight: .infinity)
		.background(Color(.systemBackground))
	}
}

#Preview {
	RootView()
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Placeholder root, replaced by the servers screen in phase 1 — though this
/// empty state is plausibly what that screen keeps for a fresh install.
struct RootView: View {
	var body: some View {
		VStack(spacing: 16) {
			// The asset is a vector, so this is drawn at whatever size it is
			// given rather than scaled from a raster. The artwork is a
			// full-bleed square with no corner treatment of its own, which
			// reads as a red block unless it is masked the way the platform
			// masks an app icon.
			Image("Logo")
				.resizable()
				.scaledToFit()
				.frame(width: 96, height: 96)
				.clipShape(RoundedRectangle(cornerRadius: 21, style: .continuous))
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

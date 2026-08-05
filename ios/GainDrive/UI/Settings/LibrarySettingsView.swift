//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// How browse results are presented, mirroring `LibrarySettingsScreen.kt`.
struct LibrarySettingsView: View {
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		@Bindable var settings = settings
		Form {
			Section {
				Toggle("Merge duplicate albums", isOn: $settings.mergeDuplicateAlbums)
			} footer: {
				Text(
					"""
					When two servers hold the same album, show it once. \
					Which one wins is decided by the order of the server list.
					"""
				)
			}
		}
		.navigationTitle("Library")
		.navigationBarTitleDisplayMode(.inline)
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct AppearanceSettingsView: View {
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		@Bindable var settings = settings
		Form {
			Picker("Theme", selection: $settings.themeMode) {
				ForEach(ThemeMode.allCases) { mode in
					Text(mode.label).tag(mode)
				}
			}
			.pickerStyle(.inline)
			.labelsHidden()
		}
		.navigationTitle("Appearance")
		.navigationBarTitleDisplayMode(.inline)
	}
}

#Preview {
	NavigationStack {
		AppearanceSettingsView()
			.environment(SettingsStore())
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The UI half of `ThemeMode`, kept here so `Data/` never imports SwiftUI.
extension ThemeMode {
	/// `nil` means "follow the system", which is what `preferredColorScheme`
	/// wants for the automatic case.
	var colorScheme: ColorScheme? {
		switch self {
		case .auto: nil
		case .light: .light
		case .dark: .dark
		}
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Global, not per server. The theme describes the phone, and a per-server
/// appearance would have no meaning on a merged library where rows from
/// several servers sit in the same list.
enum ThemeMode: String, CaseIterable, Codable, Sendable, Identifiable {
	case auto, light, dark

	var id: String { rawValue }

	var label: String {
		switch self {
		case .auto: "Automatic"
		case .light: "Light"
		case .dark: "Dark"
		}
	}
}

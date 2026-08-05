//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The three states every browse screen has, mirroring `ui/Load.kt`.
///
/// An enum rather than a struct with nullable fields, so **"loaded but empty"
/// and "not loaded yet" cannot be confused** — that confusion is the bug that
/// produces a flash of "nothing here" on every screen open.
enum Load<Value> {
	case loading
	case failed(String)
	case ready(Value)

	/// Android spells this `valueOrNull()`; a property is the Swift form of the
	/// same thing.
	var value: Value? {
		if case .ready(let value) = self { value } else { nil }
	}

	var isLoading: Bool {
		if case .loading = self { true } else { false }
	}
}

extension Load: Sendable where Value: Sendable {}
extension Load: Equatable where Value: Equatable {}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing

@testable import GainDrive

/// One gap closes for free relative to Android, where `CredentialCipher` is
/// untestable because `AndroidKeyStore` does not exist on the JVM — and whose
/// failure mode is "every saved password is unreadable", indistinguishable
/// from a legitimate restore onto a new device. The Keychain works in the
/// simulator, so that component is covered here.
///
/// Each test uses a fresh `ServerId` and deletes after itself, so a failure
/// cannot leave an item behind that makes the next run pass for the wrong
/// reason.
struct KeychainTests {
	@Test func storesAndReadsBack() {
		let id = ServerId()
		defer { Keychain.removePassword(for: id) }

		#expect(Keychain.setPassword("secret", for: id))
		#expect(Keychain.password(for: id) == "secret")
	}

	/// `SecItemAdd` on an existing account fails with `errSecDuplicateItem`
	/// rather than replacing, so a naive implementation silently keeps the old
	/// password after an edit — the user changes it, and nothing changes.
	@Test func overwritesAnExistingPassword() {
		let id = ServerId()
		defer { Keychain.removePassword(for: id) }

		Keychain.setPassword("first", for: id)
		#expect(Keychain.setPassword("second", for: id))
		#expect(Keychain.password(for: id) == "second")
	}

	@Test func keepsServersApart() {
		let one = ServerId()
		let two = ServerId()
		defer {
			Keychain.removePassword(for: one)
			Keychain.removePassword(for: two)
		}

		Keychain.setPassword("one", for: one)
		Keychain.setPassword("two", for: two)
		#expect(Keychain.password(for: one) == "one")
		#expect(Keychain.password(for: two) == "two")
	}

	@Test func removesCompletely() {
		let id = ServerId()
		Keychain.setPassword("secret", for: id)
		#expect(Keychain.removePassword(for: id))
		#expect(Keychain.password(for: id) == nil)
	}

	/// Removing a server whose password was never saved must still leave no
	/// configuration behind, so deleting nothing is the desired end state
	/// rather than a failure.
	@Test func removingSomethingAbsentSucceeds() {
		#expect(Keychain.removePassword(for: ServerId()))
	}

	@Test func missingPasswordReadsAsNil() {
		#expect(Keychain.password(for: ServerId()) == nil)
	}

	/// Passwords are not ASCII by rule, and a byte-mangling round trip would
	/// produce a token the server rejects with no clue why.
	@Test func survivesNonASCII() {
		let id = ServerId()
		defer { Keychain.removePassword(for: id) }

		let password = "pässwörd ✓ 日本語"
		Keychain.setPassword(password, for: id)
		#expect(Keychain.password(for: id) == password)
	}
}

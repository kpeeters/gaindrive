//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import OSLog
import Security

/// Server passwords, one Keychain item per server.
///
/// The Subsonic token scheme means the client must hold the plaintext password
/// - it computes `md5(password + salt)` afresh for each session - so it cannot
/// be hashed at rest. On Android that forced an AES key in the Keystore and a
/// hand-rolled cipher; here the Keychain *is* that, so there is no
/// `CredentialCipher` equivalent and nothing to test around one.
enum Keychain {
	/// `AfterFirstUnlock`, not `WhenUnlocked`: playback and, later, downloads
	/// have to work with the device locked, and a password the app cannot read
	/// until the user next types their passcode would stop both.
	///
	/// Deliberately **not** `ThisDeviceOnly`. That means the item travels in an
	/// encrypted device backup, so restoring onto a new phone brings the
	/// servers back configured. Android refuses that on purpose, because a
	/// restored ciphertext with no key fails at first use rather than at
	/// restore time and is indistinguishable from a genuine problem - a hazard
	/// that does not exist here, since the Keychain restores the secret itself
	/// rather than something that needs a key held elsewhere.
	///
	/// Computed rather than stored, because `CFString` is not `Sendable` and a
	/// stored static of a non-`Sendable` type is shared mutable state as far as
	/// strict concurrency is concerned. A computed property has no storage to
	/// share, so this needs no `nonisolated(unsafe)` - the escape hatch would
	/// have silenced the check rather than answered it. The `kSec…` globals
	/// themselves are fine to read: the importer already treats imported C
	/// constants as safe, which is why the `query` dictionary below compiles.
	private static var accessibility: CFString { kSecAttrAccessibleAfterFirstUnlock }

	private static let service = "org.gaindrive.ios.server-password"

	/// Every failure is logged with its `OSStatus`.
	///
	/// Not diagnostics for their own sake: a rejected write and a successful one
	/// are indistinguishable from the call site, and the *symptom* of a rejected
	/// one appears much later and somewhere else - the server list saying "no
	/// saved password", or a browse screen failing to build a client - which
	/// reads as a bug anywhere but here. `-34018` is `errSecMissingEntitlement`
	/// and means the signed app may not reach the keychain it asked for.
	private static let log = Logger(subsystem: "org.gaindrive.ios", category: "keychain")

	private static func query(for server: ServerId) -> [String: Any] {
		[
			kSecClass as String: kSecClassGenericPassword,
			kSecAttrService as String: service,
			kSecAttrAccount as String: server.value.uuidString,
			// Says "the iOS keychain, not the old file-based macOS one".
			// Ignored on iOS and already the default under Mac Catalyst, but
			// stated so the Mac build cannot quietly land in the other
			// keychain, where `kSecAttrAccessible` means nothing and the item
			// would not be the same item the iOS build wrote.
			kSecUseDataProtectionKeychain as String: true,
		]
	}

	@discardableResult
	static func setPassword(_ password: String, for server: ServerId) -> Bool {
		let data = Data(password.utf8)
		var attributes = query(for: server)

		// Update first, because SecItemAdd on an existing account fails with
		// errSecDuplicateItem rather than replacing - which would silently
		// leave the old password in place after an edit.
		let update: [String: Any] = [
			kSecValueData as String: data,
			kSecAttrAccessible as String: accessibility,
		]
		let updated = SecItemUpdate(attributes as CFDictionary, update as CFDictionary)
		if updated == errSecSuccess { return true }
		if updated != errSecItemNotFound {
			log.error("SecItemUpdate failed: \(updated)")
		}

		attributes[kSecValueData as String] = data
		attributes[kSecAttrAccessible as String] = accessibility
		let added = SecItemAdd(attributes as CFDictionary, nil)
		guard added == errSecSuccess else {
			log.error("SecItemAdd failed: \(added)")
			return false
		}
		return true
	}

	static func password(for server: ServerId) -> String? {
		var attributes = query(for: server)
		attributes[kSecReturnData as String] = true
		attributes[kSecMatchLimit as String] = kSecMatchLimitOne

		var item: CFTypeRef?
		let status = SecItemCopyMatching(attributes as CFDictionary, &item)
		guard status == errSecSuccess, let data = item as? Data else {
			// Absent is an ordinary answer - a server whose password was never
			// saved, or one restored without it. Anything else is not.
			if status != errSecItemNotFound {
				log.error("SecItemCopyMatching failed: \(status)")
			}
			return nil
		}
		return String(data: data, encoding: .utf8)
	}

	@discardableResult
	static func removePassword(for server: ServerId) -> Bool {
		let status = SecItemDelete(query(for: server) as CFDictionary)
		// Deleting something that is not there is the desired end state, not a
		// failure - removing a server whose password was never saved must not
		// leave the configuration behind.
		return status == errSecSuccess || status == errSecItemNotFound
	}
}

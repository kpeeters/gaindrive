//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

extension Error {
	/// A user-facing sentence for anything a request can fail with - the
	/// counterpart of `Throwable.userMessage()` in `net/Errors.kt`.
	///
	/// Transport failures matter as much as Subsonic ones here, and more
	/// often: the common case for a self-hosted server is that it is simply
	/// switched off. `localizedDescription` alone would say "could not connect
	/// to the server", which is true and useless - the question a user has at
	/// that moment is whether they typed the address wrong or the machine is
	/// down, and those are different sentences.
	var userMessage: String {
		if let subsonic = self as? SubsonicError {
			return subsonic.errorDescription ?? "The request failed."
		}
		if let url = self as? URLError {
			switch url.code {
			case .cannotFindHost:
				return "Cannot find that host."
			case .cannotConnectToHost:
				return "Nothing is listening at that address."
			case .timedOut:
				return "The server did not answer in time."
			case .notConnectedToInternet, .networkConnectionLost:
				return "No network connection."
			case .appTransportSecurityRequiresSecureConnection:
				return "The connection was blocked by App Transport Security."
			case .userAuthenticationRequired:
				return "The server asked for authentication the app did not provide."
			default:
				return url.localizedDescription
			}
		}
		if self is DecodingError {
			return "The server's reply was not in the expected format."
		}
		return localizedDescription
	}

	/// The user navigated away, or typed another character. **Not a failure**,
	/// and the distinction is load-bearing: a cancelled branch recorded as one
	/// would flash "this server did not answer" over the results on every
	/// keystroke in search and on every change of scope.
	///
	/// Two types have to be checked because the two layers disagree -
	/// `Task.checkCancellation` throws `CancellationError`, while `URLSession`
	/// reports the same event as `URLError.cancelled`. Android needed
	/// `runCatchingCancellable` for exactly this, having found that
	/// `runCatching` swallowed it.
	var isCancellation: Bool {
		self is CancellationError || (self as? URLError)?.code == .cancelled
	}
}

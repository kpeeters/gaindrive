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

/// Only the parsing is tested. The formatting is `Date.RelativeFormatStyle`'s
/// job and varies with locale, so asserting on its wording would be testing
/// Foundation in whichever language the machine happens to run.
struct RelativeTimeTests {
	/// 2026-08-05T11:22:33Z.
	private let reference: TimeInterval = 1_785_928_953

	@Test func parsesISO8601() throws {
		let date = try #require(parseTimestamp("2026-08-05T11:22:33Z"))
		#expect(abs(date.timeIntervalSince1970 - reference) < 1)
	}

	/// Foundation's ISO 8601 parsing is all-or-nothing about fractional
	/// seconds, where Java's `OffsetDateTime.parse` accepts either — so a
	/// second parser is needed rather than a second option.
	@Test func parsesISO8601WithFractionalSeconds() throws {
		let plain = try #require(parseTimestamp("2026-08-05T11:22:33Z"))
		let fractional = try #require(parseTimestamp("2026-08-05T11:22:33.250Z"))
		#expect(abs(fractional.timeIntervalSince(plain) - 0.25) < 0.01)
	}

	/// **The one that matters.** gaindrive emits `lastPlayed` raw from SQLite's
	/// `CURRENT_TIMESTAMP` — no `T`, no zone — and parsing only ISO would leave
	/// the entire Recents column silently blank.
	@Test func parsesTheSQLiteForm() throws {
		let sqlite = try #require(parseTimestamp("2026-08-05 11:22:33"))
		// Read as UTC, not as local time — the same instant whatever zone the
		// test machine is in, which a formatter left on the current locale
		// would get wrong by hours and only somewhere else.
		#expect(abs(sqlite.timeIntervalSince1970 - reference) < 1)
	}

	@Test func toleratesSurroundingWhitespace() {
		#expect(parseTimestamp("  2026-08-05 11:22:33 ") != nil)
	}

	@Test(arguments: ["", "   ", "not a date", "2026-13-45", "1785929000"])
	func rejectsWhatItCannotRead(text: String) {
		#expect(parseTimestamp(text) == nil)
	}

	@Test func absentIsNil() {
		#expect(parseTimestamp(nil) == nil)
		#expect(relativeTime(nil) == nil)
	}

	@Test func producesSomethingForAValidTimestamp() {
		#expect(relativeTime("2026-08-05 11:22:33") != nil)
	}
}

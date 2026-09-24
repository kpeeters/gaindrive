//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// "3 hours ago", from whichever of the API's timestamp formats arrived.
///
/// The formatting is the easy half. The **parsing** accepts three shapes, one
/// more than Android needs:
///
/// * ISO 8601, which is what every timestamp in the API uses…
/// * …except with fractional seconds, because Foundation's ISO 8601 parsing is
///   all-or-nothing about them where Java's `OffsetDateTime.parse` is not; and
/// * `yyyy-MM-dd HH:mm:ss` in UTC, because gaindrive emits `lastPlayed` raw
///   from SQLite's `CURRENT_TIMESTAMP` - no `T`, no zone - and parsing only ISO
///   would leave the entire Recents column silently blank.
func relativeTime(_ timestamp: String?) -> String? {
	guard let date = parseTimestamp(timestamp) else { return nil }
	return date.formatted(.relative(presentation: .named))
}

func parseTimestamp(_ timestamp: String?) -> Date? {
	guard let timestamp else { return nil }
	let text = timestamp.trimmingCharacters(in: .whitespacesAndNewlines)
	guard !text.isEmpty else { return nil }

	// `Date.ISO8601FormatStyle` rather than `ISO8601DateFormatter`: the strategy
	// is a value type and therefore `Sendable`, where the formatter is a class
	// that cannot be held in a global under strict concurrency. The alternative
	// was `nonisolated(unsafe)`, which would have asserted thread-safety the
	// compiler cannot check instead of using the type that has it.
	if let date = try? Date(text, strategy: .iso8601) { return date }
	if let date = try? Date(text, strategy: fractionalISO) { return date }
	return parseSQLiteTimestamp(text)
}

/// Foundation's ISO 8601 parsing is all-or-nothing about fractional seconds
/// where Java's `OffsetDateTime.parse` is not, so this is a second strategy
/// rather than a second option. gaindrive never emits them - its `iso8601()`
/// only inserts a `T` and appends a `Z` - but other Subsonic servers do.
private let fractionalISO = Date.ISO8601FormatStyle(includingFractionalSeconds: true)

/// `yyyy-MM-dd HH:mm:ss` in UTC, which is what gaindrive sends for
/// `lastPlayed`: SQLite's `CURRENT_TIMESTAMP`, raw, with no `T` and no zone.
/// Parsing only ISO would leave the whole Recents column silently blank.
///
/// Parsed by hand rather than with a `DateFormatter` - which is the other class
/// that cannot live in a global - and strictly, which a formatter is not: a
/// fixed machine format has no reason to accept month 13, and `Calendar` would
/// quietly roll it over into the next year rather than refusing it.
private func parseSQLiteTimestamp(_ text: String) -> Date? {
	let halves = text.split(separator: " ")
	guard halves.count == 2 else { return nil }
	let date = halves[0].split(separator: "-")
	let time = halves[1].split(separator: ":")
	guard date.count == 3, time.count == 3,
		let year = Int(date[0]), let month = Int(date[1]), let day = Int(date[2]),
		let hour = Int(time[0]), let minute = Int(time[1]), let second = Int(time[2]),
		1...12 ~= month, 1...31 ~= day,
		0...23 ~= hour, 0...59 ~= minute, 0...60 ~= second
	else {
		return nil
	}

	var calendar = Calendar(identifier: .gregorian)
	// Read as UTC. Left on the device's zone this would be wrong by hours, and
	// only for people who are not on GMT - the kind of bug that never shows up
	// where it was written.
	calendar.timeZone = TimeZone(identifier: "UTC") ?? .gmt
	return calendar.date(
		from: DateComponents(
			year: year, month: month, day: day, hour: hour, minute: minute, second: second))
}

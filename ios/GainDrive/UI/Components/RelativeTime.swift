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
///   from SQLite's `CURRENT_TIMESTAMP` — no `T`, no zone — and parsing only ISO
///   would leave the entire Recents column silently blank.
func relativeTime(_ timestamp: String?) -> String? {
	guard let date = parseTimestamp(timestamp) else { return nil }
	return date.formatted(.relative(presentation: .named))
}

func parseTimestamp(_ timestamp: String?) -> Date? {
	guard let timestamp else { return nil }
	let text = timestamp.trimmingCharacters(in: .whitespacesAndNewlines)
	guard !text.isEmpty else { return nil }

	if let date = isoParser.date(from: text) { return date }
	if let date = isoFractionalParser.date(from: text) { return date }
	return sqliteParser.date(from: text)
}

private let isoParser: ISO8601DateFormatter = {
	let parser = ISO8601DateFormatter()
	parser.formatOptions = [.withInternetDateTime]
	return parser
}()

private let isoFractionalParser: ISO8601DateFormatter = {
	let parser = ISO8601DateFormatter()
	parser.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
	return parser
}()

/// Pinned to `en_US_POSIX` and UTC. A fixed-format parser on the user's own
/// locale is the classic way to produce a parser that works everywhere except
/// on the devices of people who do not use Gregorian dates, and reading the
/// value as local time would put every play a few hours out.
private let sqliteParser: DateFormatter = {
	let parser = DateFormatter()
	parser.locale = Locale(identifier: "en_US_POSIX")
	parser.timeZone = TimeZone(identifier: "UTC")
	parser.dateFormat = "yyyy-MM-dd HH:mm:ss"
	return parser
}()

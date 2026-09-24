//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The song boundaries inside one long recording.
///
/// A concert, a DJ set or a fetched mixtape is one file holding a dozen songs,
/// and these are what let a client say where each starts. They live in a sidecar
/// text file beside the media on the server, indexed by its scan, and reach us
/// through `getChapters`, `getAlbumChapters` and a `chapter` array on the search
/// endpoints. See `doc/api.toml` at the repository root.
///
/// These are the one library concept with **no `ItemRef`**, against the rule the
/// rest of `Library.swift` follows, and that is the server's rule rather than an
/// omission here: a chapter has no id of its own, cannot be streamed, starred or
/// queued, and is acted on by playing the song it is inside and seeking to its
/// start. Hence a file of their own, where the exception can be argued once.
///
/// The client is read-only. `getChapters` also reports whether the caller may
/// save markers; it is deliberately not carried into these models, because a
/// field nothing reads is the promise of an editor that does not exist.
struct Chapter: Hashable, Sendable, Codable {
	/// Position in the list, from 1, as the server numbers it after its sort.
	let index: Int
	/// Seconds from the start of the file, to the millisecond.
	///
	/// Kept as sent rather than rounded to whole seconds: the server reports it
	/// precisely so that a client reading a list and writing it back is a fixed
	/// point, and truncating here would be the first step in losing that.
	let start: Double
	/// Whole seconds until the next marker, or until the end of the file.
	///
	/// Derived by the server, which is the only side that knows the item's own
	/// length. **0** when that span is not positive, which is what a marker past
	/// the end of the file gives and what two markers on one timestamp give.
	/// Neither is an error - a hand-typed file is allowed to be wrong.
	let duration: Int
	/// The title on that line, which may be empty. See `displayName`.
	let name: String

	/// What to draw for a marker somebody left bare.
	///
	/// The placeholder is the client's, never the server's: it reports an empty
	/// name as empty on purpose, so that a client saving back what it read
	/// cannot write "Chapter 3" into a line deliberately left blank.
	var displayName: String { chapterLabel(name, index) }
}

/// Everything `getChapters` answers about one item.
struct ChapterList: Hashable, Sendable {
	var chapters: [Chapter] = []
	var source: ChapterSource = .none

	var isEmpty: Bool { chapters.isEmpty }
}

/// Where a marker list came from.
enum ChapterSource: String, Sendable {
	/// A sidecar file beside the media. Wins outright when both exist.
	case sidecar
	/// Read out of the container itself, which is reachable for a video only.
	/// Worth telling the reader, because such a list is **not** in the scan's
	/// index and so does not appear in the album listing.
	case container
	/// No sidecar and nothing in the container, or the file could not be read.
	case none

	static func from(wire: String?) -> ChapterSource {
		ChapterSource(rawValue: wire ?? "") ?? .none
	}
}

/// One chaptered item in an album folder, as `getAlbumChapters` lists it.
struct ChapteredRecording: Hashable, Sendable {
	let ref: ItemRef
	let title: String
	let chapters: [Chapter]
}

/// A marker whose title matched a search.
///
/// A different shape from `Chapter` rather than the same one with holes, and the
/// difference is not merely a missing field. A search hit *is* its own context -
/// it names the recording, the album and the artist, because a marker means
/// nothing without knowing which concert it is in - while a `Chapter` is always
/// read alongside the item the caller already holds. It also carries no
/// duration, which cannot be known without the rest of the list, so folding the
/// two together would make `duration == 0` mean "no next marker" in one case and
/// "unknowable" in the other.
struct ChapterHit: Identifiable, Hashable, Sendable {
	/// The song this marker is inside. Play *that* and seek to `start`.
	let songRef: ItemRef
	/// That song's album folder, so a client can open the listing it sits in.
	let albumRef: ItemRef?
	let index: Int
	let start: Double
	let name: String
	/// The title of the song or film the marker is inside.
	let trackTitle: String
	let albumTitle: String
	let artistName: String

	var displayName: String { chapterLabel(name, index) }
	var id: String { "\(songRef.encoded)#\(index)" }
}

private func chapterLabel(_ name: String, _ index: Int) -> String {
	name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
		? "Chapter \(index)" : name
}

//	── Navigating a marker list ────────────────────────────────────────────────
//
//	Pure functions over a sorted list, so the rules can be tested without a
//	player. Ported from the web client rather than reinvented, as Android's are:
//	two clients disagreeing about which song is playing would be worse than
//	either rule on its own.

/// A marker counts as reached slightly early.
///
/// Without the tolerance, seeking to a marker frequently lands a few
/// milliseconds short of it - the player rounds, and a re-encoded stream starts
/// at the nearest keyframe - so the list would highlight the *previous* song for
/// a moment after jumping to one.
let chapterTolerance: Double = 0.25

/// How far into a chapter "previous" stops meaning "restart this one".
///
/// The behaviour every physical transport has, and the reason a viewer can press
/// it twice to go back a song.
let chapterRestartWindow: Double = 3

extension Array where Element == Chapter {
	/// Index of the marker being played at `position`, or nil before the first.
	func currentIndex(at position: Double) -> Int? {
		var found: Int?
		for (i, chapter) in enumerated() {
			if chapter.start <= position + chapterTolerance { found = i } else { break }
		}
		return found
	}

	/// The next marker after `position`, or nil once past the last one.
	func next(after position: Double) -> Chapter? {
		first { $0.start > position + chapterTolerance }
	}

	/// Where "previous chapter" should seek to from `position`.
	///
	/// The start of the chapter being played, unless we are already at it - and
	/// 0 when nothing has started yet, so the control is never inert.
	func previousTarget(from position: Double) -> Double {
		guard let i = currentIndex(at: position) else { return 0 }
		let start = self[i].start
		return (i == 0 || position - start > chapterRestartWindow) ? start : self[i - 1].start
	}
}

/// `h:mm:ss` where the recording is long enough to need it, `m:ss` otherwise.
///
/// **Not `formatDuration`**, for two reasons that both bite on the first marker
/// of every list: that one answers `--:--` for zero, which is a length nobody
/// knows rather than the start of a file, and it takes whole seconds. This one
/// truncates a position - a marker at 90.9 s is at 1:30, which is where seeking
/// to it lands, and rounding up to 1:31 would name a second the player never
/// sits at.
func formatChapterTime(_ seconds: Double) -> String {
	let total = max(0, Int(seconds))
	let hours = total / 3600
	let minutes = (total % 3600) / 60
	let secs = total % 60
	return hours > 0
		? String(format: "%d:%02d:%02d", hours, minutes, secs)
		: String(format: "%d:%02d", minutes, secs)
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The album screen's track listing, flattened to one entry per row.
///
/// It exists because a chaptered recording is not one row. Its markers stand in
/// for it - a concert is one file holding a dozen songs, and listing it as
/// `concert.mkv` names the file rather than the music - so one song can produce
/// many rows, and the screen's "one row per song" shape no longer holds.
///
/// Pure, so the rules below can be tested without a screen or a player.
struct AlbumListRow: Identifiable, Hashable {
	enum Kind: Hashable {
		/// An ordinary track. The number is the one to draw, already resolved
		/// against the positional fallback.
		case track(number: Int?)
		case marker(Chapter)
	}

	/// The song this row plays. For a marker that is the *recording*, which is
	/// the only thing here with an id anything can stream.
	let song: Song
	/// Where that song is in the album, which is what playback queues from.
	/// **Into the flat song list, never into a disc slice** - the same trap
	/// `AlbumDetailView` documents.
	let queueIndex: Int
	let kind: Kind
	/// A heading naming the recording, drawn above this row.
	///
	/// Carried by the row rather than emitted as one of its own so that every
	/// row stays individually identified: the server allows a thousand markers
	/// on one item, and interleaving heading entries would make the list's
	/// identity depend on how many of them there were.
	let recordingHeading: String?

	var id: String {
		switch kind {
		case .track: return song.ref.encoded
		case .marker(let chapter): return "c/\(song.ref.encoded)/\(chapter.index)"
		}
	}

	/// Which disc this row belongs to, so the view can group without unpicking
	/// the flattening again.
	var disc: Int { song.discNumber ?? 1 }
}

/// Flattens an album's songs, replacing each chaptered recording with its
/// markers.
///
/// Two rules, both of them the web client's:
///
/// * a heading naming the recording appears only when **more than one** item in
///   the album has markers, by the same argument the disc heading follows: a
///   heading says *which* group a row belongs to, and one group needs none.
///   Counted over what will actually be drawn, so an empty entry cannot conjure
///   one.
/// * an album whose tracks are all numbered 0 or 1 carries no usable numbering,
///   so its rows are numbered by position instead. Album-wide, matching
///   `web/app.js` and the Android client, so the three read the same.
///
/// `chapters` empty produces precisely the listing this screen drew before
/// chapters existed, which is what makes the feature free for the albums that
/// have none.
func albumListRows(songs: [Song], chapters: [ItemRef: [Chapter]]) -> [AlbumListRow] {
	let useSeq = songs.allSatisfy { ($0.track ?? 0) <= 1 }
	let multiChaptered = songs.filter { !(chapters[$0.ref] ?? []).isEmpty }.count > 1

	var rows: [AlbumListRow] = []
	for (index, song) in songs.enumerated() {
		let markers = chapters[song.ref] ?? []
		guard !markers.isEmpty else {
			rows.append(
				AlbumListRow(
					song: song, queueIndex: index,
					kind: .track(number: useSeq ? index + 1 : song.track),
					recordingHeading: nil))
			continue
		}
		for (position, chapter) in markers.enumerated() {
			rows.append(
				AlbumListRow(
					song: song, queueIndex: index, kind: .marker(chapter),
					// The heading rides on the recording's first marker, which
					// is the row it introduces.
					recordingHeading: (multiChaptered && position == 0) ? song.title : nil))
		}
	}
	return rows
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The merge rules for "All servers" scope, chosen to be predictable rather
/// than clever. Pure functions, mirroring `data/Merge.kt`.
///
/// **Every `perServer` argument must arrive in registry order.** That order is
/// the tie-break for which server's ref, artwork and index letter a merged row
/// takes, which is how preference between two servers holding the same album is
/// expressed by moving a row in Settings rather than by a separate
/// favourite-server setting that could disagree with it.
///
/// There is no `songs` function, and its absence is the rule: a track list
/// always comes from one album on one server.
enum Merge {

	/// Collapses artists whose names match after case-folding and trimming.
	///
	/// The first contributor in registry order wins the ref, the artwork and
	/// the index letter; only the album count and the ref list grow. Starred
	/// anywhere is starred - the alternative is a star that depends on which
	/// server happened to answer first.
	static func artists(perServer: [[Artist]]) -> [Artist] {
		// One server is not a merge. Returning it untouched also preserves the
		// order it chose, which for search results is relevance.
		if perServer.count == 1 { return perServer[0] }

		var order: [String] = []
		var byKey: [String: Artist] = [:]

		for list in perServer {
			for artist in list {
				let key = matchKey(artist.name)
				guard let existing = byKey[key] else {
					order.append(key)
					byKey[key] = artist
					continue
				}
				byKey[key] = Artist(
					ref: existing.ref,
					name: existing.name,
					albumCount: existing.albumCount + artist.albumCount,
					coverArt: existing.coverArt ?? artist.coverArt,
					starredAt: existing.starredAt ?? artist.starredAt,
					refs: existing.refs + artist.refs
				)
			}
		}
		return order.compactMap { byKey[$0] }
	}

	/// Merges the index buckets of several servers.
	///
	/// Artists are keyed across **all** buckets, not within each: two servers
	/// filing the same artist under different letters would otherwise produce
	/// two rows, which is exactly what merging exists to prevent. The merged
	/// artist keeps the first contributor's letter.
	static func artistIndexes(perServer: [[ArtistIndex]]) -> [ArtistIndex] {
		if perServer.count == 1 { return perServer[0] }

		// Which bucket each merged artist belongs in, decided by whoever
		// contributed it first.
		var labelForKey: [String: String] = [:]
		for list in perServer {
			for bucket in list {
				for artist in bucket.artists where labelForKey[matchKey(artist.name)] == nil {
					labelForKey[matchKey(artist.name)] = bucket.label
				}
			}
		}

		let merged = artists(perServer: perServer.map { $0.flatMap(\.artists) })
		var grouped: [String: [Artist]] = [:]
		for artist in merged {
			let label = labelForKey[matchKey(artist.name)] ?? "#"
			grouped[label, default: []].append(artist)
		}

		return grouped.keys.sorted(by: labelPrecedes).map { label in
			ArtistIndex(
				label: label,
				// Interleaving two already-sorted lists by hand is not sorted,
				// so the bucket is sorted rather than concatenated.
				artists: grouped[label, default: []].sorted {
					matchKey($0.name) < matchKey($1.name)
				})
		}
	}

	/// Every server's category buckets flattened into one alphabetical list,
	/// same-named sections collapsed across servers.
	///
	/// Built on `artistIndexes`, which already keys artists by name across all
	/// buckets - a "Film" section on two servers becomes one row carrying both
	/// refs, the first contributor in registry order winning the ref. The
	/// explicit sort is load-bearing: the single-server shortcut in that
	/// function returns the server's own bucket order untouched, and the
	/// merged list draws these under one header where only alphabetical reads
	/// as an order at all. `matchKey`, not `caseInsensitiveCompare`, so the
	/// order agrees with how the buckets themselves are sorted.
	static func categories(perServer: [[ArtistIndex]]) -> [Artist] {
		artistIndexes(perServer: perServer)
			.flatMap(\.artists)
			.sorted { matchKey($0.name) < matchKey($1.name) }
	}

	/// Collapses albums that two *different* servers both hold.
	///
	/// Matched on artist and title through `matchKey`, which keeps letters and
	/// digits and discards everything else - that is what makes "Vol. 2" match
	/// "Vol 2", and curly quotes match straight ones.
	///
	/// Deliberately **not** matched on year: a remaster disagrees about it
	/// between servers, which would split exactly the pairs worth collapsing.
	///
	/// Across servers only. Two same-titled albums on one server are two
	/// albums - separately filed editions - and collapsing them would hide one.
	static func albums(_ albums: [Album]) -> [Album] {
		guard Set(albums.map(\.ref.server)).count > 1 else { return albums }

		var order: [String] = []
		var byKey: [String: Album] = [:]

		for album in albums {
			let key = "\(matchKey(album.artistName))\u{1F}\(matchKey(album.title))"
			guard let existing = byKey[key] else {
				order.append(key)
				byKey[key] = album
				continue
			}
			// Already merged with something from the same server: two editions
			// filed separately, not a duplicate. Keep both.
			guard !existing.refs.contains(where: { $0.server == album.ref.server }) else {
				order.append(key + album.ref.encoded)
				byKey[key + album.ref.encoded] = album
				continue
			}
			byKey[key] = Album(
				ref: existing.ref,
				title: existing.title,
				artistName: existing.artistName,
				artistRef: existing.artistRef ?? album.artistRef,
				songCount: max(existing.songCount, album.songCount),
				// `max`, not "the winner's": the field is absent on some
				// listings, so a zero means "not said" and must never
				// overwrite a real count from the other copy.
				videoCount: max(existing.videoCount, album.videoCount),
				duration: max(existing.duration, album.duration),
				year: existing.year ?? album.year,
				genre: existing.genre ?? album.genre,
				coverArt: existing.coverArt ?? album.coverArt,
				starredAt: existing.starredAt ?? album.starredAt,
				refs: existing.refs + album.refs
			)
		}
		return order.compactMap { byKey[$0] }
	}

	/// Letters and digits, case-folded. Two servers describing the same record
	/// rarely punctuate it the same way.
	///
	/// A title made entirely of punctuation keeps its raw form, or every such
	/// title would collapse into one row.
	/// The same buckets, **without merging the artists inside them**.
	///
	/// For the uploads slice, where two accounts' identically named folders are
	/// not the same artist. The owner buckets exist precisely to keep them
	/// apart - `personal=*` groups the response by username rather than by
	/// first letter - so collapsing rows by name across servers would file one
	/// person's upload under another's heading, which is the one thing that
	/// listing has to get right.
	static func concatenatedIndexes(perServer: [[ArtistIndex]]) -> [ArtistIndex] {
		if perServer.count == 1 { return perServer[0] }

		var grouped: [String: [Artist]] = [:]
		for bucket in perServer.flatMap({ $0 }) {
			grouped[bucket.label, default: []] += bucket.artists
		}
		return grouped.keys.sorted(by: labelPrecedes).map { label in
			ArtistIndex(
				label: label,
				// Concatenating two already-sorted lists is not sorted.
				artists: grouped[label, default: []].sorted {
					matchKey($0.name) < matchKey($1.name)
				})
		}
	}

	static func matchKey(_ raw: String) -> String {
		let stripped = raw.lowercased().unicodeScalars
			.filter { CharacterSet.alphanumerics.contains($0) }
		let key = String(String.UnicodeScalarView(stripped))
		return key.isEmpty ? raw.trimmingCharacters(in: .whitespacesAndNewlines) : key
	}

	/// Letters first, `#` at the end of the rail rather than where its code
	/// point would put it - which is before "A", and looks like a mistake.
	private static func labelPrecedes(_ lhs: String, _ rhs: String) -> Bool {
		let lhsIsHash = lhs == "#"
		let rhsIsHash = rhs == "#"
		if lhsIsHash != rhsIsHash { return rhsIsHash }
		return lhs.localizedCaseInsensitiveCompare(rhs) == .orderedAscending
	}
}

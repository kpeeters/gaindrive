//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Turning an identifier into one safe path component.
///
/// **One definition, because both stores have to agree with themselves.** The
/// audio store writes a file under an encoded name and later finds it by
/// building the same name again; the mirror does the same. A second copy of
/// this rule would not fail loudly — it would make a file unfindable by the
/// code that wrote it, which reads as a cache that never hits.
///
/// Hyphens, dots and underscores are left alone so a UUID directory and an
/// integer filename read as themselves; an escaped hyphen in every server
/// directory would be noise in every `ls` for no gain. What matters is only
/// that the transformation is the same in both directions.
///
/// Not because gaindrive needs it — its ids are integers and its server ids are
/// UUIDs. A Subsonic id is a string somebody else chose, and one containing a
/// slash would otherwise write outside the directory it was meant for.
enum FileNames {
	static func component(_ raw: String) -> String {
		raw.addingPercentEncoding(withAllowedCharacters: safe) ?? raw
	}

	private static let safe: CharacterSet = {
		var set = CharacterSet.alphanumerics
		set.insert(charactersIn: "-._")
		return set
	}()
}

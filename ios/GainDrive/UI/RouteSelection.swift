//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

//	Apart from `Navigation.swift` so that file stays Foundation-only: `Route`
//	is plain data, and this is the one part of it that speaks SwiftUI.

extension Route {
	/// An albums pane's selection, as the path's next level.
	///
	/// `AlbumsView` selects `Album` values - its rows need the album itself -
	/// while the path holds `Route`s, so `chosen` keeps the album that was
	/// picked, and the selection reads it back only while the path still shows
	/// that album. Used where an artist's albums sit inside another tab's
	/// panes: Search and Recents. Library keeps its own selections.
	static func albumSelection(
		_ path: Binding<[Route]>, level: Int, chosen: Binding<Album?>
	) -> Binding<Album?> {
		Binding(
			get: {
				guard level < path.wrappedValue.count,
					case .album(let ref, _, _, _) = path.wrappedValue[level],
					let album = chosen.wrappedValue, album.ref == ref
				else { return nil }
				return album
			},
			set: { album in
				chosen.wrappedValue = album
				var next = Array(path.wrappedValue.prefix(level))
				if let album { next.append(.album(album.ref, title: album.title)) }
				path.wrappedValue = next
			})
	}
}

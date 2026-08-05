//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Where a browse screen can push to.
///
/// These hold `ItemRef` **values**, not the `<serverId>/<itemId>` string.
/// Android encodes because `navigation-compose` routes are strings and Media3
/// hands the id back with no other context; a SwiftUI path holds values, and
/// `ARCHITECTURE.md` scopes the string encoding to cache keys, pin records and
/// anything persisted. A navigation path in memory is none of those, and going
/// through the string would be a second serialisation for nothing.
enum Route: Hashable {
	/// A **list** of refs, because a merged artist row stands for the same
	/// artist on several servers, each with its own id, and the albums screen
	/// has to ask all of them.
	///
	/// The name travels alongside so the title bar has something to show before
	/// the body has loaded.
	case albums(artists: [ItemRef], name: String)
	case album(ItemRef, title: String)
	case playlist(ItemRef, name: String)
}

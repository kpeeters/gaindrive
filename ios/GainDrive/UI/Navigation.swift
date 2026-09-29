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
	/// `fromCategories` says the row was tapped in a **categories** slice, in
	/// which case the header draws no portrait and expects no biography: Film
	/// and Series are not performers, and the server knows it - see
	/// `is_category_folder()`. Without it the avatar sits as a placeholder for
	/// ever while the fetch retries a 404.
	///
	/// `fromUploads` says the row was tapped in the **uploads** listing, which
	/// keys the album sort to that listing's own preference.
	///
	/// Both travel on the route rather than being read from module state
	/// because **an artist reference says which folder, never which listing it
	/// was reached through** - and both are defaulted, so a section reached
	/// from search keeps today's placeholder rather than having the answer
	/// guessed.
	case albums(artists: [ItemRef], name: String, fromCategories: Bool, fromUploads: Bool)
	/// `autoPlay` names a track to start once the listing has arrived, which is
	/// how a hit in Recents or Search is played.
	///
	/// **Going through the album is not a detour.** `nativeSeek` is false on
	/// every song `search3`, `getStarred2`, `getPlaylist` and `getRecentSongs`
	/// return, because those queries do not select the codec columns the server
	/// computes it from - so playing such a hit where it stands would send a
	/// perfectly remuxable film down the re-encode path every time. Read again
	/// through `getAlbum` it carries the flag, and lands on the right tier.
	/// Android's `Route.Album.autoPlayRef` exists for the same reason.
	///
	/// `autoPlayAt` starts that track partway in, in seconds, and exists for one
	/// caller: a chapter match in search. A marker has no id anything can
	/// stream, so acting on one means opening the album its recording sits in
	/// and starting that recording partway through - which is the same detour
	/// `autoPlay` already takes, for the same reason, with an offset added.
	case album(ItemRef, title: String, autoPlay: ItemRef?, autoPlayAt: Double)
	case playlist(ItemRef, name: String)

	/// The ordinary way in, from a listing that is neither categories nor
	/// uploads.
	static func albums(artists: [ItemRef], name: String) -> Route {
		.albums(artists: artists, name: name, fromCategories: false, fromUploads: false)
	}

	/// Open the album and start nothing - every browse screen's way in.
	///
	/// A static member rather than a default on the associated value, which
	/// Swift does not allow. The two differ in argument labels, so they are
	/// distinct signatures and no existing call site had to change.
	static func album(_ ref: ItemRef, title: String) -> Route {
		.album(ref, title: title, autoPlay: nil, autoPlayAt: 0)
	}

	/// Open the album and start a track from its beginning - a hit in Recents,
	/// Search or Starred.
	static func album(_ ref: ItemRef, title: String, autoPlay: ItemRef?) -> Route {
		.album(ref, title: title, autoPlay: autoPlay, autoPlayAt: 0)
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A cover to fetch, and the name to file it under.
///
/// The two are separate because **the URL is not stable across launches**. It
/// carries `t=` and `s=`, and the salt is regenerated every session, so a
/// URL-keyed cache - `URLCache`, or any off-the-shelf image loader's - sees a
/// different key for the same bytes on every cold start and re-downloads the
/// entire grid. Naming what the bytes *are* instead is what makes the disk
/// cache worth having.
struct CoverSource: Hashable, Sendable {
	let url: URL
	/// `<serverId>/<coverId>@<size>` plus `#<index>` for an extra image.
	/// Contains no salt, and therefore survives a relaunch.
	let cacheKey: String
}

/// Cover art URLs for every configured server.
///
/// Built once per screen load rather than per row: resolving a server means
/// reading the registry and the Keychain, which is not something to do inside
/// a list cell.
struct CoverUrls: Sendable {
	private let clients: [ServerId: SubsonicClient]

	init(clients: [ServerId: SubsonicClient]) {
		self.clients = clients
	}

	/// `size` matters as much as the id: it is part of the key, so asking for
	/// one consistent size per context is what makes the cache hit. Requesting
	/// 144 px for a 48 pt thumbnail and 800 px for a hero is deliberate.
	///
	/// `index` selects one of the extra images `getAlbumImages` counts; 0 is
	/// the main cover.
	func source(_ ref: ItemRef?, size: Int, index: Int = 0) -> CoverSource? {
		guard let ref, let client = clients[ref.server] else { return nil }
		var parameters = ["id": ref.id, "size": String(size)]
		if index > 0 { parameters["index"] = String(index) }
		let key = "\(ref.server.value.uuidString)/\(ref.id)@\(size)" + (index > 0 ? "#\(index)" : "")
		return CoverSource(
			url: client.url("getCoverArt", parameters: parameters),
			cacheKey: key)
	}
}

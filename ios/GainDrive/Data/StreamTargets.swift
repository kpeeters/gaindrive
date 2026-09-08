//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Resolves what to ask a server for, for one track.
///
/// **One resolver, two callers**, and that is the point of it existing rather
/// than each doing the same three lines. `PlayerConnection` builds a URL to
/// play and `PinRepository` builds a URL to download, and if those two ever
/// disagreed the downloader would store bytes under one key while playback
/// looked for another — a track that downloads successfully and then streams
/// anyway, with nothing to say why.
///
/// `StreamUrls.target` stays the pure part: this only gathers the client and
/// that track's own account ceiling, and nothing here closes over a "current"
/// server, which is what lets a queue span two of them.
@MainActor
struct StreamTargets {
	let registry: ServerRegistry
	let settings: SettingsStore
	let accounts: Accounts

	/// A film. See `StreamUrls.video` for why it carries neither `format` nor
	/// `maxBitRate`, and why the account ceiling is not applied.
	func video(for song: Song) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: song.ref.server) else {
			return nil
		}
		return StreamUrls.video(for: song, client: client)
	}

	func target(for ref: ItemRef) async -> StreamTarget? {
		let clients = registry.clientsSnapshot()
		guard let client = clients.client(for: ref.server) else { return nil }
		let cap = await accounts.cap(for: ref.server, using: clients)
		return StreamUrls.target(
			for: ref, client: client, wanted: settings.audioQuality, accountCap: cap)
	}
}

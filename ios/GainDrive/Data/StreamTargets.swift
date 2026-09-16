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
	/// `maxBitRate`, why the account ceiling is not applied, and why `playable`
	/// defaults to empty rather than being filled in here.
	func video(
		for song: Song,
		transcoded: Bool = false,
		playable: Set<String> = []
	) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: song.ref.server) else {
			return nil
		}
		return StreamUrls.video(
			for: song, client: client, transcoded: transcoded,
			playable: playable)
	}

	/// `playable` is a **function** rather than a set, unlike `video(for:)`
	/// above, because the set follows from a quality this resolver reads and
	/// the caller does not have. A second reading of the setting at the call
	/// site would be two readings that can disagree — the thing
	/// `PinRepository` already refuses to do with `isVideo`.
	///
	/// It defaults to declaring nothing, and the default is the safety.
	/// **The cast route reaches this same resolver** — `CastUrls.audio(for:)`
	/// calls it — and a receiver declares none of this, so the declaration
	/// belongs at the call sites that read the bytes themselves and nowhere
	/// else. `LocalEngine` and `PinRepository` pass
	/// `avfoundationPlayable(for:)`; everything else takes the default.
	func target(
		for ref: ItemRef,
		playable: (AudioQuality) -> Set<String> = { _ in [] }
	) async -> StreamTarget? {
		await target(for: ref, wanted: settings.audioQuality, playable: playable)
	}

	/// A target at a stated quality rather than the configured one.
	///
	/// **The ceiling still applies**, which is the whole reason this is here
	/// rather than at the call site: the account's `maxBitRate` belongs to that
	/// track's server, and `AudioQuality.cappedBy` models what the server would
	/// actually send — including turning a request for the original into MP3 at
	/// the cap. A caller that skipped this would build a cache key claiming
	/// bytes it is not going to receive.
	///
	/// One caller: the cast route, which must not send a container a receiver
	/// cannot decode however the local setting is spelled.
	func target(
		for ref: ItemRef, wanted: AudioQuality,
		playable: (AudioQuality) -> Set<String> = { _ in [] }
	) async -> StreamTarget? {
		let clients = registry.clientsSnapshot()
		guard let client = clients.client(for: ref.server) else { return nil }
		// Resolved from `wanted`, which is the quality **before** the ceiling
		// below is applied: `cappedBy` turns a request for the original into
		// mp3 at the cap, and reading the capped value would silently stop
		// every capped account declaring the lossless half. Computed before
		// the await as well, so a non-escaping closure never has to survive a
		// suspension point.
		let declared = playable(wanted)
		let cap = await accounts.cap(for: ref.server, using: clients)
		return StreamUrls.target(
			for: ref, client: client, wanted: wanted, accountCap: cap,
			playable: declared)
	}
}

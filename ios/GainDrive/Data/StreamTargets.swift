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
/// looked for another - a track that downloads successfully and then streams
/// anyway, with nothing to say why.
///
/// `StreamUrls.target` stays the pure part: this only gathers that track's
/// client, and nothing here closes over a "current" server, which is what lets
/// a queue span two of them.
@MainActor
struct StreamTargets {
	let registry: ServerRegistry
	let settings: SettingsStore

	/// A film. See `StreamUrls.video` for why it carries neither `format` nor
	/// `maxBitRate`, and why `files` defaults to the server's own set rather than being filled in here.
	func video(
		for song: Song,
		transcoded: Bool = false,
		files: Set<String> = StreamUrls.serverFileContainers
	) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: song.ref.server) else {
			return nil
		}
		return StreamUrls.video(
			for: song, client: client, transcoded: transcoded, files: files)
	}

	/// A film's playlist, which is what a film download fetches.
	func hls(for ref: ItemRef) -> URL? {
		guard let client = registry.clientsSnapshot().client(for: ref.server) else { return nil }
		return StreamUrls.hls(id: ref.id, client: client)
	}

	/// `playable` defaults to declaring nothing, and the default is the
	/// safety. **The cast route reaches this same resolver** -
	/// `CastUrls.audio(for:)` calls it - and a receiver declares none of this,
	/// so the declaration belongs at the call sites that read the bytes
	/// themselves and nowhere else. `LocalEngine` and `PinRepository` pass
	/// `avfoundationAudio`; everything else takes the default.
	func target(for ref: ItemRef, playable: Set<String> = []) -> StreamTarget? {
		target(for: ref, wanted: settings.audioQuality, playable: playable)
	}

	/// A video's **soundtrack**: the configured quality through
	/// `forVideoAudio`, and nothing declared.
	///
	/// Nothing declared because the server guards the audio declaration on
	/// the song not being a video, and a parameter it drops invites the next
	/// reader to believe it does something. The one resolver for both callers
	/// that want a film's sound - local playback with "audio only" on, and a
	/// receiver with no screen - so the two cannot ask for different bytes.
	func soundtrack(for ref: ItemRef) -> StreamTarget? {
		target(for: ref, wanted: settings.audioQuality.forVideoAudio())
	}

	/// A video's soundtrack copied out as it stands, in the `format` the
	/// browse response named for it, for a receiver told `mime` up front.
	///
	/// No `maxBitRate`: the server only copies when none is given, since a
	/// video's stored bitrate is the container's and says nothing about its
	/// audio.
	func soundtrackCopy(for ref: ItemRef, format: String, mime: String) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: ref.server) else { return nil }
		return StreamTarget(
			url: client.url("stream", parameters: ["id": ref.id, "format": format]),
			quality: .original,
			cacheKey: CacheKeys.of(ref, quality: .original),
			contentType: mime)
	}

	/// The original converted to FLAC, for a receiver that cannot play the
	/// stored file. Lossless rather than a lossy encode, so a lossy source is
	/// given no second generation of loss: the rule the server applies to an
	/// original this device cannot play, and to a film's soundtrack on its own
	/// cast (`CAST_AUDIO_ONLY_FORMAT`).
	func lossless(for ref: ItemRef) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: ref.server) else { return nil }
		return StreamTarget(
			url: client.url("stream", parameters: ["id": ref.id, "format": "flac"]),
			quality: .original,
			cacheKey: CacheKeys.of(ref, quality: .original),
			contentType: "audio/flac")
	}

	/// A target at a stated quality rather than the configured one. One
	/// caller: the cast route, which must not send a container a receiver
	/// cannot decode however the local setting is spelled.
	func target(
		for ref: ItemRef, wanted: AudioQuality, playable: Set<String> = []
	) -> StreamTarget? {
		guard let client = registry.clientsSnapshot().client(for: ref.server) else {
			return nil
		}
		return StreamUrls.target(
			for: ref, client: client, quality: wanted, playable: playable)
	}
}

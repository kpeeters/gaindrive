//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One caption track on offer.
struct CaptionTrack: Identifiable, Hashable, Sendable {
	/// The server's own caption id: an ffprobe stream index, or **−1** for the
	/// sidecar file beside the video. Negative is a real id here, not a
	/// sentinel - "no subtitles" is the absence of a selection, not a value.
	let id: String
	let name: String
}

/// Listing a video's captions, and fetching one.
///
/// Two calls kept together because the second is only ever reached through the
/// first, and because both are the *only* reads in the app whose answer is not
/// an envelope.
struct CaptionTracks: Sendable {
	let registry: ServerRegistry

	/// **Listing costs one request; a track costs another, and only when one
	/// is chosen.** The same bargain the web client strikes by leaving a
	/// `<track>` in its default `disabled` mode and Android by
	/// `setSelectionFlags(0)`: an unselected caption is never fetched.
	///
	/// A downloaded film answers from its stored captions, online or not.
	@MainActor
	func list(for song: ItemRef) async -> [CaptionTrack] {
		if let stored = await FilmStore.shared.captions(for: song) { return stored }
		guard let client = registry.client(for: song.server) else { return [] }
		guard let info = try? await client.videoInfo(id: song.id) else { return [] }
		return info.captions.compactMap { dto in
			guard let id = dto.id, !id.isEmpty else { return nil }
			// A sidecar has no language and often no title, and a row labelled
			// with nothing is a row nobody can choose between.
			let name = dto.name.flatMap { $0.isEmpty ? nil : $0 } ?? "Subtitles"
			return CaptionTrack(id: id, name: name)
		}
	}

	@MainActor
	func cues(for song: ItemRef, track: CaptionTrack) async -> [Cue] {
		if let text = await FilmStore.shared.captionText(for: song, id: track.id) {
			return WebVTT.parse(text)
		}
		guard let client = registry.client(for: song.server) else { return [] }
		guard let vtt = try? await client.captions(id: song.id, captionId: track.id) else {
			return []
		}
		return WebVTT.parse(vtt)
	}

	/// Every track with its WebVTT text, for a film being downloaded. Nil
	/// when the server could not be asked, so a later refresh tries again; a
	/// track that fails on its own is left out rather than failing the rest.
	@MainActor
	func download(for song: ItemRef) async -> [(CaptionTrack, String)]? {
		guard let client = registry.client(for: song.server),
			let info = try? await client.videoInfo(id: song.id)
		else { return nil }
		var found: [(CaptionTrack, String)] = []
		for dto in info.captions {
			guard let id = dto.id, !id.isEmpty,
				let text = try? await client.captions(id: song.id, captionId: id)
			else { continue }
			let name = dto.name.flatMap { $0.isEmpty ? nil : $0 } ?? "Subtitles"
			found.append((CaptionTrack(id: id, name: name), text))
		}
		return found
	}
}

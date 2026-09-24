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
	@MainActor
	func list(for song: ItemRef) async -> [CaptionTrack] {
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
		guard let client = registry.client(for: song.server) else { return [] }
		guard let vtt = try? await client.captions(id: song.id, captionId: track.id) else {
			return []
		}
		return WebVTT.parse(vtt)
	}
}

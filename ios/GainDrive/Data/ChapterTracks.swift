//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The chapter markers inside the item being played, read from the file itself.
///
/// The playback-path half of chapters. Its browse counterpart is
/// `LibraryRepository.albumChapters`, and the split is the server's: this asks
/// `getChapters`, which opens the media and is therefore right about a list
/// somebody edited a moment ago and about a video's own container chapters,
/// while the listing asks the scan's index, which is cheap enough to want on
/// every album open. A player needs the first; a listing can only afford the
/// second.
///
/// Shaped like `CaptionTracks` and for the same reasons: a per-item lookup, on
/// the load path, of something no mirror holds. It stays out of
/// `LibraryRepository` because none of that class's machinery — the offline
/// decision, the mirror, the fan-out across servers — has anything to say about
/// it.
struct ChapterTracks: Sendable {
	let registry: ServerRegistry

	/// **Never throws and never returns nil.** A recording with no markers, a
	/// server too old for the endpoint and a request that failed are the same
	/// thing here — no chapter list to draw — and none of them is a reason to
	/// refuse to play the item.
	@MainActor
	func chapters(for song: ItemRef) async -> ChapterList {
		guard let client = registry.client(for: song.server) else { return ChapterList() }
		guard let payload = try? await client.chapters(id: song.id) else { return ChapterList() }
		return LibraryMapper.chapterList(payload)
	}
}

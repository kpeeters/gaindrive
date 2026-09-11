//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

//	The shared rows, mirroring `ui/components/Rows.kt`. Every browse screen is
//	built from these, which is what keeps browse and search from drifting.

struct SectionHeading: View {
	let text: String

	var body: some View {
		Text(text)
			.font(.subheadline.weight(.semibold))
			.foregroundStyle(.secondary)
	}
}

extension View {
	/// For the pinned header of a `.plain` list, and nowhere else. iOS 26
	/// pins those on translucent Liquid Glass, so the rows scroll visibly
	/// through the letter — hence an opaque, full-bleed background instead.
	/// `Color(.systemBackground)` is the plain list's own background in both
	/// appearances, so the header matches the rows either side of it.
	///
	/// The zeroed `listRowInsets` remove the default header margins, which
	/// would otherwise survive as translucent slivers at the pane edges; the
	/// padding puts back what the margins provided, so the text keeps lining
	/// up with the rows.
	///
	/// An extension rather than part of `SectionHeading`, because that view
	/// is also drawn *inside* rows (the recording headings in
	/// `AlbumDetailView`), where this treatment would be wrong — and two of
	/// the artist list's headers are bare `Text`.
	///
	/// Define `GD_GLASS_HEADERS` (commented out in `project.yml`, then
	/// `make generate`) to restore the stock translucent header.
	@ViewBuilder
	func pinnedHeaderBackground() -> some View {
		#if GD_GLASS_HEADERS
		self
		#else
		self
			.frame(maxWidth: .infinity, alignment: .leading)
			.padding(.horizontal, 10)
			.padding(.vertical, 8)
			.background(Color(.systemBackground))
			.listRowInsets(EdgeInsets())
		#endif
	}
}

/// **No artwork, deliberately** — as `ArtistRow` on Android, which takes no
/// cover either.
///
/// `getArtists` hands out a `coverArt` id for *every* artist whether or not any
/// image exists (the server has no `cover_art_id` for artists and simply reuses
/// the folder id), and `getCoverArt` answers an id with no local art by querying
/// MusicBrainz, Wikidata, Wikipedia, TheAudioDB and Discogs **inline on the
/// request thread, with three one-second sleeps**. An avatar per row therefore
/// costs one of those per artist, and the negative result is not even cached
/// when MusicBrainz is rate-limiting.
///
/// The portrait is worth one request on the artist header, where `AlbumsView`
/// still shows it. It is not worth N on a list.
struct ArtistRow: View {
	let item: ArtistUi

	var body: some View {
		VStack(alignment: .leading, spacing: 2) {
			Text(item.artist.name)
				.lineLimit(1)
			HStack(spacing: 6) {
				Text(albumCountText)
					.font(.footnote)
					.foregroundStyle(.secondary)
				ServerBadges(names: item.badges)
			}
		}
		.frame(maxWidth: .infinity, alignment: .leading)
		.contentShape(.rect)
	}

	private var albumCountText: String {
		item.artist.albumCount == 1 ? "1 album" : "\(item.artist.albumCount) albums"
	}
}

struct AlbumRow: View {
	let item: AlbumUi

	var body: some View {
		HStack(spacing: 12) {
			CoverThumb(source: item.cover)
			VStack(alignment: .leading, spacing: 2) {
				HStack(spacing: 6) {
					// The same glyph a video track carries in a listing: a
					// folder of films and a film's one track are the same
					// warning at two levels, and two icons would read as two
					// different things.
					//
					// **Only ever added.** `videoCount` is absent on the
					// directory-shaped listings, so no icon does not mean no
					// video — a wrong positive would be a lie, a missing one is
					// silence, and there is deliberately no audio counterpart.
					if item.album.videoCount > 0 {
						Image(systemName: "film")
							.font(.caption)
							.foregroundStyle(.secondary)
							.accessibilityLabel(
								item.album.videoCount == 1
									? "1 video" : "\(item.album.videoCount) videos")
					}
					Text(item.album.title)
						.lineLimit(1)
				}
				HStack(spacing: 6) {
					Text(subtitle)
						.font(.footnote)
						.foregroundStyle(.secondary)
						.lineLimit(1)
					ServerBadges(names: item.badges)
				}
			}
			Spacer(minLength: 0)
		}
		.contentShape(.rect)
	}

	private var subtitle: String {
		var parts: [String] = []
		if !item.album.artistName.isEmpty { parts.append(item.album.artistName) }
		if let year = item.album.year { parts.append(String(year)) }
		return parts.joined(separator: " · ")
	}
}

/// A track **inside** its album or playlist: it has a number column and needs
/// no cover, because the screen above it is already the artwork.
struct TrackRow: View {
	@Environment(PinRepository.self) private var pins

	let song: Song
	var state: TrackState = .idle
	/// Overrides the song's own track number, so a caller can substitute a
	/// positional one: an album ripped without tags reports every track as 0,
	/// which the mapper folds to nil, and a column of blanks is worse than
	/// numbers nobody wrote down. nil means "use the song's own".
	var number: Int?
	var showNumber = true
	var trailing: AnyView?
	/// The number column scales with Dynamic Type: a fixed 24 pt box clips a
	/// three-digit track number at the accessibility sizes, and this column
	/// exists precisely so the row does not reflow — so it has to grow with the
	/// text rather than crop it.
	///
	/// **Not `private`, and declared last.** A private stored property makes
	/// the whole memberwise initialiser private, which would break every
	/// `TrackRow(song:…)` in another file; declaring it after the rest keeps
	/// `song` the first parameter.
	@ScaledMetric(relativeTo: .footnote) var numberWidth: CGFloat = 24

	var body: some View {
		HStack(spacing: 12) {
			if showNumber {
				// A fixed box rather than an intrinsic width, so the row does
				// not reflow when the indicator replaces the number — which is
				// exactly what this column was reserved for.
				numberOrIndicator
					.frame(width: numberWidth, alignment: .trailing)
			}
			VStack(alignment: .leading, spacing: 2) {
				HStack(spacing: 6) {
					if song.isVideo {
						// Worth warning about: tapping this takes over the
						// screen rather than starting background audio.
						Image(systemName: "film")
							.font(.caption)
							.foregroundStyle(.secondary)
							.accessibilityLabel("Video")
					}
					Text(song.title).lineLimit(1)
				}
				if !song.artistName.isEmpty {
					Text(song.artistName)
						.font(.footnote)
						.foregroundStyle(.secondary)
						.lineLimit(1)
				}
			}
			Spacer(minLength: 0)
			if let trailing { trailing }
			StoredMark(song: song.ref)
			Text(formatDuration(song.duration))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
		}
		.contentShape(.rect)
	}

	@ViewBuilder
	private var numberOrIndicator: some View {
		switch state {
		case .idle:
			Text((number ?? song.track).map(String.init) ?? "")
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
		case .loading:
			ProgressView().controlSize(.mini)
		case .current:
			Image(systemName: "speaker.wave.2.fill")
				.font(.caption)
				.foregroundStyle(Color.accentColor)
		}
	}
}

/// A track **outside** its album — in search, recents or starred — so it needs
/// the cover, the artist and the album to be identifiable on its own.
struct SongRow: View {
	let item: SongUi
	var trailingText: String?

	var body: some View {
		HStack(spacing: 12) {
			CoverThumb(source: item.cover)
			VStack(alignment: .leading, spacing: 2) {
				Text(item.song.title).lineLimit(1)
				HStack(spacing: 6) {
					Text(subtitle)
						.font(.footnote)
						.foregroundStyle(.secondary)
						.lineLimit(1)
					ServerBadge(name: item.badge)
				}
			}
			Spacer(minLength: 0)
			StoredMark(song: item.song.ref)
			if let trailingText {
				Text(trailingText)
					.font(.caption)
					.foregroundStyle(.secondary)
			}
		}
		.contentShape(.rect)
	}

	private var subtitle: String {
		[item.song.artistName, item.song.albumTitle]
			.filter { !$0.isEmpty }
			.joined(separator: " · ")
	}
}

struct PlaylistRow: View {
	let playlist: Playlist

	var body: some View {
		VStack(alignment: .leading, spacing: 2) {
			Text(playlist.name).lineLimit(1)
			Text(subtitle)
				.font(.footnote)
				.foregroundStyle(.secondary)
		}
		.contentShape(.rect)
	}

	/// Hours and minutes rather than `formatDuration`'s mm:ss — "184:07" is not
	/// a length anyone reads.
	private var subtitle: String {
		let tracks = playlist.songCount == 1 ? "1 track" : "\(playlist.songCount) tracks"
		guard playlist.duration > 0 else { return tracks }
		let hours = playlist.duration / 3600
		let minutes = (playlist.duration % 3600) / 60
		let length = hours > 0 ? "\(hours) h \(minutes) min" : "\(minutes) min"
		return "\(tracks) · \(length)"
	}
}

/// A chapter marker standing in for its recording's row.
///
/// Shaped like `TrackRow` and deliberately not built from it. Two of that row's
/// three trailing decorations address a **song id**, which a marker has not: the
/// stored mark says whether *the file* is on the device, which is one answer for
/// every marker in a concert, and the playback indicator would light every
/// marker at once for the same reason. So the number column carries the marker's
/// index and nothing claims to be about this row that is really about the file
/// it is inside.
struct ChapterRow: View {
	let chapter: Chapter
	/// Passed in rather than derived: which marker is playing is a question
	/// about the player's *position*, and answering it per row would make every
	/// listing redraw twice a second. See `AlbumDetailView`.
	var playing = false

	@ScaledMetric(relativeTo: .footnote) var numberWidth: CGFloat = 24

	var body: some View {
		HStack(spacing: 12) {
			Text(String(chapter.index))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(playing ? Color.accentColor : Color.secondary)
				.frame(width: numberWidth, alignment: .trailing)
			Text(chapter.displayName)
				.lineLimit(1)
				.foregroundStyle(playing ? Color.accentColor : Color.primary)
			Spacer(minLength: 0)
			// 0 is what the server sends for a span that is not positive — two
			// markers on one timestamp, or one past the end of the file — and
			// `--:--` beside a row that plays perfectly well would read as a
			// fault rather than as a hand-typed list being allowed to be odd.
			if chapter.duration > 0 {
				Text(formatDuration(chapter.duration))
					.font(.footnote.monospacedDigit())
					.foregroundStyle(.secondary)
			}
		}
		.contentShape(.rect)
	}
}

/// A chapter match in a search listing.
///
/// Shaped like `SongRow` minus the artwork, which a marker has none of — its
/// recording's cover is the album's, and drawing it on every row would say the
/// hits were albums.
///
/// The trailing column carries *where in the recording* the marker is, where a
/// song row carries how long it is. That is the one fact a search hit has and a
/// listing row does not, and it is what tells two takes of one song apart.
struct ChapterHitRow: View {
	let hit: ChapterHit

	var body: some View {
		HStack(spacing: 12) {
			VStack(alignment: .leading, spacing: 2) {
				Text(hit.displayName).lineLimit(1)
				Text(subtitle)
					.font(.footnote)
					.foregroundStyle(.secondary)
					.lineLimit(1)
			}
			Spacer(minLength: 0)
			Text(formatChapterTime(hit.start))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
		}
		.contentShape(.rect)
	}

	/// Artist, album, then the recording itself: a marker means nothing without
	/// knowing which concert it is in. The album is what the folder is called
	/// and the track what the file is called, and they coincide often enough —
	/// a folder holding one recording named after it — that an exact duplicate
	/// is dropped rather than printed twice.
	private var subtitle: String {
		var parts = [hit.artistName, hit.albumTitle]
		if hit.trackTitle != hit.albumTitle { parts.append(hit.trackTitle) }
		return parts.filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
			.joined(separator: " · ")
	}
}

/// mm:ss, or h:mm:ss once there is an hour to show.
func formatDuration(_ seconds: Int) -> String {
	guard seconds > 0 else { return "--:--" }
	let hours = seconds / 3600
	let minutes = (seconds % 3600) / 60
	let secs = seconds % 60
	return hours > 0
		? String(format: "%d:%02d:%02d", hours, minutes, secs)
		: String(format: "%d:%02d", minutes, secs)
}

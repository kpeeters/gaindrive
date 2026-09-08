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

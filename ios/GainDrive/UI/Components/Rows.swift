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

struct ArtistRow: View {
	let item: ArtistUi

	var body: some View {
		HStack(spacing: 12) {
			ArtistAvatar(source: item.cover, size: 44)
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
			Spacer(minLength: 0)
		}
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
				Text(item.album.title)
					.lineLimit(1)
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
	let song: Song
	var showNumber = true
	var trailing: AnyView?

	var body: some View {
		HStack(spacing: 12) {
			if showNumber {
				// A fixed box rather than an intrinsic width, so nothing in the
				// row shifts when a mark appears beside it — which is what
				// phase 3's playing indicator and phase 5's download tick will
				// both do.
				Text(song.track.map(String.init) ?? "")
					.font(.footnote.monospacedDigit())
					.foregroundStyle(.secondary)
					.frame(width: 24, alignment: .trailing)
			}
			VStack(alignment: .leading, spacing: 2) {
				HStack(spacing: 6) {
					if song.isVideo {
						// Worth warning about: tapping this takes over the
						// screen rather than starting background audio.
						Image(systemName: "film")
							.font(.caption)
							.foregroundStyle(.secondary)
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
			Text(formatDuration(song.duration))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
		}
		.contentShape(.rect)
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

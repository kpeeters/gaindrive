package org.gaindrive.android.data

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.net.AlbumDto
import org.gaindrive.android.net.AlbumInfoDto
import org.gaindrive.android.net.ArtistDto
import org.gaindrive.android.net.ArtistInfoDto
import org.gaindrive.android.net.ArtistWithAlbums
import org.gaindrive.android.net.IndexDto
import org.gaindrive.android.net.PlaylistDto
import org.gaindrive.android.net.SearchResultDto
import org.gaindrive.android.net.SongDto

/**
 * DTO to domain. The single place where an absent optional field is turned
 * into something the UI can render, so those decisions are made once instead
 * of at every use site.
 *
 * Every mapper takes the [ServerId] explicitly: it is not in the payload, and
 * pairing it with the server's ids here is what produces a usable [ItemRef].
 */

private fun ServerId.ref(id: String?): ItemRef? =
	id?.takeIf { it.isNotBlank() }?.let { ItemRef(this, it) }

fun ArtistDto.toDomain(server: ServerId) = Artist(
	ref = ItemRef(server, id),
	name = name,
	albumCount = albumCount,
	// The server uses the artist's own folder id as its cover art id.
	coverArt = server.ref(coverArt ?: id),
	starredAt = starred,
)

fun IndexDto.toDomain(server: ServerId) = ArtistIndex(
	label = name,
	artists = artist.map { it.toDomain(server) },
)

fun ArtistWithAlbums.toDomain(server: ServerId) = Artist(
	ref = ItemRef(server, id),
	name = name,
	albumCount = if (albumCount > 0) albumCount else album.size,
	coverArt = server.ref(coverArt ?: id),
	starredAt = starred,
)

fun AlbumDto.toDomain(server: ServerId) = Album(
	ref = ItemRef(server, id),
	// getAlbum sends `name`; the directory-shaped endpoints send `title`.
	// Either can be the one that is present.
	title = name.ifBlank { title.orEmpty() },
	artistName = artist.orEmpty(),
	// artistId is the ID3 field; parent is the folder-browsing equivalent and
	// holds the same value on this server. Prefer the former, accept the latter.
	artistRef = server.ref(artistId ?: parent),
	songCount = if (songCount > 0) songCount else song.size,
	duration = duration,
	year = year?.takeIf { it > 0 },
	genre = genre?.takeIf { it.isNotBlank() },
	coverArt = server.ref(coverArt),
	starredAt = starred,
)

fun SongDto.toDomain(server: ServerId) = Song(
	ref = ItemRef(server, id),
	title = title,
	artistName = artist.orEmpty(),
	albumTitle = album.orEmpty(),
	albumRef = server.ref(albumId ?: parent),
	track = track?.takeIf { it > 0 },
	discNumber = discNumber?.takeIf { it > 0 },
	year = year?.takeIf { it > 0 },
	duration = duration,
	bitRate = bitRate,
	suffix = suffix,
	contentType = contentType,
	sizeBytes = size,
	coverArt = server.ref(coverArt),
	starredAt = starred,
	lastPlayedAt = lastPlayed,
)

fun PlaylistDto.toDomain(server: ServerId) = Playlist(
	ref = ItemRef(server, id),
	name = name,
	comment = comment?.takeIf { it.isNotBlank() },
	owner = owner,
	isPublic = public,
	songCount = if (songCount > 0) songCount else entry.size,
	duration = duration,
	songs = entry.map { it.toDomain(server) },
)

fun ArtistInfoDto.toDomain() = ArtistInfo(
	biography = biography?.takeIf { it.isNotBlank() },
	wikiUrl = wikiUrl?.takeIf { it.isNotBlank() },
	allMusicUrl = allMusicUrl?.takeIf { it.isNotBlank() },
	lastFmUrl = lastFmUrl?.takeIf { it.isNotBlank() },
	discogsUrl = discogsUrl?.takeIf { it.isNotBlank() },
)

fun AlbumInfoDto.toDomain() = AlbumNotes(
	notes = notes?.takeIf { it.isNotBlank() },
	wikiUrl = wikiUrl?.takeIf { it.isNotBlank() },
	allMusicUrl = allMusicUrl?.takeIf { it.isNotBlank() },
)

fun SearchResultDto.toDomain(server: ServerId) = LibrarySelection(
	artists = artist.map { it.toDomain(server) },
	albums = album.map { it.toDomain(server) },
	songs = song.map { it.toDomain(server) },
)

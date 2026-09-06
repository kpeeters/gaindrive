package org.gaindrive.android.data

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.AlbumNotes
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistInfo
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.Chapter
import org.gaindrive.android.data.model.ChapterHit
import org.gaindrive.android.data.model.ChapterList
import org.gaindrive.android.data.model.ChapterSource
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.MusicRoot
import org.gaindrive.android.data.model.Playlist
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.data.model.SongChapters
import org.gaindrive.android.net.AlbumChapterSongDto
import org.gaindrive.android.net.AlbumDto
import org.gaindrive.android.net.AlbumInfoDto
import org.gaindrive.android.net.ArtistDto
import org.gaindrive.android.net.ArtistInfoDto
import org.gaindrive.android.net.ArtistWithAlbums
import org.gaindrive.android.net.ChapterDto
import org.gaindrive.android.net.ChapterHitDto
import org.gaindrive.android.net.ChaptersDto
import org.gaindrive.android.net.DirectoryDto
import org.gaindrive.android.net.IndexDto
import org.gaindrive.android.net.MusicFolderDto
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

/**
 * [albumRef] overrides where the track says it belongs, and folder browsing has
 * to pass it. Two reasons the fields below cannot be trusted there: a directory
 * child carries *both* `albumId` and `parent` on a server that keeps the ID3 and
 * folder hierarchies apart, and the ID3 one wins here — pointing the track at an
 * album no folder-mode listing will ever produce. And a track inside a disc
 * subfolder has `parent` set to that subfolder rather than to the album.
 *
 * It matters beyond the screen: `LibraryDao.songsOfAlbum` keys on this, and it
 * backs both offline album detail and what an album pin protects.
 */
fun SongDto.toDomain(server: ServerId, albumRef: ItemRef? = null) = Song(
	ref = ItemRef(server, id),
	title = title,
	artistName = artist.orEmpty(),
	albumArtistName = displayAlbumArtist.orEmpty(),
	albumTitle = album.orEmpty(),
	albumRef = albumRef ?: server.ref(albumId ?: parent),
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
	isVideo = isVideo,
	nativeSeek = nativeSeek,
	season = season?.takeIf { it > 0 },
	transcodedContentType = transcodedContentType,
	width = originalWidth?.takeIf { it > 0 },
	height = originalHeight?.takeIf { it > 0 },
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
	chapters = chapter.map { it.toDomain(server) },
)

// ── Chapters ────────────────────────────────────────────────────────────────

fun ChapterDto.toDomain() = Chapter(
	index = index,
	startSeconds = start,
	duration = duration,
	name = name,
)

fun AlbumChapterSongDto.toDomain(server: ServerId) = SongChapters(
	ref = ItemRef(server, id),
	title = title,
	chapters = chapter.map { it.toDomain() },
)

fun ChaptersDto.toDomain() = ChapterList(
	chapters = chapter.map { it.toDomain() },
	source = ChapterSource.from(source),
)

fun ChapterHitDto.toDomain(server: ServerId) = ChapterHit(
	songRef = ItemRef(server, songId),
	// Nullable rather than assumed: without it there is no listing to open, and
	// the row then only plays. Every gaindrive server sends it.
	albumRef = server.ref(parent),
	index = index,
	startSeconds = start,
	name = name,
	trackTitle = track,
	albumTitle = album,
	artistName = artist,
)

fun MusicFolderDto.toDomain() = MusicRoot(id = id, name = name, contentType = contentType)

// ── Folder browsing ─────────────────────────────────────────────────────────
//
// `getMusicDirectory` describes the same artists, albums and tracks as the ID3
// endpoints, in a shape that names none of them: everything is a directory or a
// child. Which of the three a given listing is depends only on where the user
// was, so the mappers below are named for what the caller knows it asked for.

/**
 * A child directory of an artist's listing, read as one of their albums.
 *
 * The folder listing does not count tracks or sum durations, so both are zero
 * and the row simply omits them — `AlbumRow` already draws a subtitle from
 * whichever parts it has. The cover falls back to the folder's own id, the same
 * convention `ArtistDto.toDomain` relies on.
 */
fun SongDto.toAlbum(server: ServerId) = Album(
	ref = ItemRef(server, id),
	title = title.ifBlank { album.orEmpty() },
	artistName = artist.orEmpty(),
	artistRef = server.ref(parent),
	songCount = 0,
	duration = 0,
	year = year?.takeIf { it > 0 },
	genre = null,
	coverArt = server.ref(coverArt ?: id),
	starredAt = null,
)

/** A directory reached as an artist. Its albums are its subdirectories. */
fun DirectoryDto.toArtist(server: ServerId) = Artist(
	ref = ItemRef(server, id),
	name = name,
	albumCount = child.count { it.isDir },
	coverArt = server.ref(coverArt ?: id),
	starredAt = null,
)

/**
 * A directory reached as an album, with the tracks that were found under it —
 * which may have come from disc subfolders rather than from this listing, so
 * they are passed in rather than read from [child].
 */
fun DirectoryDto.toAlbum(server: ServerId, songs: List<Song>) = Album(
	ref = ItemRef(server, id),
	title = name,
	// The folder says nothing about who made the record; its tracks do — but
	// the album's artist, not the first one's own. On a compilation those
	// differ, and taking `artistName` here headed the whole album with whoever
	// track 1 happened to be. Falling back to it covers a server too old to
	// send the album artist at all.
	artistName = songs.firstOrNull()
		?.let { it.albumArtistName.ifBlank { it.artistName } }
		.orEmpty(),
	artistRef = server.ref(parent),
	songCount = songs.size,
	duration = songs.sumOf { it.duration },
	year = songs.firstNotNullOfOrNull { it.year },
	genre = null,
	coverArt = server.ref(coverArt ?: id),
	starredAt = null,
)

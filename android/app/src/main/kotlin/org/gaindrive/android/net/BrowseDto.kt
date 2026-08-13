package org.gaindrive.android.net

import kotlinx.serialization.Serializable

/**
 * DTOs for the browsing endpoints, shaped to the JSON the server actually
 * emits rather than to the spec's prose. Since `SPEC-AUDIT.md` these agree:
 * ids are strings and `starred` is an ISO 8601 timestamp.
 *
 * Nearly every field is optional, so everything has a default. Deciding what
 * an absent field means is the mapper's job, not the DTO's.
 */

@Serializable
data class ArtistDto(
	val id: String = "",
	val name: String = "",
	val albumCount: Int = 0,
	val coverArt: String? = null,
	/** ISO 8601 instant, or null when not starred. */
	val starred: String? = null,
)

@Serializable
data class IndexDto(
	/** The bucket letter, or "#" for names that do not start with one. */
	val name: String = "",
	val artist: List<ArtistDto> = emptyList(),
)

@Serializable
data class AlbumDto(
	val id: String = "",
	/** Folder-browsing parent; the same value as [artistId] on this server. */
	val parent: String? = null,
	val artistId: String? = null,
	val name: String = "",
	val title: String? = null,
	val artist: String? = null,
	val songCount: Int = 0,
	val duration: Int = 0,
	val created: String? = null,
	val coverArt: String? = null,
	val year: Int? = null,
	val genre: String? = null,
	val starred: String? = null,
	val song: List<SongDto> = emptyList(),
)

@Serializable
data class SongDto(
	val id: String = "",
	val parent: String? = null,
	val albumId: String? = null,
	val title: String = "",
	val artist: String? = null,
	val album: String? = null,
	val track: Int? = null,
	val discNumber: Int? = null,
	val year: Int? = null,
	val genre: String? = null,
	val size: Long = 0,
	val contentType: String? = null,
	val suffix: String? = null,
	val duration: Int = 0,
	val bitRate: Int = 0,
	val coverArt: String? = null,
	val starred: String? = null,
	val transcodedContentType: String? = null,
	val transcodedSuffix: String? = null,
	val transcodedBitRate: Int? = null,
	/** Only present in `getRecentSongs`; a gaindrive extension. */
	val lastPlayed: String? = null,

	// ── Video ───────────────────────────────────────────────────────────
	//
	// The server derives isVideo from the file extension, so it is right on
	// every endpoint. nativeSeek is not: only getVideos, getMusicDirectory,
	// getAlbum and getSong select the codec columns it is computed from, and
	// everywhere else it comes back false. That is the safe direction — such a
	// video plays and seeks, just over HLS when it need not have — so the flag
	// is trusted as given rather than second-guessed.

	val isVideo: Boolean = false,
	/**
	 * gaindrive extension: whether the stream this entry would produce carries
	 * a Content-Length and answers Range requests. False means the server can
	 * only re-encode it on the fly, which is chunked and unseekable.
	 */
	val nativeSeek: Boolean = false,
	/**
	 * gaindrive extension: the season an episode belongs to, absent for
	 * anything that is not one. `discNumber` already carries the same number —
	 * it is what orders and groups the tracks — so this only decides whether a
	 * group is headed "Series 2" or "Disc 2". Populated by the same endpoints
	 * as [nativeSeek], and absent from the rest, where a season reads as a
	 * disc; that is cosmetic and the ordering is unaffected.
	 */
	val season: Int? = null,
	/** Both omitted when the scan could not probe the file. */
	val originalWidth: Int? = null,
	val originalHeight: Int? = null,
)

@Serializable
data class PlaylistDto(
	val id: String = "",
	val name: String = "",
	val comment: String? = null,
	val owner: String? = null,
	val public: Boolean = false,
	val songCount: Int = 0,
	val duration: Int = 0,
	val created: String? = null,
	val changed: String? = null,
	val coverArt: String? = null,
	val entry: List<SongDto> = emptyList(),
)

// ── Response bodies ─────────────────────────────────────────────────────────
//
// Each implements SubsonicBody so the existing requireOk() in
// SubsonicException.kt unwraps them without special cases.

@Serializable
data class ArtistsContainer(
	val index: List<IndexDto> = emptyList(),
)

@Serializable
data class GetArtistsBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val artists: ArtistsContainer? = null,
) : SubsonicBody

@Serializable
data class MusicFolderDto(
	val id: String = "",
	val name: String = "",
	/**
	 * gaindrive extension: "artists" or "categories".
	 *
	 * Null, not defaulted, and the distinction carries weight: absent means the
	 * server has no concept of root kinds, which also means it will ignore a
	 * `contentType` query parameter and answer with its entire library. That
	 * has to be detectable, or such a server's folders appear under every mode.
	 */
	val contentType: String? = null,
)

@Serializable
data class MusicFoldersContainer(
	val musicFolder: List<MusicFolderDto> = emptyList(),
)

@Serializable
data class GetMusicFoldersBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val musicFolders: MusicFoldersContainer? = null,
) : SubsonicBody

@Serializable
data class ArtistWithAlbums(
	val id: String = "",
	val name: String = "",
	val albumCount: Int = 0,
	val coverArt: String? = null,
	val starred: String? = null,
	val album: List<AlbumDto> = emptyList(),
)

@Serializable
data class GetArtistBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val artist: ArtistWithAlbums? = null,
) : SubsonicBody

@Serializable
data class GetAlbumBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val album: AlbumDto? = null,
) : SubsonicBody

@Serializable
data class AlbumInfoDto(
	val notes: String? = null,
	val wikiUrl: String? = null,
	val allMusicUrl: String? = null,
	val musicBrainzId: String? = null,
)

@Serializable
data class GetAlbumInfoBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val albumInfo2: AlbumInfoDto? = null,
) : SubsonicBody

/**
 * Portrait URLs here point at MusicBrainz/Wikipedia, not at the server. The
 * app ignores them and asks `getCoverArt` for the artist folder instead, which
 * keeps the fetch on one authenticated path the server can cache — and working
 * when the phone reaches the server over a VPN the image host is not on.
 */
@Serializable
data class ArtistInfoDto(
	val biography: String? = null,
	val musicBrainzId: String? = null,
	val lastFmUrl: String? = null,
	val wikiUrl: String? = null,
	val allMusicUrl: String? = null,
	val discogsUrl: String? = null,
)

@Serializable
data class GetArtistInfoBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val artistInfo2: ArtistInfoDto? = null,
) : SubsonicBody

/**
 * One selectable subtitle stream. [id] is the ffprobe stream index, which is
 * what `getCaptions` expects back as `captionId`; [name] is the stream's title,
 * falling back to its language.
 *
 * Expect an empty list for a DVD: its subtitles are bitmaps, and the server
 * filters those out rather than offer tracks that could never become WebVTT.
 */
@Serializable
data class CaptionDto(
	val id: String = "",
	val name: String = "",
)

@Serializable
data class VideoInfoDto(
	val id: String = "",
	val captions: List<CaptionDto> = emptyList(),
)

@Serializable
data class GetVideoInfoBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val videoInfo: VideoInfoDto? = null,
) : SubsonicBody

@Serializable
data class SearchResultDto(
	val artist: List<ArtistDto> = emptyList(),
	val album: List<AlbumDto> = emptyList(),
	val song: List<SongDto> = emptyList(),
)

@Serializable
data class Search3Body(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val searchResult3: SearchResultDto? = null,
) : SubsonicBody

@Serializable
data class Starred2Body(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val starred2: SearchResultDto? = null,
) : SubsonicBody

@Serializable
data class PlaylistsContainer(
	val playlist: List<PlaylistDto> = emptyList(),
)

@Serializable
data class GetPlaylistsBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val playlists: PlaylistsContainer? = null,
) : SubsonicBody

@Serializable
data class GetPlaylistBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val playlist: PlaylistDto? = null,
) : SubsonicBody

@Serializable
data class RecentSongsContainer(
	val song: List<SongDto> = emptyList(),
)

@Serializable
data class GetRecentSongsBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val recentSongs: RecentSongsContainer? = null,
) : SubsonicBody

/** Endpoints that return nothing beyond a status: star, unstar, scrobble. */
@Serializable
data class EmptyBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
) : SubsonicBody

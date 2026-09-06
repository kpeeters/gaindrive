package org.gaindrive.android.data.local

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Artist
import org.gaindrive.android.data.model.ArtistIndex
import org.gaindrive.android.data.model.LibrarySelection
import org.gaindrive.android.data.model.Playlist

/**
 * What is worth showing when nothing can be fetched.
 *
 * Offline, a library listing every artist on the server is mostly rows that do
 * nothing — the shelf is full and almost none of it can be taken down. So an
 * artist, album or playlist appears only when there is stored audio behind it.
 *
 * Individual *tracks* are not filtered by this. Inside an album that survived,
 * knowing which three of its twelve tracks are here is useful, and dropping the
 * rest would misreport the album's length and renumber it. Those rows are dimmed
 * instead — see `AvailabilityState`.
 *
 * Applied only when genuinely offline. A server that merely failed while online
 * still shows its whole stored library: the connection may come back, and
 * hiding rows because one request timed out would be a strange thing to do.
 */
data class StoredFilter(
	/** Encoded refs of songs held in full — the audio cache's own keys. */
	val songs: Set<String>,
	val albums: Set<String>,
	val artists: Set<String>,
	val playlists: Set<String>,
	/** How many albums an artist has *here*, which is not what the server said. */
	val albumCounts: Map<String, Int>,
) {

	// Named apart rather than overloaded: generic erasure gives every
	// `List<…> -> List<…>` the same JVM signature.
	fun filterIndexes(indexes: List<ArtistIndex>): List<ArtistIndex> = indexes
		.map { index -> ArtistIndex(index.label, index.artists.mapNotNull(::keep)) }
		// A bucket letter with nothing under it is a header over empty space.
		.filter { it.artists.isNotEmpty() }

	private fun keep(artist: Artist): Artist? {
		val key = artist.ref.encode()
		if (key !in artists) return null
		// Corrected, not left alone: "12 albums" over two rows is the confusion
		// this whole filter exists to remove.
		return artist.copy(albumCount = albumCounts[key] ?: artist.albumCount)
	}

	fun filterAlbums(albums: List<Album>): List<Album> =
		albums.filter { it.ref.encode() in this.albums }

	fun filterPlaylists(playlists: List<Playlist>): List<Playlist> =
		playlists.filter { it.ref.encode() in this.playlists }

	// Rebuilt field by field rather than copied, so a new one is dropped unless
	// it is named here. `chapters` is the case that exists today and the omission
	// is deliberate: nothing mirrors a marker — it has no id to key a row on — so
	// there are never any to filter, and the section simply does not appear
	// offline. Anything that changes that has to add a line here.
	fun filterSelection(selection: LibrarySelection): LibrarySelection = LibrarySelection(
		artists = selection.artists.mapNotNull(::keep),
		albums = filterAlbums(selection.albums),
		// Songs are filtered here, unlike inside an album: a search result that
		// cannot play is not telling you anything you asked to know.
		songs = selection.songs.filter { it.ref.encode() in songs },
	)

	companion object {
		val EMPTY = StoredFilter(
			songs = emptySet(),
			albums = emptySet(),
			artists = emptySet(),
			playlists = emptySet(),
			albumCounts = emptyMap(),
		)
	}
}

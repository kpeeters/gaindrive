package org.gaindrive.android.ui

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.Song

/**
 * Domain items paired with their cover URL, already resolved.
 *
 * Rows stay dumb this way: resolving a cover means reading DataStore and
 * decrypting a password, which must happen once per screen load rather than
 * once per row. Shared across browse and search so the two cannot drift.
 */
data class AlbumUi(
	val album: Album,
	val coverUrl: String?,
	/**
	 * The owning servers' names in merged scope, empty when they would be
	 * noise. Plural because a row whose duplicates were collapsed stands for
	 * every server that has the album, and hiding that would make the missing
	 * second row look like a bug.
	 */
	val badges: List<String> = emptyList(),
)

data class SongUi(
	val song: Song,
	val coverUrl: String?,
	val badge: String? = null,
)

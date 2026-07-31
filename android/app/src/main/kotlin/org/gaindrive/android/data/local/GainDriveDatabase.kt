package org.gaindrive.android.data.local

import androidx.room.Database
import androidx.room.RoomDatabase

/**
 * The library mirror and the pin list.
 *
 * Separate from the credential store on purpose: `ServerStore` keeps servers
 * and passwords in DataStore, so wiping or migrating this database can never
 * log anyone out.
 */
@Database(
	entities = [
		ArtistEntity::class,
		AlbumEntity::class,
		SongEntity::class,
		PlaylistEntity::class,
		PlaylistSongEntity::class,
		PinEntity::class,
	],
	version = 1,
	// Nothing consumes exported schemas yet, and destructive migration is the
	// right answer for a mirror: it refills itself from the servers.
	exportSchema = false,
)
abstract class GainDriveDatabase : RoomDatabase() {
	abstract fun libraryDao(): LibraryDao
	abstract fun pinDao(): PinDao
}

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
		ArtistInfoEntity::class,
		AlbumNotesEntity::class,
		ChapterEntity::class,
	],
	// 2: artists gained contentType. 3: songs gained isVideo. 4: songs gained
	// season. 5: songs gained albumArtistName. 6: albums gained videoCount.
	// 7: biographies, album notes and chapters are mirrored. The mirror is a
	// cache and the database is built with fallbackToDestructiveMigration, so a
	// bump is the whole cost.
	version = 7,
	// Nothing consumes exported schemas yet, and destructive migration is the
	// right answer for a mirror: it refills itself from the servers.
	exportSchema = false,
)
abstract class GainDriveDatabase : RoomDatabase() {
	abstract fun libraryDao(): LibraryDao
	abstract fun pinDao(): PinDao
}

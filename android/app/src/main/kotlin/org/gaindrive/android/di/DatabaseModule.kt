package org.gaindrive.android.di

import android.content.Context
import androidx.room.Room
import dagger.Module
import dagger.Provides
import dagger.hilt.InstallIn
import dagger.hilt.android.qualifiers.ApplicationContext
import dagger.hilt.components.SingletonComponent
import org.gaindrive.android.data.local.GainDriveDatabase
import org.gaindrive.android.data.local.LibraryDao
import org.gaindrive.android.data.local.PinDao
import javax.inject.Singleton

@Module
@InstallIn(SingletonComponent::class)
object DatabaseModule {

	@Provides
	@Singleton
	fun database(@ApplicationContext context: Context): GainDriveDatabase =
		Room.databaseBuilder(context, GainDriveDatabase::class.java, "library.db")
			// A mirror is rebuilt by browsing, so throwing it away on a schema
			// change costs nothing but the next few requests - far less than
			// hand-writing migrations for a cache would. dropAllTables because
			// nothing in this database is worth more than the tables Room
			// knows about; keeping unknown ones would only preserve debris.
			.fallbackToDestructiveMigration(dropAllTables = true)
			.build()

	@Provides
	fun libraryDao(db: GainDriveDatabase): LibraryDao = db.libraryDao()

	@Provides
	fun pinDao(db: GainDriveDatabase): PinDao = db.pinDao()
}

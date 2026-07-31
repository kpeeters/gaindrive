package org.gaindrive.android.data.local

import androidx.room.Dao
import androidx.room.Query
import androidx.room.Upsert
import kotlinx.coroutines.flow.Flow

@Dao
interface PinDao {

	@Query("SELECT * FROM pins")
	fun observeAll(): Flow<List<PinEntity>>

	@Query("SELECT * FROM pins")
	suspend fun all(): List<PinEntity>

	@Upsert
	suspend fun add(pin: PinEntity)

	@Query("DELETE FROM pins WHERE refKey = :refKey")
	suspend fun remove(refKey: String)

	/** Pins whose server has been removed would otherwise protect nothing forever. */
	@Query("DELETE FROM pins WHERE refKey LIKE :serverPrefix")
	suspend fun removeForServer(serverPrefix: String)
}

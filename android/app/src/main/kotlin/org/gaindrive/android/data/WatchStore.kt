package org.gaindrive.android.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What changed this session about [ref]'s watched mark and resume point, over
 * what its listing was loaded with. A null field falls back to the model.
 */
data class WatchOverride(val watched: Boolean? = null, val positionMs: Long? = null)

/**
 * The watched marks and resume points this session has changed, so a listing
 * loaded before a play or a toggle still shows the truth without a reload.
 *
 * Two writers. The user, through [setWatched], which is shown at once and put
 * back if the server refuses. And playback, through [noteProgress], fed by
 * every report `PlaybackService` sends, mirroring what the server does with
 * that same report: a mark at 95%, a resume point from 30 s.
 *
 * App-scoped with its writes in the application scope, for the reason
 * `StarStore` gives: the sheet the action came from has closed before the
 * server answers, so a refusal goes out on [errors] for the shell to toast.
 */
@Singleton
class WatchStore @Inject constructor(
	private val library: LibraryRepository,
	private val scope: CoroutineScope,
) {

	private val _overrides = MutableStateFlow<Map<ItemRef, WatchOverride>>(emptyMap())
	val overrides: StateFlow<Map<ItemRef, WatchOverride>> = _overrides.asStateFlow()

	private val _errors = MutableSharedFlow<String>(extraBufferCapacity = 4)
	val errors: SharedFlow<String> = _errors.asSharedFlow()

	/** Marks or unmarks [songs]; what cannot be finished is left alone. */
	fun setWatched(songs: List<Song>, watched: Boolean) {
		val items = songs.filter { it.isResumable }
		if (items.isEmpty()) return
		val before = _overrides.value
		_overrides.update { current ->
			current + items.associate { song ->
				// Marking drops the resume point on the server too.
				song.ref to if (watched) WatchOverride(true, 0L)
				else (current[song.ref] ?: WatchOverride()).copy(watched = false)
			}
		}
		scope.launch {
			items.groupBy { it.ref.server }.forEach { (server, group) ->
				runCatchingCancellable {
					library.setWatched(server, group.map { it.ref.id }, watched)
				}.onFailure { err ->
					_overrides.update { current ->
						current - group.map { it.ref }.toSet() +
							group.mapNotNull { s -> before[s.ref]?.let { s.ref to it } }
					}
					_errors.tryEmit(err.userMessage())
				}
			}
		}
	}

	/**
	 * A playback report for [ref], at [positionMs] into an item of
	 * [durationMs]. The caller has checked the item is one the server tracks.
	 */
	fun noteProgress(ref: ItemRef, positionMs: Long, durationMs: Long) {
		when {
			durationMs > 0 && positionMs >= durationMs * 95 / 100 ->
				_overrides.update { it + (ref to WatchOverride(true, 0L)) }
			// A rewatch that stops early keeps its mark, as on the server.
			positionMs >= MIN_POSITION_MS -> _overrides.update {
				it + (ref to (it[ref] ?: WatchOverride()).copy(positionMs = positionMs))
			}
		}
	}

	private companion object {
		/** The server's own floor: below it the play has barely begun. */
		const val MIN_POSITION_MS = 30_000L
	}
}

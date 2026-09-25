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
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.data.model.StarKind
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import javax.inject.Inject
import javax.inject.Singleton

/**
 * What the user has starred this session, over what the last fetch said.
 *
 * gaindrive stores stars by *path*, since folder and song rowids do not survive
 * a rescan, and an id that does not resolve to a path is **silently ignored**.
 * So `star` answering `ok` is no evidence that anything changed, and the
 * `starredAt` a screen was loaded with cannot be trusted after a toggle. The
 * store shows the new state at once and then asks `getStarred2`, the only
 * call that can actually answer. Ported from the iOS `StarStore`.
 *
 * App-scoped, and the writes run in the application scope, because the
 * long-press sheet closes the moment its action is tapped: a write tied to the
 * sheet would be cancelled before the server heard it. For the same reason a
 * failure is published on [errors] for the app shell to toast, rather than
 * returned to a caller that is already gone.
 *
 * There is no `forget(server)`. A [ServerId] is generated per added server
 * and never reused, so a removed server's overrides can never match anything
 * again; they are a few entries that die with the process.
 */
@Singleton
class StarStore @Inject constructor(
	private val library: LibraryRepository,
	private val scope: CoroutineScope,
) {

	/** Only what was touched this session; everything else falls back to its model. */
	private val _overrides = MutableStateFlow<Map<ItemRef, Boolean>>(emptyMap())
	val overrides: StateFlow<Map<ItemRef, Boolean>> = _overrides.asStateFlow()

	/**
	 * Toggles in flight. A second tap is refused rather than queued: two writes
	 * racing leave whichever answers last in charge, which is the earlier tap
	 * about half the time.
	 */
	private val _busy = MutableStateFlow<Set<ItemRef>>(emptySet())
	val busy: StateFlow<Set<ItemRef>> = _busy.asStateFlow()

	private val _errors = MutableSharedFlow<String>(extraBufferCapacity = 4)
	val errors: SharedFlow<String> = _errors.asSharedFlow()

	fun isStarred(ref: ItemRef, fallback: Boolean): Boolean =
		_overrides.value[ref] ?: fallback

	/** [currently] is what the caller's model says, used when nothing overrides it. */
	fun toggle(ref: ItemRef, kind: StarKind, currently: Boolean) {
		if (ref in _busy.value) return
		val wanted = !isStarred(ref, currently)
		_overrides.update { it + (ref to wanted) }
		_busy.update { it + ref }
		scope.launch {
			try {
				runCatchingCancellable {
					library.setStarred(ref, kind, wanted)
				}.onSuccess {
					reconcile(ref.server)
				}.onFailure {
					// A refusal we *can* see, unlike the silent kind: put it back.
					_overrides.update { it + (ref to !wanted) }
					_errors.tryEmit(it.userMessage())
				}
			} finally {
				_busy.update { it - ref }
			}
		}
	}

	/**
	 * Folds a `getStarred2` answer into the overrides for [server]. Failure to
	 * fetch leaves the optimistic state standing: the write itself succeeded,
	 * and a star that flipped back for want of a confirmation would be the
	 * less likely truth.
	 */
	private suspend fun reconcile(server: ServerId) {
		val starred = runCatchingCancellable { library.starred(server) }.getOrNull() ?: return
		val truth = buildSet {
			starred.artists.mapTo(this) { it.ref }
			starred.albums.mapTo(this) { it.ref }
			starred.songs.mapTo(this) { it.ref }
		}
		_overrides.update { reconciled(it, server, truth) }
	}

	companion object {
		/**
		 * Every override on [server] is set to what [truth] says; other
		 * servers' are untouched.
		 *
		 * Deliberately unlike iOS, which drops an override the server agrees
		 * with: the screen that drew it still holds the `starredAt` it was
		 * loaded with, so dropping it would flip the star back until that
		 * screen reloads.
		 */
		fun reconciled(
			overrides: Map<ItemRef, Boolean>,
			server: ServerId,
			truth: Set<ItemRef>,
		): Map<ItemRef, Boolean> =
			overrides.mapValues { (ref, value) ->
				if (ref.server == server) ref in truth else value
			}
	}
}

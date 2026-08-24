package org.gaindrive.android.ui.fetch

import androidx.lifecycle.SavedStateHandle
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import androidx.navigation.toRoute
import dagger.hilt.android.lifecycle.HiltViewModel
import kotlinx.coroutines.Job
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.UrlFetchRepository
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Route
import javax.inject.Inject

/**
 * A server this URL could be sent to: one that answered the capability probe
 * with at least one handler.
 */
data class FetchTarget(
	val id: ServerId,
	val name: String,
	val canAudio: Boolean,
	val canVideo: Boolean,
	/** For the hint line, so the panel can say what will actually do the work. */
	val handlerNames: List<String>,
)

/**
 * Where a job has got to.
 *
 * Mapped from the server's string rather than parsed as an enum, so a state a
 * newer server invents shows as [Unknown] instead of failing the response. The
 * three terminal ones are told apart because they need different words and
 * different colours, not because anything branches on them.
 */
enum class FetchState {
	QUEUED, RUNNING, SCANNING, DONE, ERROR, CANCELLED, UNKNOWN;

	val isLive: Boolean get() = this == QUEUED || this == RUNNING || this == SCANNING

	/** Only these two may be cancelled; the server refuses the rest. */
	val isCancellable: Boolean get() = this == QUEUED || this == RUNNING

	companion object {
		fun of(raw: String): FetchState = when (raw) {
			"queued" -> QUEUED
			"running" -> RUNNING
			"scanning" -> SCANNING
			"done" -> DONE
			"error" -> ERROR
			"cancelled" -> CANCELLED
			else -> UNKNOWN
		}
	}
}

data class FetchUrlUiState(
	val url: String = "",
	/** Null while the servers are still being probed. */
	val targets: List<FetchTarget>? = null,
	val server: ServerId? = null,
	val artist: String = "",
	val album: String = "",
	val audio: Boolean = true,
	val submitting: Boolean = false,
	val job: FetchJobDto? = null,
	val error: String? = null,
) {
	val target: FetchTarget? get() = targets?.firstOrNull { it.id == server }

	val state: FetchState get() = job?.let { FetchState.of(it.state) } ?: FetchState.UNKNOWN

	/** A job still running; the form stays visible but disabled behind it. */
	val live: Boolean get() = job != null && state.isLive

	/** Both offered means the choice is worth drawing; one means it is not. */
	val showModes: Boolean get() = target?.let { it.canAudio && it.canVideo } == true

	val canSubmit: Boolean
		get() = url.isNotBlank() && server != null && !submitting && !live
}

@HiltViewModel
class FetchUrlViewModel @Inject constructor(
	private val fetches: UrlFetchRepository,
	private val registry: ServerRegistry,
	private val settings: SettingsStore,
	private val connectivity: Connectivity,
	savedStateHandle: SavedStateHandle,
) : ViewModel() {

	// Through the type-safe route rather than a string key, so renaming the
	// field is a compile error rather than a silent empty panel.
	private val sharedUrl: String = savedStateHandle.toRoute<Route.FetchUrl>().url

	private val _state = MutableStateFlow(FetchUrlUiState(url = sharedUrl))
	val state: StateFlow<FetchUrlUiState> = _state.asStateFlow()

	private var poller: Job? = null

	init {
		viewModelScope.launch {
			// The names and the mode before the probe: they are local, and
			// showing empty fields that fill in a second later reads as the app
			// losing what was typed last time.
			_state.update {
				it.copy(
					artist = settings.fetchArtist.first(),
					album = settings.fetchAlbum.first(),
					audio = settings.fetchAudio.first(),
				)
			}
			// Driven by connectivity rather than run once, and the flow is
			// Eagerly started so this also *is* the first probe. A panel opened
			// with no signal would otherwise stay on "no server can fetch this"
			// after the signal came back — a statement about the network dressed
			// up as one about the servers. Settled servers are never re-probed.
			connectivity.online.collect { online ->
				if (online && _state.value.targets.isNullOrEmpty()) loadTargets()
			}
		}
	}

	/**
	 * Probes every enabled server and keeps the ones that can fetch.
	 *
	 * Sequential rather than a fan-out: almost everyone has one server, the
	 * probe is cached for the process after the first answer, and a bounded
	 * timeout inside the repository already stops an unreachable one holding up
	 * the rest for long.
	 */
	private suspend fun loadTargets() {
		val configs = registry.enabledServers.first()
		val targets = configs.mapNotNull { config ->
			val handlers = fetches.handlersFor(config)
			if (handlers.isEmpty()) return@mapNotNull null
			FetchTarget(
				id = config.id,
				name = config.name,
				canAudio = handlers.any { it.audio },
				canVideo = handlers.any { it.video },
				handlerNames = handlers.map { it.name },
			)
		}

		// The remembered server may have been removed, disabled, or had its
		// upload rights taken away since it was written.
		val remembered = settings.fetchServer.first()?.let { ServerId(it) }
		val chosen = targets.firstOrNull { it.id == remembered }?.id ?: targets.firstOrNull()?.id

		_state.update { it.copy(targets = targets, server = chosen) }
		chosen?.let { alignModeWith(it) }
	}

	fun onArtist(v: String) = _state.update { it.copy(artist = v, error = null) }

	fun onAlbum(v: String) = _state.update { it.copy(album = v, error = null) }

	fun onServer(id: ServerId) {
		_state.update { it.copy(server = id, error = null) }
		viewModelScope.launch {
			settings.setFetchServer(id.value)
			alignModeWith(id)
		}
	}

	fun onAudio(audio: Boolean) {
		_state.update { it.copy(audio = audio, error = null) }
		viewModelScope.launch { settings.setFetchAudio(audio) }
	}

	fun clearNames() {
		_state.update { it.copy(artist = "", album = "") }
		viewModelScope.launch { settings.setFetchNames("", "") }
	}

	/**
	 * A server whose handlers only do one of the two must not be sent the
	 * other: the server would refuse with "that handler cannot fetch video",
	 * which is a true statement about a choice the user was never offered.
	 */
	private fun alignModeWith(id: ServerId) {
		val target = _state.value.targets?.firstOrNull { it.id == id } ?: return
		val audio = when {
			_state.value.audio && target.canAudio -> true
			!_state.value.audio && target.canVideo -> false
			else -> target.canAudio
		}
		_state.update { it.copy(audio = audio) }
	}

	fun submit() {
		val s = _state.value
		val server = s.server ?: return
		if (!s.canSubmit) return
		_state.update { it.copy(submitting = true, error = null) }
		viewModelScope.launch {
			// Saved before the request, not after it succeeds: what the user
			// typed is worth keeping whether or not the server accepted the URL.
			settings.setFetchNames(s.artist.trim(), s.album.trim())
			runCatchingCancellable {
				fetches.submit(server, s.url, s.audio, s.artist, s.album)
			}.fold(
				onSuccess = { job ->
					_state.update { it.copy(submitting = false, job = job) }
					watch(server, job.id)
				},
				onFailure = { e ->
					_state.update {
						it.copy(submitting = false, error = "Could not start: ${e.userMessage()}")
					}
				},
			)
		}
	}

	fun cancel() {
		val s = _state.value
		val server = s.server ?: return
		val id = s.job?.id ?: return
		viewModelScope.launch {
			// The poll reports the new state; a failure here is not worth its
			// own message, since the job either stopped or it did not and the
			// panel is about to say which.
			runCatchingCancellable { fetches.cancel(server, id) }
		}
	}

	/**
	 * Follows one job to its end.
	 *
	 * Filtered to the id [submit] was given rather than rendering every job the
	 * account has, which is what the web client's shared upload bar must do.
	 * Nothing here has to notice a fetch started elsewhere.
	 *
	 * Neither kind of bad tick gives up at once, and they are counted apart
	 * because they mean different things. A poll that *fails* is usually the
	 * server being busy indexing what it has just downloaded; a poll that
	 * succeeds but does not mention the job is the job having vanished, which
	 * the server does not do for some minutes after one ends. So a failure gets
	 * a long rope and an absence a short one, and the message at the end of each
	 * says which happened.
	 *
	 * Living in [viewModelScope] is what stops it: leaving the panel ends the
	 * poll and does *not* cancel the fetch, which goes on running on the server.
	 */
	private fun watch(server: ServerId, jobId: String) {
		poller?.cancel()
		poller = viewModelScope.launch {
			var absences = 0
			var failures = 0
			while (true) {
				delay(POLL_INTERVAL_MS)

				val jobs = runCatchingCancellable { fetches.jobs(server) }.getOrNull()
				if (jobs == null) {
					if (++failures >= MAX_POLL_FAILURES) {
						_state.update {
							it.copy(
								error = "Lost contact with the server. The fetch may " +
									"still be running; the web client can say.",
							)
						}
						return@launch
					}
					continue
				}
				failures = 0

				val job = jobs.firstOrNull { it.id == jobId }
				if (job == null) {
					if (++absences >= MAX_ABSENCES) {
						_state.update {
							it.copy(error = "The server stopped reporting on that fetch.")
						}
						return@launch
					}
					continue
				}
				absences = 0

				_state.update { it.copy(job = job) }
				if (!FetchState.of(job.state).isLive) return@launch
			}
		}
	}

	private companion object {
		/** What the web client uses, and fast enough for a progress bar. */
		const val POLL_INTERVAL_MS = 2_000L

		/** Six seconds of a job the server has never heard of is conclusive. */
		const val MAX_ABSENCES = 3

		/** Half a minute, which a scan of a large batch can legitimately take. */
		const val MAX_POLL_FAILURES = 15
	}
}

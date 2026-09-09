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
import kotlinx.coroutines.flow.last
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.FetchMonitor
import org.gaindrive.android.data.FetchStatus
import org.gaindrive.android.data.LibraryRepository
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.UrlFetchRepository
import org.gaindrive.android.data.model.BrowseScope
import org.gaindrive.android.data.model.FetchState
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.LibraryMode
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.gaindrive.android.net.runCatchingCancellable
import org.gaindrive.android.net.userMessage
import org.gaindrive.android.ui.Route
import javax.inject.Inject

/**
 * A name the library already knows, offered as a completion.
 *
 * [refs] is every (server, id) pair the name stands for, which is what makes
 * "do I already have this album?" one call rather than one per server.
 */
data class NameSuggestion(
	val name: String,
	val refs: List<ItemRef>,
	/** Where it was found, for the label: a slice's name, or staging. */
	val where: String,
	val staging: Boolean,
)

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

data class FetchUrlUiState(
	val url: String = "",
	/** Null while the servers are still being probed. */
	val targets: List<FetchTarget>? = null,
	val server: ServerId? = null,
	val artist: String = "",
	val album: String = "",
	val audio: Boolean = true,
	val submitting: Boolean = false,
	/**
	 * Every fetch the account has anywhere, straight from [FetchMonitor].
	 *
	 * Held whole and narrowed to the chosen server by [jobs] below, rather than
	 * narrowed on the way in: the server can change after the monitor last
	 * emitted, and a list filtered at collection time would then describe the
	 * server the user has just navigated away from.
	 */
	val status: FetchStatus = FetchStatus(),
	val error: String? = null,
	/**
	 * The slices the library offers, minus Uploads. Chooses what the two name
	 * fields are *called* and what the artist field pre-fills to — and nothing
	 * else. It deliberately does not narrow the suggestions below.
	 */
	val kinds: List<LibraryMode> = emptyList(),
	val kind: LibraryMode = LibraryMode.ARTISTS,
	/** Every top-level name the library knows, across every slice and server. */
	val suggestions: List<NameSuggestion> = emptyList(),
	/** What the library already has under these names, or null. */
	val existing: String? = null,
) {
	val target: FetchTarget? get() = targets?.firstOrNull { it.id == server }

	/**
	 * What the chosen server is reporting, newest first — not only the job this
	 * panel submitted.
	 *
	 * A list rather than the single slot this used to be, because the panel now
	 * adopts whatever is already running when it opens. That is the whole fix:
	 * the slot was filled only by [FetchUrlViewModel.submit], so returning to a
	 * panel whose fetch was still downloading showed nothing at all.
	 */
	val jobs: List<FetchJobDto> get() = status.on(server).map { it.job }

	/** The server has stopped answering; the jobs above are the last we knew. */
	val contactLost: Boolean get() = server != null && server in status.contactLost

	/**
	 * The job this URL is already, or was recently, being fetched by — a live
	 * one in preference to a finished one.
	 *
	 * **Exact equality after trimming, and no normalisation.** The blocking half
	 * of this has to agree with `gaindrive.cc:7930`, which compares the URL
	 * strings as they arrive; a client that refused something the server would
	 * have accepted is a client lying about the server. Stripping a fragment or
	 * a tracking parameter is tempting and would break that agreement, so if it
	 * is ever added it belongs to the advisory wording alone and never to
	 * [canSubmit].
	 */
	val duplicate: FetchJobDto?
		get() = url.trim().takeIf { it.isNotEmpty() }?.let { u ->
			val mine = jobs.filter { it.url == u }
			mine.firstOrNull { FetchState.of(it.state).isLive } ?: mine.firstOrNull()
		}

	/** Both offered means the choice is worth drawing; one means it is not. */
	val showModes: Boolean get() = target?.let { it.canAudio && it.canVideo } == true

	/** Same rule as the library's own chip row: one slice is no choice at all. */
	val showKinds: Boolean get() = kinds.size > 1

	val filingUnderCategory: Boolean get() = kind.id == "categories"

	val artistLabel: String get() = if (filingUnderCategory) "Category" else "Artist"

	val albumLabel: String get() = if (filingUnderCategory) "Name" else "Album"

	/**
	 * Refused only for a URL that is *already* being fetched, which is the one
	 * refusal the server would issue anyway.
	 *
	 * This used to be `!live` — no job of any kind may be running — and that was
	 * right only while `live` could mean nothing but "a job this panel started".
	 * Now that the panel adopts whatever the account has running, the same rule
	 * would let a fetch begun in the web client freeze the form on the phone,
	 * which is a worse failure than the duplicate this exists to prevent and one
	 * the person in front of it did nothing to cause.
	 */
	val canSubmit: Boolean
		get() = url.isNotBlank() && server != null && !submitting &&
			duplicate?.let { FetchState.of(it.state).isLive } != true
}

@HiltViewModel
class FetchUrlViewModel @Inject constructor(
	private val fetches: UrlFetchRepository,
	private val monitor: FetchMonitor,
	private val library: LibraryRepository,
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

	private var checker: Job? = null

	init {
		// The shell keeps the monitor warm, so its last sweep is usually seconds
		// old and a running job is on screen in the first frame. Asked again
		// anyway: opening this panel is the one moment where being a minute
		// behind would put the wrong answer in front of the person about to
		// paste a URL.
		monitor.refresh()
		viewModelScope.launch {
			monitor.status.collect { s -> _state.update { it.copy(status = s) } }
		}
		viewModelScope.launch {
			// What just landed is now in staging, and the panel stays open for
			// the next URL. Without this the suggestions are the ones loaded when
			// the panel opened, so fetching the same thing twice in one sitting
			// draws no warning at all — which is exactly how a duplicate gets
			// made. It hangs off the monitor rather than off a poll of our own,
			// so it still fires for a fetch that finished while this panel was
			// closed and is only being reopened now.
			monitor.completions.collect {
				loadSuggestions()
				recheck()
			}
		}
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
		viewModelScope.launch {
			loadSuggestions()
			// Straight away, because the fields arrive already filled from last
			// time and a name that has gone stale is exactly what this surfaces.
			recheck()
		}
	}

	/**
	 * Every top-level name the library knows, across every slice and every
	 * enabled server.
	 *
	 * **Not narrowed by [FetchUrlUiState.kind].** A name filed under a
	 * categories root is just as much a name already in the library as one under
	 * an artists root, and offering only half of them would invite a second
	 * spelling of something that is already there. The kind decides what the
	 * fields are called, not what is known.
	 *
	 * Staging is included and marked as such: something fetched a fortnight ago
	 * and never promoted is the likeliest duplicate of all, and it is the one no
	 * library listing would show.
	 */
	private suspend fun loadSuggestions() {
		val modes = runCatchingCancellable {
			library.availableModes(BrowseScope.AllServers)
		}.getOrDefault(listOf(LibraryMode.ARTISTS))

		val kinds = modes.filterNot { it == LibraryMode.UPLOADS }
		_state.update { s ->
			s.copy(
				kinds = kinds,
				kind = if (s.kind in kinds) s.kind else kinds.firstOrNull() ?: s.kind,
			)
		}

		// Uploads last, so a library hit wins the first-spelling tie-break below
		// and the message says "in your library" rather than "in staging" for
		// something that is in both.
		val sources = kinds + LibraryMode.UPLOADS
		val seen = LinkedHashMap<String, NameSuggestion>()
		for (mode in sources) {
			val indexes = runCatchingCancellable {
				library.artistIndexes(BrowseScope.AllServers, mode)
			}.getOrNull()?.items ?: continue
			for (artist in indexes.flatMap { it.artists }) {
				val key = artist.name.lowercase()
				if (seen.containsKey(key)) continue
				seen[key] = NameSuggestion(
					name = artist.name,
					refs = artist.refs,
					where = if (mode == LibraryMode.UPLOADS) "your uploads" else mode.label,
					staging = mode == LibraryMode.UPLOADS,
				)
			}
		}
		_state.update { it.copy(suggestions = seen.values.toList()) }
	}

	/**
	 * What the library already holds under the typed names, as one line.
	 *
	 * **Advisory, and it never blocks a fetch.** Nothing can collide here: the
	 * batch is a fresh UUID directory, and the destination root is not chosen
	 * until an admin promotes it, so whether *that* will be refused is genuinely
	 * unpredictable from here. Saying otherwise would be pretending to know
	 * something we do not.
	 *
	 * Strongest signal only. Three notes stacked under two text boxes is noise,
	 * and knowing the album is already there makes "that artist exists" beside
	 * the point.
	 */
	private fun recheck() {
		checker?.cancel()
		checker = viewModelScope.launch {
			delay(CHECK_DEBOUNCE_MS)
			_state.update { it.copy(existing = describeExisting()) }
		}
	}

	private suspend fun describeExisting(): String? {
		val s = _state.value
		val artist = s.artist.trim()
		val album = s.album.trim()
		val hit = s.suggestions.firstOrNull { it.name.equals(artist, ignoreCase = true) }

		if (hit != null) {
			// Staging and the library need different words: only one of them
			// means the thing is actually in the shared library.
			val place =
				if (hit.staging) "in your uploads, not yet promoted"
				else "in ${hit.where}"

			if (album.isNotBlank()) {
				// That name's own albums — the precise question, and one call
				// however many servers hold the name, because a merged row
				// carries each server's ref.
				val albums = runCatchingCancellable {
					library.albumsOfArtist(hit.refs)
				}.getOrNull()?.items.orEmpty()
				val match = albums.firstOrNull { it.title.equals(album, ignoreCase = true) }
				if (match != null) {
					return "You already have “${match.title}” under ${hit.name} — $place."
				}
			}
			return "“${hit.name}” is already $place."
		}

		// Nothing matched by name, which is the case worth searching for: the
		// same record filed under a spelling you would not have typed. Searched
		// on the album, that being the distinctive string.
		if (album.length < MIN_SEARCH_LENGTH) return null
		val found = runCatchingCancellable {
			library.searchProgressively(BrowseScope.AllServers, album, 0, 3, 0, 0).last()
		}.getOrNull()?.items?.albums.orEmpty()
		val first = found.firstOrNull() ?: return null
		return buildString {
			append("Possibly already there: “${first.title}”")
			if (first.artistName.isNotBlank()) append(" by ${first.artistName}")
			if (found.size > 1) append(" and ${found.size - 1} more")
			append(".")
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
		// A server with something live outranks the remembered one, and is not
		// written back to settings: the strip in the app's bottom bar leads here,
		// and it must land on a panel showing the job it was reporting. Where the
		// user's attention is right now is a stronger statement than a preference
		// written last week — but only for this visit.
		val busy = monitor.status.value.live.map { it.server }.toSet()
		val chosen = targets.firstOrNull { it.id in busy }?.id
			?: targets.firstOrNull { it.id == remembered }?.id
			?: targets.firstOrNull()?.id

		_state.update { it.copy(targets = targets, server = chosen) }
		chosen?.let { alignModeWith(it) }
	}

	/**
	 * The URL is editable, not just displayed.
	 *
	 * It arrives filled in from a share and empty from the uploads listing's own
	 * "Fetch from a URL" row, and both want the same field: the second has
	 * nothing to start from, and the first may have picked the wrong link out of
	 * a paragraph that held two.
	 */
	fun onUrl(v: String) = _state.update { it.copy(url = v, error = null) }

	fun onArtist(v: String) {
		_state.update { it.copy(artist = v, error = null) }
		recheck()
	}

	fun onAlbum(v: String) {
		_state.update { it.copy(album = v, error = null) }
		recheck()
	}

	/**
	 * Changes what the fields are called and what the artist field starts from,
	 * and nothing else — in particular not what [FetchUrlUiState.suggestions]
	 * holds. Not persisted either: a category one week and an album the next is
	 * the ordinary case, and it costs one tap to say so.
	 */
	fun onKind(kind: LibraryMode) {
		if (kind == _state.value.kind) return
		_state.update { it.copy(kind = kind, error = null) }
	}

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
		_state.update { it.copy(artist = "", album = "", existing = null) }
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
					_state.update { it.copy(submitting = false) }
					// Handed straight to the monitor rather than held here, so
					// the row says "Queued" in this frame and goes on being
					// reported by the shell's strip after this panel is closed.
					monitor.adopt(server, s.target?.name.orEmpty(), job)
				},
				onFailure = { e ->
					_state.update {
						it.copy(submitting = false, error = "Could not start: ${e.userMessage()}")
					}
				},
			)
		}
	}

	/**
	 * Takes the id because there can be several rows now, one of which may be a
	 * fetch this panel never started — which is exactly the one a person who has
	 * just found it running wants to be able to stop.
	 */
	fun cancel(id: String) {
		val server = _state.value.server ?: return
		viewModelScope.launch {
			// The monitor reports the new state; a failure here is not worth its
			// own message, since the job either stopped or it did not and the
			// panel is about to say which.
			runCatchingCancellable { fetches.cancel(server, id) }
			monitor.refresh()
		}
	}

	private companion object {
		/** Long enough that typing a name is not a request per keystroke. */
		const val CHECK_DEBOUNCE_MS = 400L

		/** Below this a title search matches half the library and says nothing. */
		const val MIN_SEARCH_LENGTH = 3
	}
}

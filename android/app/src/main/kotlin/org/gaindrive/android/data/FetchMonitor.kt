package org.gaindrive.android.data

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.channels.BufferOverflow
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.SharedFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asSharedFlow
import kotlinx.coroutines.flow.channelFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.distinctUntilChanged
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flatMapLatest
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.launch
import kotlinx.coroutines.withTimeoutOrNull
import org.gaindrive.android.data.model.FetchState
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.gaindrive.android.net.runCatchingCancellable
import javax.inject.Inject
import javax.inject.Singleton

/** One job, and which server is running it: a job id is unique only per server. */
data class FetchJobRef(
	val server: ServerId,
	val serverName: String,
	val job: FetchJobDto,
) {
	val state: FetchState get() = FetchState.of(job.state)
}

/**
 * Every fetch job this account has, across servers, as one value.
 *
 * [contactLost] names servers whose polls have been failing long enough to say
 * so. Their jobs are still listed - the last thing we knew is better than
 * nothing, and a fetch does not stop because the phone lost the Wi-Fi.
 */
data class FetchStatus(
	val jobs: List<FetchJobRef> = emptyList(),
	val contactLost: Set<ServerId> = emptySet(),
) {
	val live: List<FetchJobRef> get() = jobs.filter { it.state.isLive }

	/** The one actually doing something, if any - the server runs one at a time. */
	val moving: FetchJobRef?
		get() = jobs.firstOrNull {
			it.state == FetchState.RUNNING || it.state == FetchState.SCANNING
		}

	fun on(server: ServerId?): List<FetchJobRef> =
		if (server == null) emptyList() else jobs.filter { it.server == server }
}

/**
 * What the account's URL fetches are doing, for as long as the app is on screen.
 *
 * **It exists because a fetch outlives the screen that started it.** The panel
 * used to own the polling in its own `viewModelScope`, and `Route.FetchUrl` is a
 * drill-down - so leaving it cancelled the poll, returning built a fresh view
 * model with no job in it, and the panel then looked exactly like a first visit
 * while the download went on running. Pasting the same URL again is the obvious
 * next move, and it makes a second copy: the server's duplicate check covers
 * only `queued|running|scanning`, and the index check the panel does covers only
 * what a scan has already filed. Between submitting and being indexed a fetch
 * was invisible to both.
 *
 * In `data/` rather than beside the panel because three consumers now want it -
 * the panel, the shell's strip and the uploads listing - and because it must not
 * be scoped to any of them.
 *
 * Separate from [UrlFetchRepository], which stays the transport. That class's
 * own note argues it is not [LibraryRepository] because it is stateless
 * request/response with no fan-out and no lifecycle; a poll loop with a cadence,
 * per-server failure counting and a completion signal is precisely what it says
 * it is not. Same split as [Connectivity] over `NetworkMonitor`.
 */
@Singleton
class FetchMonitor @Inject constructor(
	private val fetches: UrlFetchRepository,
	private val accounts: Accounts,
	private val registry: ServerRegistry,
	private val connectivity: Connectivity,
	private val libraryRevision: LibraryRevision,
	private val scope: CoroutineScope,
) {

	// The store the loop writes and [status] republishes. Held apart from the
	// shared flow so it survives between subscription windows: a rotation drops
	// the last subscriber for a moment, and the strip must not blank and refill.
	private val store = MutableStateFlow(FetchStatus())

	// Wakes the loop out of its sleep. Buffered and dropping, so refresh() never
	// suspends and a burst of them collapses into one sweep.
	private val nudges = MutableSharedFlow<Unit>(
		extraBufferCapacity = 1,
		onBufferOverflow = BufferOverflow.DROP_OLDEST,
	)

	// The last state seen per job id, so a completion is noticed once. The web
	// client keeps the same map for the same reason.
	private val lastState = mutableMapOf<String, String>()

	private val _completions = MutableSharedFlow<FetchJobRef>(extraBufferCapacity = 8)

	/** One per observed transition into `done`. Nothing replays it. */
	val completions: SharedFlow<FetchJobRef> = _completions.asSharedFlow()

	/**
	 * Collecting this is what makes it poll, and that is the design.
	 *
	 * The shell subscribes for as long as the app is composed, so backgrounding
	 * stops the polling after the grace window and foregrounding restarts the
	 * loop - whose first act is a sweep. **"The app came back" and "find out what
	 * is fetching" are therefore the same event**, which is what notices a job
	 * started in the web client, or one that outlived the process, without any
	 * lifecycle plumbing of its own. Polling while the app is away would be
	 * battery spent on a progress bar nobody is looking at, and the server keeps
	 * a finished job for fifteen minutes regardless.
	 */
	@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
	val status: StateFlow<FetchStatus> =
		combine(registry.enabledServers, connectivity.online) { servers, online ->
			if (online) servers else emptyList()
		}
			.distinctUntilChanged()
			.flatMapLatest { servers -> pump(servers) }
			.stateIn(scope, SharingStarted.WhileSubscribed(SUBSCRIBER_GRACE_MS), FetchStatus())

	/** An immediate sweep: a panel opening, or a submit or cancel just landed. */
	fun refresh() {
		nudges.tryEmit(Unit)
	}

	/**
	 * Merges in the job `fetchUrl` just returned, so the panel says "Queued" in
	 * the same frame instead of a poll interval later. The next sweep replaces
	 * it with the server's own account of the same job.
	 */
	fun adopt(server: ServerId, name: String, job: FetchJobDto) {
		store.update { s ->
			val ref = FetchJobRef(server, name, job)
			// Seeded, not compared: a job we have only just created has no
			// previous state, and leaving it out would make its first sighting
			// look like a transition.
			lastState[job.id] = job.state
			FetchStatus(listOf(ref) + s.jobs.filterNot { it.job.id == job.id }, s.contactLost)
		}
		refresh()
	}

	private fun pump(servers: List<ServerConfig>): Flow<FetchStatus> = channelFlow {
		if (servers.isNotEmpty()) launch { loop(servers) }
		store.collect { send(it) }
	}

	private suspend fun loop(servers: List<ServerConfig>) {
		// Whether an account may upload is the endpoint's own precondition -
		// getFetchJobs runs check_upload_perm before it answers - so a server
		// without it would cost one guaranteed error 50 per tick. Accounts is
		// warm by now: the first browse load asks canUploadTo of every scoped
		// server to decide whether to offer an Uploads chip.
		//
		// Deliberately not getUrlHandlers. That answers what may be *offered*,
		// which is the panel's question; this only has to know what to *read*,
		// and a server with upload rights and no handlers simply answers with an
		// empty list. One fewer cached verdict to keep honest.
		val poll = servers.filter { runCatchingCancellable { accounts.canUploadTo(it) }
			.getOrDefault(false) }
		if (poll.isEmpty()) return

		val failures = mutableMapOf<ServerId, Int>()
		while (true) {
			// Only the servers actually holding something get the fast cadence:
			// two servers with one job between them is one request a tick, not
			// two. With nothing live this is every server, which is the sweep
			// that notices a fetch someone started in another client.
			val busy = store.value.live.map { it.server }.toSet()
			val fast = poll.filter { it.id in busy }
			sweep(if (fast.isEmpty()) poll else fast, failures)

			val interval = if (store.value.live.isEmpty()) IDLE_MS else LIVE_MS
			withTimeoutOrNull(interval) { nudges.first() }
		}
	}

	private suspend fun sweep(
		servers: List<ServerConfig>,
		failures: MutableMap<ServerId, Int>,
	) {
		val done = mutableListOf<FetchJobRef>()
		for (config in servers) {
			val jobs = runCatchingCancellable { fetches.jobs(config.id) }.getOrNull()
			if (jobs == null) {
				// A failed poll never clears what is known. It is usually the
				// server busy indexing what it has just downloaded, which is the
				// one moment a fetch most wants reporting on.
				val n = (failures[config.id] ?: 0) + 1
				failures[config.id] = n
				if (n >= MAX_POLL_FAILURES) {
					store.update { it.copy(contactLost = it.contactLost + config.id) }
				}
				continue
			}
			failures[config.id] = 0

			val refs = jobs.map { FetchJobRef(config.id, config.name, it) }
			done += completedSince(lastState, refs)

			store.update { s ->
				FetchStatus(
					jobs = refs + s.jobs.filterNot { it.server == config.id },
					contactLost = s.contactLost - config.id,
				)
			}
		}

		// Once for the sweep, not once per job: two fetches finishing together
		// are one thing to re-read, and each bump is a full reload of every
		// browse screen watching.
		//
		// The listing that has to change is the uploads one, and this is the
		// mechanism that already exists for it: ServerSelection.browse combines
		// LibraryRevision, and every browse screen collects that. So
		// ArtistsViewModel needs no change - its deliberate "kept until
		// something asks for it again" cache is right, and this is something
		// asking.
		//
		// No delay before it, and no forgetLibrary(): the server scans before it
		// reports done - which is what the separate `scanning` state is for - so
		// the library is already correct, and a fetch only adds a batch rather
		// than moving ids about as a promote does.
		if (done.isNotEmpty()) libraryRevision.bump()
		for (ref in done) _completions.tryEmit(ref)
	}

	private companion object {
		/** Fast enough for a progress bar, and what the panel always used. */
		const val LIVE_MS = 2_000L

		/**
		 * One request per upload-capable server per minute of foreground time is
		 * the price of noticing a fetch started in another client. The two-second
		 * rate is reserved for a job already known to be moving.
		 */
		const val IDLE_MS = 60_000L

		/** What a rotation must not look like. Matches the shell's other flows. */
		const val SUBSCRIBER_GRACE_MS = 5_000L

		/** Half a minute, which a scan of a large batch can legitimately take. */
		const val MAX_POLL_FAILURES = 15
	}
}

/**
 * Which of [refs] have just finished, updating [previous] as it goes.
 *
 * Pulled out of the sweep so the rule can be exercised without a server, a
 * scope or an Android runtime; it is the rule that decides whether the uploads
 * listing is re-read, and getting it wrong is either a listing that never
 * refreshes or one that refreshes every two seconds for a quarter of an hour.
 *
 * **A transition we actually saw, never a first sighting.** `was == null` is a
 * job that was already finished when we first looked: it landed before anyone
 * was watching, and the browse load that follows launch includes it anyway.
 *
 * Deliberately *not* also gated on the loop's first pass. A fetch that finishes
 * while the app is backgrounded is the commonest completion there is - the poll
 * stops, the job ends, and the first sweep after foregrounding is the only
 * chance anything has to notice it. The caller's map outlives its loop for
 * exactly that reason.
 */
internal fun completedSince(
	previous: MutableMap<String, String>,
	refs: List<FetchJobRef>,
): List<FetchJobRef> {
	val done = mutableListOf<FetchJobRef>()
	for (ref in refs) {
		val was = previous.put(ref.job.id, ref.job.state)
		if (was != null && was != ref.job.state && ref.state == FetchState.DONE) done += ref
	}
	return done
}

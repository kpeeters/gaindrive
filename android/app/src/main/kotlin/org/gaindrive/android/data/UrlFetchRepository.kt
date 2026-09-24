package org.gaindrive.android.data

import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeoutOrNull
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.FetchJobDto
import org.gaindrive.android.net.OfflineException
import org.gaindrive.android.net.SubsonicClient
import org.gaindrive.android.net.SubsonicClientFactory
import org.gaindrive.android.net.SubsonicException
import org.gaindrive.android.net.UrlHandlerDto
import org.gaindrive.android.net.requireOk
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Having the server fetch a pasted URL into the signed-in account's own upload
 * area.
 *
 * Separate from [LibraryRepository] rather than another dozen methods on it:
 * nothing here reads or writes the library, none of it fans out across servers
 * - a fetch lands on exactly one - and none of it has an offline story beyond
 * refusing.
 */
@Singleton
class UrlFetchRepository @Inject constructor(
	private val clients: SubsonicClientFactory,
	private val registry: ServerRegistry,
	private val connectivity: Connectivity,
) {

	private val mutex = Mutex()
	private val handlers = mutableMapOf<ServerId, List<UrlHandlerDto>>()

	/**
	 * Which URLs [config] can fetch. Empty means it cannot be offered at all.
	 *
	 * **One call answers two questions.** `getUrlHandlers` runs the server's own
	 * upload-permission check before it answers, so error 50 means the account
	 * has no upload role and any other error means the server is too old to know
	 * the endpoint. Both reduce to "not eligible", which is why there is no
	 * separate `getUser` probe for `uploadRole` here.
	 *
	 * Cached in memory for the process, like [Accounts]: one cheap call per
	 * server per launch, a server-side change picked up on the next launch, and
	 * nothing to migrate.
	 *
	 * **Only a verdict is cached.** A server that answered - with handlers, or
	 * with an error saying this account may not upload - has settled the
	 * question and is not asked again. A timeout, a dead Wi-Fi or offline mode
	 * has settled nothing, and caching it would leave the feature unavailable
	 * until the app was restarted.
	 */
	suspend fun handlersFor(config: ServerConfig): List<UrlHandlerDto> = mutex.withLock {
		handlers[config.id]?.let { return@withLock it }
		if (!connectivity.online.value) return@withLock emptyList()
		probe(config)?.also { handlers[config.id] = it } ?: emptyList()
	}

	/** Drops a cached verdict, so an account whose rights just changed is re-asked. */
	suspend fun forget(server: ServerId) = mutex.withLock {
		handlers.remove(server)
		Unit
	}

	/** Null means the server did not answer, which is not a verdict about it. */
	private suspend fun probe(config: ServerConfig): List<UrlHandlerDto>? =
		withTimeoutOrNull(PROBE_TIMEOUT_MS) {
			try {
				clients.clientFor(config)
					.getUrlHandlers()
					.requireOk()
					.urlHandlers
					?.urlHandler
					.orEmpty()
			} catch (e: CancellationException) {
				throw e
			} catch (e: SubsonicException) {
				// The server answered and said no: error 50 for an account with
				// no upload role, something else for a server too old to know
				// the endpoint. Either way this server cannot be offered, and
				// neither is a fault anyone can fix from a share sheet.
				emptyList()
			} catch (e: Exception) {
				null
			}
		}

	/**
	 * Queues a fetch and returns the job as the server created it. Its `id` is
	 * the only handle on this one job - [jobs] returns everything the account
	 * has running.
	 *
	 * Blank [artist] or [album] is sent as nothing at all rather than as an
	 * empty value: the server reads an absent parameter as "keep whatever the
	 * handler parsed from the title", and a present-but-unusable one as an
	 * error.
	 */
	suspend fun submit(
		server: ServerId,
		url: String,
		audio: Boolean,
		artist: String,
		album: String,
	): FetchJobDto = onServer(server) { client ->
		client.fetchUrl(
			url = url,
			mode = if (audio) "audio" else "video",
			artist = artist.trim().ifBlank { null },
			album = album.trim().ifBlank { null },
		).requireOk().fetchJob ?: error("The server queued a fetch but did not describe it")
	}

	/** The account's own jobs on [server], newest first. */
	suspend fun jobs(server: ServerId): List<FetchJobDto> = onServer(server) { client ->
		client.getFetchJobs().requireOk().fetchJobs?.fetchJob.orEmpty()
	}

	suspend fun cancel(server: ServerId, jobId: String) = onServer(server) { client ->
		client.cancelFetch(jobId).requireOk()
		Unit
	}

	/**
	 * Resolves the client off the main thread and refuses up front when there is
	 * no network. Nothing here can be queued for later - the work is the
	 * server's - so failing at once is the honest answer, which is the same
	 * reasoning as `LibraryRepository.requireOnline()`.
	 */
	private suspend fun <T> onServer(
		server: ServerId,
		block: suspend (SubsonicClient) -> T,
	): T = withContext(Dispatchers.IO) {
		if (!connectivity.online.value) {
			throw OfflineException("You are offline, so nothing can be fetched.")
		}
		val config = registry.get(server) ?: error("No such server configured: $server")
		block(clients.clientFor(config))
	}

	private companion object {
		/**
		 * Short: the panel waits for every configured server's probe before it
		 * can draw, and an unreachable one must not hold up the ones that
		 * answered.
		 */
		const val PROBE_TIMEOUT_MS = 4_000L
	}
}

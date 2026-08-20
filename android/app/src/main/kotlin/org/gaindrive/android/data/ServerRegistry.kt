package org.gaindrive.android.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.coroutines.flow.update
import org.gaindrive.android.data.crypto.CredentialCipher
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.model.ServerConfig
import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.SubsonicClientFactory
import javax.inject.Inject
import javax.inject.Singleton

/**
 * The single source of truth for which servers exist. Owns encryption on the
 * way in and decryption on the way out, so nothing above this layer handles
 * ciphertext.
 *
 * List order is meaningful: it is the tie-break for merged browse results and
 * the order of per-server sections.
 */
@Singleton
class ServerRegistry @Inject constructor(
	private val store: ServerStore,
	private val cipher: CredentialCipher,
	private val clients: SubsonicClientFactory,
	private val local: LocalLibrary,
	private val roots: MusicRoots,
) {

	val servers: Flow<List<ServerConfig>> = store.servers.map { stored ->
		stored.map { it.toConfig() }
	}

	val enabledServers: Flow<List<ServerConfig>> =
		servers.map { list -> list.filter { it.enabled } }

	suspend fun get(id: ServerId): ServerConfig? =
		servers.first().firstOrNull { it.id == id }

	/**
	 * Bumped on **every** change to the server list, and watched by the browse
	 * screens as their signal to re-read the library.
	 *
	 * It has to exist because the scope alone cannot carry the change:
	 * `BrowseScope.AllServers` is a singleton, so disabling one server and
	 * enabling another produces an identical scope, an identical `BrowseState`,
	 * and a `distinctUntilChanged` that swallows it — leaving the disabled
	 * server's artists on screen under the newly enabled server's badges. Every
	 * other in-place edit has the same shape: a corrected URL, a reorder (which
	 * is the tie-break for merged rows), a browse-mode flip.
	 *
	 * Bumped in [mutate] and nowhere else, which is why [add] goes through it
	 * rather than saving for itself.
	 */
	private val _revision = MutableStateFlow(0)
	val revision: StateFlow<Int> = _revision.asStateFlow()

	/** Returns the new server's id. */
	suspend fun add(
		name: String,
		url: String,
		username: String,
		password: String,
		browseByFolder: Boolean,
	): ServerId {
		val id = ServerId.new()
		val normalised = ServerConfig.normaliseUrl(url)
		val entry = StoredServer(
			id = id.value,
			name = name.ifBlank { ServerConfig.defaultName(normalised) },
			url = normalised,
			username = ServerConfig.normaliseUsername(username),
			password = cipher.encrypt(ServerConfig.normalisePassword(password)),
			browseByFolder = browseByFolder,
		)
		mutate { it + entry }
		return id
	}

	/**
	 * Blank [password] keeps the stored one, so the editor does not have to
	 * round-trip a secret through the UI just to rename a server.
	 */
	suspend fun update(
		id: ServerId,
		name: String,
		url: String,
		username: String,
		password: String,
		browseByFolder: Boolean,
	) {
		// Read before mutating: whether the browse mode changed is only
		// answerable against the value that was there.
		val previous = store.servers.first().firstOrNull { it.id == id.value }

		mutate { list ->
			list.map { entry ->
				if (entry.id != id.value) entry
				else {
					val normalised = ServerConfig.normaliseUrl(url)
					// Trimmed before the blank check, so a field holding
					// nothing but pasted whitespace keeps the stored password
					// rather than replacing it with empty.
					val newPassword = ServerConfig.normalisePassword(password)
					entry.copy(
						name = name.ifBlank { ServerConfig.defaultName(normalised) },
						url = normalised,
						username = ServerConfig.normaliseUsername(username),
						password = if (newPassword.isEmpty()) entry.password
						else cipher.encrypt(newPassword),
						browseByFolder = browseByFolder,
					)
				}
			}
		}
		// Credentials or address may have changed; the cached client was built
		// from the old ones.
		clients.forget(id)

		// A browse mode changed underneath everything derived from it. The two
		// hierarchies are separate id spaces on a server that keeps them apart,
		// so the mirrored library is not stale — it names things the new mode
		// will never ask for, and would answer offline browsing with rows that
		// cannot be opened. The chips are computed from the roots and the flag
		// together, so that cache goes too.
		//
		// The reload itself is not arranged here: `mutate` has already bumped
		// the revision, as it does for any change to the list.
		if (previous != null && previous.browseByFolder != browseByFolder) {
			roots.forget(id)
			local.forgetServer(id)
		}
	}

	suspend fun setEnabled(id: ServerId, enabled: Boolean) =
		mutate { list -> list.map { if (it.id == id.value) it.copy(enabled = enabled) else it } }

	suspend fun remove(id: ServerId) {
		mutate { list -> list.filterNot { it.id == id.value } }
		clients.forget(id)
		roots.forget(id)
		// Its mirrored library would otherwise sit there forever, unreachable
		// and unremovable — nothing else knows the server ever existed.
		local.forgetServer(id)
	}

	suspend fun move(from: Int, to: Int) = mutate { list ->
		if (from !in list.indices || to !in list.indices) list
		else list.toMutableList().apply { add(to, removeAt(from)) }
	}

	/**
	 * The single write path, so [revision] has one place to be bumped from.
	 *
	 * A change that changes nothing is dropped rather than announced — `move`
	 * with out-of-range indices returns the list it was given, and re-saving an
	 * editor without touching a field produces an identical one. DataStore
	 * already declines to re-emit for an unchanged value, but [revision] is a
	 * separate flow and would fire regardless, reloading every browse screen for
	 * nothing.
	 */
	private suspend fun mutate(block: (List<StoredServer>) -> List<StoredServer>) {
		val current = store.servers.first()
		val next = block(current)
		if (next == current) return
		store.save(next)
		_revision.update { it + 1 }
	}

	/**
	 * A server whose password cannot be decrypted keeps its place in the list
	 * with an empty password: it shows as needing attention rather than
	 * silently vanishing, which is what the user needs to see after a restore
	 * onto a device that has no Keystore key.
	 */
	private fun StoredServer.toConfig() = ServerConfig(
		id = ServerId(id),
		name = name,
		url = url,
		username = username,
		password = cipher.decryptOrNull(password).orEmpty(),
		enabled = enabled,
		browseByFolder = browseByFolder,
	)
}

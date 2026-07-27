package org.gaindrive.android.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import org.gaindrive.android.data.crypto.CredentialCipher
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
) {

	val servers: Flow<List<ServerConfig>> = store.servers.map { stored ->
		stored.map { it.toConfig() }
	}

	val enabledServers: Flow<List<ServerConfig>> =
		servers.map { list -> list.filter { it.enabled } }

	suspend fun get(id: ServerId): ServerConfig? =
		servers.first().firstOrNull { it.id == id }

	/** Returns the new server's id. */
	suspend fun add(name: String, url: String, username: String, password: String): ServerId {
		val id = ServerId.new()
		val normalised = ServerConfig.normaliseUrl(url)
		val entry = StoredServer(
			id = id.value,
			name = name.ifBlank { ServerConfig.defaultName(normalised) },
			url = normalised,
			username = username,
			password = cipher.encrypt(password),
		)
		store.save(store.servers.first() + entry)
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
	) {
		mutate { list ->
			list.map { entry ->
				if (entry.id != id.value) entry
				else {
					val normalised = ServerConfig.normaliseUrl(url)
					entry.copy(
						name = name.ifBlank { ServerConfig.defaultName(normalised) },
						url = normalised,
						username = username,
						password = if (password.isBlank()) entry.password
						else cipher.encrypt(password),
					)
				}
			}
		}
		// Credentials or address may have changed; the cached client was built
		// from the old ones.
		clients.forget(id)
	}

	suspend fun setEnabled(id: ServerId, enabled: Boolean) =
		mutate { list -> list.map { if (it.id == id.value) it.copy(enabled = enabled) else it } }

	suspend fun remove(id: ServerId) {
		mutate { list -> list.filterNot { it.id == id.value } }
		clients.forget(id)
	}

	suspend fun move(from: Int, to: Int) = mutate { list ->
		if (from !in list.indices || to !in list.indices) list
		else list.toMutableList().apply { add(to, removeAt(from)) }
	}

	private suspend fun mutate(block: (List<StoredServer>) -> List<StoredServer>) {
		store.save(block(store.servers.first()))
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
	)
}

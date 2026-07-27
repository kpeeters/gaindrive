package org.gaindrive.android.data.model

/**
 * A configured server as the rest of the app sees it. The password is held in
 * plaintext in memory because the Subsonic token scheme has to recompute
 * `md5(password + salt)` per session; it is encrypted at rest.
 */
data class ServerConfig(
	val id: ServerId,
	val name: String,
	/** Base URL with any trailing slash removed. */
	val url: String,
	val username: String,
	val password: String,
	val enabled: Boolean = true,
) {
	companion object {
		/** Trailing slashes break path joining; the web client strips them too. */
		fun normaliseUrl(raw: String): String = raw.trim().trimEnd('/')

		/**
		 * Falls back to the URL's host so a server always has something to
		 * show in a list, even if the user never types a name.
		 */
		fun defaultName(url: String): String =
			runCatching { java.net.URI(url).host }.getOrNull()?.takeIf { it.isNotBlank() }
				?: url.removePrefix("https://").removePrefix("http://").substringBefore('/')
	}
}

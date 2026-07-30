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
		 * No Subsonic username meaningfully starts or ends with a space, and
		 * long opaque ones — Bandcamp issues a 32-character token as the
		 * username — are pasted, which is exactly how a stray space gets in.
		 * It then travels into `u=` on every request as `%20`, and a server
		 * whose `ping` does not authenticate will accept the account and then
		 * fail on the first endpoint that looks the user up.
		 */
		fun normaliseUsername(raw: String): String = raw.trim()

		/**
		 * Falls back to the URL's host so a server always has something to
		 * show in a list, even if the user never types a name.
		 */
		fun defaultName(url: String): String =
			runCatching { java.net.URI(url).host }.getOrNull()?.takeIf { it.isNotBlank() }
				?: url.removePrefix("https://").removePrefix("http://").substringBefore('/')
	}
}

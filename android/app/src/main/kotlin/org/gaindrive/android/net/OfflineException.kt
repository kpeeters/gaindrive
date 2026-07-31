package org.gaindrive.android.net

import java.io.IOException

/**
 * Raised instead of making a request the app knows would not be attempted:
 * either there is no network, or the user has asked for offline mode.
 *
 * An IOException like [SubsonicException], so it travels the same path as a
 * transport failure and no call site has to catch a third family. The message
 * is written to be shown as-is — `Throwable.userMessage()` falls through to it.
 */
class OfflineException(
	override val message: String = "You are offline.",
) : IOException(message)

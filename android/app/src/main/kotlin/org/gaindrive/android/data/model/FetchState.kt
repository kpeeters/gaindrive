package org.gaindrive.android.data.model

/**
 * Where a job has got to.
 *
 * Mapped from the server's string rather than parsed as an enum, so a state a
 * newer server invents shows as [UNKNOWN] instead of failing the response. The
 * three terminal ones are told apart because they need different words and
 * different colours, not because anything branches on them.
 *
 * In `data/model` rather than beside the panel that draws it, because
 * `FetchMonitor` decides from it which jobs are still live and `data` may not
 * depend on `ui`. Two spellings of "still live" would be two answers to whether
 * the form should refuse a duplicate.
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

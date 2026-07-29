package org.gaindrive.android.playback

/**
 * The rules governing where the hand-picked part of the queue ends and the
 * automatic tail begins, as the web client models it.
 *
 * Pure arithmetic, separated from [PlayerConnection] so it can be tested: these
 * are one-line rules whose failure mode is severe — an off-by-one in
 * [afterAppend] or [tailToDrop] means "add to queue" quietly deletes tracks the
 * user still wanted.
 *
 * `value` is the index of the first automatic entry. Everything before it was
 * put there deliberately; everything from it onwards is the remainder of an
 * album and may be discarded.
 */
@JvmInline
value class QueueBoundary(val value: Int) {

	/**
	 * Playing an album from [startIndex]: that track and everything before it
	 * is what the user asked for, the rest is the album carrying on.
	 */
	fun afterPlay(startIndex: Int) = QueueBoundary(startIndex + 1)

	/** Appending a hand-picked track to a queue that now holds [newCount]. */
	fun afterAppend(newCount: Int) = QueueBoundary(newCount)

	/**
	 * Inserting a hand-picked track at [at]. Landing in the automatic region
	 * makes that position manual; landing inside the manual region simply makes
	 * it one longer.
	 */
	fun afterInsert(at: Int) =
		QueueBoundary(if (value <= at) at + 1 else value + 1)

	/** Removing the entry at [index] shifts the boundary only if it was before it. */
	fun afterRemove(index: Int) =
		QueueBoundary(if (index < value) value - 1 else value)

	/**
	 * The automatic tail to discard before appending, or null when there is
	 * none. Returned as a half-open range so the caller can hand it straight to
	 * `removeMediaItems`.
	 */
	fun tailToDrop(queueSize: Int): IntRange? =
		if (value < queueSize) value until queueSize else null

	companion object {
		val EMPTY = QueueBoundary(0)

		/**
		 * A queue that outlived the process is treated as entirely hand-picked.
		 * Assuming the opposite would let the next enqueue delete a queue the
		 * user still wanted, which is much worse than keeping too much.
		 */
		fun adoptingExisting(queueSize: Int) = QueueBoundary(queueSize)
	}
}

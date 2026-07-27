package org.gaindrive.android.ui

/**
 * The three states every screen has. A sealed type rather than a data class
 * with nullable fields, so "loaded but empty" and "not loaded yet" cannot be
 * confused — which is the bug that produces a flash of "nothing here" on every
 * screen open.
 */
sealed interface Load<out T> {
	data object Loading : Load<Nothing>
	data class Failed(val message: String) : Load<Nothing>
	data class Ready<T>(val value: T) : Load<T>
}

fun <T> Load<T>.valueOrNull(): T? = (this as? Load.Ready)?.value

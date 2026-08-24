package org.gaindrive.android.data

import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Bumped when something the app did has *moved* things in a server's library,
 * so every browse screen must read it again.
 *
 * Today that is only `promoteAlbum`, which takes an album out of the uploads
 * area and puts it in the shared library. Two listings are wrong the moment it
 * succeeds and neither is the one the user is looking at, so nothing else would
 * ever correct them: a browse screen holds its list until its scope changes, by
 * design — see the comment on `ArtistsViewModel._state`.
 *
 * A separate object rather than a field on [LibraryRepository] because
 * [ServerSelection] is what browse screens watch and it must not depend on the
 * repository for one integer. It is deliberately **not**
 * `LocalLibrary.revision`, which is bumped by every mirror write including the
 * ones a browse itself performs: folding that into the reload signal would be a
 * loop.
 */
@Singleton
class LibraryRevision @Inject constructor() {

	// Named to match `ServerRegistry.revision`, which it sits beside in the one
	// combine that reads both.
	private val _revision = MutableStateFlow(0)
	val revision: StateFlow<Int> = _revision.asStateFlow()

	fun bump() = _revision.update { it + 1 }
}

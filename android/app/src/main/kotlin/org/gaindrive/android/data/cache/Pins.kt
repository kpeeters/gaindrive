package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.Album
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song

/** What a pin was placed on. Only [PinKind.SONG] names a cache key directly. */
enum class PinKind { SONG, ALBUM, PLAYLIST }

data class Pin(val ref: ItemRef, val kind: PinKind)

/** A pin with a name on it, for anywhere that lists them back to the user. */
data class PinnedItem(val pin: Pin, val label: String)

/**
 * What each pin covers: its own encoded ref, to the encoded refs of the songs
 * under it.
 *
 * Bare refs on both sides, not cache keys — quality belongs to the bytes, not
 * to what a pin covers, and the same pin protects whichever quality is set at
 * the time. `PinRepository.applyProtection` is where the union is turned into
 * cache keys for the evictor. See `CacheKeys` for why the distinction matters.
 *
 * Kept per pin rather than flattened, because the two questions want different
 * shapes — eviction needs the union ([allKeys]), while the download indicator
 * needs to know how much of *this* album has arrived.
 */
@JvmInline
value class PinCoverage(val byPin: Map<String, List<String>>) {
	val allKeys: Set<String> get() = byPin.values.flatMapTo(mutableSetOf()) { it }
}

/**
 * The audio cache keys a set of pins covers.
 *
 * Pure, and takes membership already resolved, so the rule that decides what
 * survives eviction can be tested without a database or a cache. Membership is
 * passed in rather than looked up because it changes underneath a pin — a
 * playlist gains a track and the pin has to cover it.
 *
 * Unknown members are simply absent: pinning an album that has never been
 * opened protects nothing until its track list has been seen, which is the
 * honest answer rather than a guess.
 */
fun expandPins(
	pins: List<Pin>,
	albumSongs: Map<ItemRef, List<ItemRef>>,
	playlistSongs: Map<ItemRef, List<ItemRef>>,
): PinCoverage = PinCoverage(
	pins.associate { pin ->
		val songs = when (pin.kind) {
			PinKind.SONG -> listOf(pin.ref)
			PinKind.ALBUM -> albumSongs[pin.ref].orEmpty()
			PinKind.PLAYLIST -> playlistSongs[pin.ref].orEmpty()
		}
		pin.ref.encode() to songs.map { it.encode() }
	}
)

/**
 * The pictures a pin covers, which is a different question from the audio.
 *
 * Pure and fed already-resolved rows for the same reason [expandPins] is:
 * the rule is worth testing without a database behind it.
 *
 * [album] may be null even for an album pin, because the mirror is emptied
 * underneath a recompute when a server is removed or its browse mode changes.
 * The fallback is the pin's own ref, which is always a valid `getCoverArt` id
 * in gaindrive, where a cover art id is a folder id.
 *
 * An album pin takes the artist portrait too. It is one extra file, and it is
 * the only art here that cannot simply be re-fetched on demand: the server has
 * to go out to MusicBrainz and friends to find one, which offline cannot
 * happen at all.
 */
fun artRefsOf(pin: Pin, album: Album?, songs: List<Song>): List<ItemRef> = when (pin.kind) {
	PinKind.SONG -> listOfNotNull(songs.firstOrNull()?.coverArt ?: pin.ref)
	PinKind.ALBUM -> listOfNotNull(
		album?.coverArt ?: pin.ref,
		album?.artistRef,
	) + songs.mapNotNull { it.coverArt }
	// A playlist has no art of its own, so it is worth exactly the covers of
	// what is on it.
	PinKind.PLAYLIST -> songs.mapNotNull { it.coverArt }
}.distinct()

/**
 * A video is part of what a pin covers only when it is being played for its
 * soundtrack — `SettingsStore.videoAudioOnly`.
 *
 * Otherwise it could never complete: a video the server can only re-encode has
 * no `Content-Length`, so nothing downstream can decide the copy is whole, and
 * the byte cache is sized for tracks rather than films. With the setting on,
 * what is fetched is an ordinary audio transcode with a real length, and none
 * of that applies. The individual action follows the same rule (see
 * `TrackActionsSheet`); this is the collection case, where the video is
 * incidental and the rest of the album should still download.
 *
 * Turning the setting back off leaves the stored bytes behind but stops the pin
 * covering them, so eviction reclaims them in its own time. That is the right
 * way round — a pin means "keep what I can play", and with the setting off the
 * film is not something this app plays from the cache.
 *
 * Here rather than private to `PinRepository`, and spelled as a predicate over
 * one track, because `StoredContainers` asks the same question of collections
 * nobody pinned and has rows rather than songs to ask it of. A second spelling
 * — an `isVideo = 0` in SQL, say — would make an album holding a film read as
 * complete in a list and as permanently incomplete on its own screen.
 */
fun covered(isVideo: Boolean, audioOnly: Boolean): Boolean = !isVideo || audioOnly

/** [covered] over a list of songs, which is the shape a pin expands to. */
fun List<Song>.downloadable(audioOnly: Boolean): List<Song> =
	filter { covered(it.isVideo, audioOnly) }

/**
 * How long to let the mirror settle before recomputing what it says.
 *
 * Both classes that listen on `LocalLibrary.revision` want the same wait and
 * for the same reason: one browse writes several tables in a burst, and only
 * the settled result is worth the recompute.
 */
const val REVISION_DEBOUNCE_MS = 500L

/**
 * Which collections are entirely on the device, from their membership and the
 * set of song keys that are here.
 *
 * The unpinned counterpart of [pinPhaseOf]: that one answers "how far has this
 * download got", this one "is all of this here anyway", which is what lets an
 * album played straight through be told apart from one nobody has.
 *
 * **An empty membership is not complete**, and that is the whole reason this is
 * a function rather than a `containsAll`. The mirror only holds the tracks of
 * collections visited while online, so an album never opened has no members at
 * all — and [PinStatus.fraction] reports exactly that case as 1f, which is the
 * right answer for a pin the user placed and the wrong one here. Unknown has to
 * read as unknown, or every album in a fresh library claims to be downloaded.
 */
fun collectionsFullyStored(
	membership: Map<String, List<String>>,
	here: Set<String>,
): Set<String> =
	membership.filterValues { keys -> keys.isNotEmpty() && here.containsAll(keys) }.keys

/** What a pin is doing, as far as the icon reporting it is concerned. */
enum class PinPhase {
	/** Every track it covers is stored. */
	COMPLETE,

	/** Downloading, or just asked for and not yet reported on. */
	RUNNING,

	/** Queued, but a requirement is unmet — in practice, waiting for Wi-Fi. */
	WAITING,

	/** At least one track failed and nothing is retrying it. */
	FAILED,
}

/** How far a pin has got, for the icon that reports it. */
data class PinStatus(
	val stored: Int,
	val total: Int,
	val phase: PinPhase,
) {
	val complete: Boolean get() = phase == PinPhase.COMPLETE

	val fraction: Float get() = if (total <= 0) 1f else stored.toFloat() / total
}

/**
 * Which of the four a pin is in.
 *
 * Order matters. Completion wins outright. A failure only shows once nothing is
 * still trying, so one track failing part-way through an album does not stop the
 * rest reporting progress. "Waiting" needs something actually queued behind it,
 * or an album that finished would report itself as waiting the moment the
 * connection went metered.
 *
 * Anything else is [PinPhase.RUNNING], including the moment just after a tap
 * when the download manager has not reported anything yet — showing failure
 * there would flash an error on every single pin.
 */
fun pinPhaseOf(
	coveredKeys: List<String>,
	storedKeys: Set<String>,
	downloads: DownloadStates,
): PinPhase {
	val stored = coveredKeys.count { it in storedKeys }
	val queued = coveredKeys.count { it in downloads.active }
	return when {
		stored >= coveredKeys.size -> PinPhase.COMPLETE
		queued == 0 && coveredKeys.any { it in downloads.failed } -> PinPhase.FAILED
		queued > 0 && downloads.notMetRequirements != 0 -> PinPhase.WAITING
		else -> PinPhase.RUNNING
	}
}

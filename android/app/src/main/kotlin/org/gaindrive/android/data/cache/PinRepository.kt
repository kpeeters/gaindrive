package org.gaindrive.android.data.cache

import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.FlowPreview
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.SharingStarted
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.combine
import kotlinx.coroutines.flow.debounce
import kotlinx.coroutines.flow.drop
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.stateIn
import kotlinx.coroutines.launch
import org.gaindrive.android.data.ServerRegistry
import org.gaindrive.android.data.SettingsStore
import org.gaindrive.android.data.StreamUrls
import org.gaindrive.android.data.local.LocalLibrary
import org.gaindrive.android.data.local.PinDao
import org.gaindrive.android.data.local.PinEntity
import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.Song
import org.gaindrive.android.playback.playableAudioFor
import javax.inject.Inject
import javax.inject.Singleton

/** Why a pin was refused, or that it was not. */
sealed interface PinResult {
	data object Ok : PinResult

	/** Pinning this would put more than the cap out of eviction's reach. */
	data class TooLarge(val neededBytes: Long, val capBytes: Long) : PinResult

	/** Nothing stored to download yet — open it once while online first. */
	data object NotKnownYet : PinResult
}

/**
 * What the user has asked to keep, and everything that follows from it:
 * protection from eviction, and downloading the audio now rather than
 * whenever it next happens to be played.
 */
@Singleton
class PinRepository @Inject constructor(
	private val pinDao: PinDao,
	private val local: LocalLibrary,
	private val pinnedKeys: PinnedKeys,
	private val downloads: DownloadQueue,
	private val audioCache: AudioCache,
	private val settings: SettingsStore,
	private val registry: ServerRegistry,
	private val streamUrls: StreamUrls,
	scope: CoroutineScope,
) {

	private val _pins = MutableStateFlow<List<Pin>>(emptyList())

	/** The pins themselves, as placed — an album pin stays one entry. */
	val pins: StateFlow<List<Pin>> = _pins.asStateFlow()

	private val _coverage = MutableStateFlow(PinCoverage(emptyMap()))

	private val _protectedKeys = MutableStateFlow<Set<String>>(emptySet())

	/** The songs those pins expand to, for the "downloaded" markers. */
	val protectedKeys: StateFlow<Set<String>> = _protectedKeys.asStateFlow()

	/**
	 * How far each pin has got, keyed by its encoded ref.
	 *
	 * Progress is counted in whole tracks, not bytes: Media3 pushes download
	 * state changes but not continuous progress, and the cache's own notion of
	 * "stored" is per track anyway. For an album that reads naturally — three of
	 * twelve — and for a single track it is the difference between a spinner and
	 * a tick, which is all that was missing.
	 */
	val statuses: StateFlow<Map<String, PinStatus>> = combine(
		_coverage,
		audioCache.cachedKeys,
		downloads.states,
	) { coverage, stored, downloadStates ->
		// A finished download counts even when the cache cannot vouch for it —
		// see DownloadStates.completed on why it often cannot.
		val here = stored + downloadStates.completed
		coverage.byPin.mapValues { (_, keys) ->
			PinStatus(
				stored = keys.count { it in here },
				total = keys.size,
				phase = pinPhaseOf(keys, here, downloadStates),
			)
		}
	}.stateIn(scope, SharingStarted.Eagerly, emptyMap())

	/**
	 * Keeps an existing download library from being re-fetched on upgrade.
	 *
	 * Before quality was a setting, everything downloaded was the original file.
	 * Letting the new default apply to an install that already has pins would
	 * quietly re-download all of them at Opus, over whatever connection happens
	 * to be there. Recording what those bytes actually are instead leaves the
	 * choice to the user, who can change it in Settings and get the dialog.
	 *
	 * Runs before the pin collector below, so protection is never applied under
	 * the wrong quality even briefly.
	 */
	private suspend fun adoptExistingDownloadQuality() {
		if (settings.audioQualityChosen.first()) return
		if (pinDao.all().isEmpty()) return
		settings.setAudioQuality(AudioQuality.ORIGINAL)
	}

	@OptIn(FlowPreview::class)
	private fun start(scope: CoroutineScope) {
		scope.launch {
			adoptExistingDownloadQuality()
			pinDao.observeAll().collect { rows ->
				// A pin for a server that has since been removed protects a key
				// nothing will ever play. Dropped here rather than from the
				// removal itself: the registry cannot call into this without
				// closing a loop, and removing the rows re-runs this collector
				// with them gone.
				val known = registry.servers.first().map { it.id }.toSet()
				val (live, orphans) = rows.mapNotNull { it.toPin() }
					.partition { it.ref.server in known }
				orphans.map { it.ref.server }.distinct()
					.forEach { pinDao.removeForServer("${it.value}/%") }
				applyProtection(live)
			}
		}
		// A pinned playlist gains a track, or a pinned album's track list is
		// seen for the first time: what the pin covers has changed even though
		// the pin has not. Debounced because a browse writes several tables in
		// a burst and only the settled result matters.
		scope.launch {
			local.revision.drop(1).debounce(REVISION_DEBOUNCE_MS).collect {
				applyProtection(_pins.value)
			}
		}
	}

	init {
		start(scope)
	}

	fun isPinned(ref: ItemRef): Boolean = _pins.value.any { it.ref == ref }

	/**
	 * Pins [ref] and starts downloading what it covers.
	 *
	 * Refused when it would not fit: eviction cannot reclaim pinned bytes, so a
	 * pin past the cap silently turns the cap into a lie. Better to say so here,
	 * where raising the cap is one screen away.
	 */
	suspend fun pin(ref: ItemRef, kind: PinKind): PinResult {
		val songs = songsFor(Pin(ref, kind))
		if (songs.isEmpty()) return PinResult.NotKnownYet

		val cap = settings.cacheMaxBytes.first()
		val quality = settings.audioQuality.first()
		val alreadyPinned = audioCache.pinnedBytes.value
		val adding = songs
			.filterNot {
				pinnedKeys.isPinned(CacheKeys.of(it.ref, it.effectiveQuality(quality)))
			}
			.sumOf { it.storedSizeAt(quality) }
		if (alreadyPinned + adding > cap) {
			return PinResult.TooLarge(alreadyPinned + adding, cap)
		}

		pinDao.add(PinEntity(refKey = ref.encode(), kind = kind.name))
		// Protection is applied here rather than waiting for the DAO's flow: a
		// download starting before its key is protected could be evicted by the
		// very bytes it is writing.
		applyProtection(_pins.value + Pin(ref, kind))
		enqueue(songs)
		return PinResult.Ok
	}

	suspend fun unpin(ref: ItemRef) {
		val pin = _pins.value.firstOrNull { it.ref == ref } ?: return
		pinDao.remove(ref.encode())

		val before = _protectedKeys.value
		applyProtection(_pins.value - pin)
		// Only what nothing else still covers: a track pinned on its own and
		// also part of a pinned album must survive the album being unpinned.
		(before - _protectedKeys.value).forEach { downloads.remove(it) }
	}

	/**
	 * Re-enqueues what a pin covers, for a download that failed.
	 *
	 * Already-stored tracks cost nothing to re-request: the download manager
	 * finds them complete in the cache and reports them done. Removal is not
	 * offered here — the pin list in Settings is where a download you have
	 * given up on gets deleted.
	 */
	suspend fun retry(ref: ItemRef) {
		val pin = _pins.value.firstOrNull { it.ref == ref } ?: return
		enqueue(songsFor(pin))
	}

	suspend fun refresh() = applyProtection(_pins.value)

	/**
	 * Pins with something readable attached, for the list in Settings.
	 *
	 * A pin whose item is no longer in the mirror still appears, named by its
	 * kind alone — it is the only place it can be removed from, so hiding it
	 * would strand it.
	 */
	suspend fun describe(pins: List<Pin>): List<PinnedItem> = pins.map { pin ->
		val label = when (pin.kind) {
			PinKind.SONG -> local.song(pin.ref)?.title
			PinKind.ALBUM -> local.album(pin.ref)?.let { "${it.artistName} — ${it.title}" }
			PinKind.PLAYLIST -> local.playlists(pin.ref.server)
				.firstOrNull { it.ref == pin.ref }?.name
		}
		PinnedItem(pin, label ?: "Unknown ${pin.kind.name.lowercase()}")
	}

	private suspend fun applyProtection(pins: List<Pin>) {
		val coverage = computeCoverage(pins)
		val keys = coverage.allKeys
		// Two forms of the same set, and they are no longer interchangeable.
		//
		// The evictor works in *cache* keys, which carry the quality, because
		// that is what the stored bytes are filed under. Everything above —
		// availability marks, pin coverage, the UI — works in bare refs, which
		// is what the library mirror and the DAO understand.
		//
		// Only the quality currently set is protected: a copy left behind by an
		// earlier setting stays playable but becomes ordinary evictable cache,
		// which is what makes a quality change reclaim its own space.
		val quality = settings.audioQuality.first()
		// A video's soundtrack is filed under the substituted quality (see
		// AudioQuality.forVideoAudio), so protecting it under the plain one
		// would name bytes that do not exist and leave the ones that do
		// evictable — a downloaded film that quietly stops playing offline.
		// One bulk query rather than a lookup per ref: a playlist pin can
		// cover hundreds.
		val videos = local.videoRefKeys(keys)
		// Set before publishing: the evictor reads this, and a download racing
		// ahead of it would be evictable for the length of the race.
		pinnedKeys.keys = keys.mapTo(mutableSetOf()) {
			CacheKeys.of(it, if (it in videos) quality.forVideoAudio() else quality)
		}
		_pins.value = pins
		_coverage.value = coverage
		_protectedKeys.value = keys
	}

	/**
	 * Re-fetches everything pinned, at whatever quality is now set.
	 *
	 * Protection is re-applied first so `PinnedKeys` holds the new keys before
	 * any download starts writing under them — the same ordering [pin] relies
	 * on, for the same reason.
	 */
	suspend fun refreshDownloads() {
		applyProtection(_pins.value)
		_pins.value.forEach { enqueue(songsFor(it)) }
	}

	private suspend fun computeCoverage(pins: List<Pin>): PinCoverage {
		// Read once for the whole pass rather than per pin: a flip halfway
		// through would otherwise produce a coverage that included the videos
		// under some pins and not others.
		val audioOnly = settings.videoAudioOnly.first()
		return retaining(
			expandPins(
				pins = pins,
				albumSongs = pins.filter { it.kind == PinKind.ALBUM }
					.associate {
						it.ref to local.songsOfAlbum(it.ref)
							.downloadable(audioOnly).map(Song::ref)
					},
				playlistSongs = pins.filter { it.kind == PinKind.PLAYLIST }
					.associate {
						it.ref to local.songsOfPlaylist(it.ref)
							.downloadable(audioOnly).map(Song::ref)
					},
			)
		)
	}

	/**
	 * Keeps the previous membership of any pin that has just come back empty.
	 *
	 * An empty expansion is a legitimate *state* — pinning an album that has
	 * never been opened protects nothing, which `expandPins` documents — but it
	 * is never a legitimate *transition* for a pin that already covered
	 * something. What produces one is the mirror being emptied underneath it:
	 * removing a server, or changing one's browse mode. Both write through
	 * `LocalLibrary`, which bumps the revision this class listens on, so the
	 * recompute lands while the tables are bare.
	 *
	 * Without this the coverage collapses, `PinnedKeys` shrinks, and the evictor
	 * is free to reclaim audio the user explicitly asked to keep — silently, and
	 * only until they next happen to open the album. The stale membership is the
	 * safer of the two wrong answers: it protects bytes that are already on
	 * disk, and the next successful browse replaces it.
	 */
	private fun retaining(fresh: PinCoverage): PinCoverage {
		val previous = _coverage.value.byPin
		if (previous.isEmpty()) return fresh
		return PinCoverage(
			fresh.byPin.mapValues { (pin, songs) ->
				songs.ifEmpty { previous[pin].orEmpty() }
			}
		)
	}

	private suspend fun songsFor(pin: Pin): List<Song> = when (pin.kind) {
		PinKind.SONG -> listOfNotNull(local.song(pin.ref))
		PinKind.ALBUM -> local.songsOfAlbum(pin.ref)
		PinKind.PLAYLIST -> local.songsOfPlaylist(pin.ref)
	}.downloadable(settings.videoAudioOnly.first())

	private suspend fun enqueue(songs: List<Song>) {
		songs.forEach { song ->
			// A video only ever reaches here through downloadable(), which lets
			// one past exactly when its soundtrack is what will be fetched — so
			// `isVideo` is the flag the URL builder wants, not a second reading
			// of the setting that could disagree with the first.
			// The same declaration playback makes, and it must be: both build
			// the same cache key, so a pinned copy and a played one that
			// declared differently would write different bytes under it.
			val target = streamUrls.forDownload(
				song.ref,
				song.isVideo,
				::playableAudioFor,
			) ?: return@forEach
			downloads.add(song.ref, target)
		}
	}

	/**
	 * The quality this song's bytes are actually filed under.
	 *
	 * A video's soundtrack is always a transcode — `AudioFormat.ORIGINAL` for
	 * one means "send the film", which is not a thing that can be stored — so
	 * the substitution has to be applied wherever a cache key or a size is
	 * derived, not only where the URL is built.
	 */
	private fun Song.effectiveQuality(quality: AudioQuality): AudioQuality =
		if (isVideo) quality.forVideoAudio() else quality

	/**
	 * Roughly what this song will occupy once stored.
	 *
	 * [Song.sizeBytes] is the size of the file on the server, which is the wrong
	 * number as soon as anything is transcoded — an album of FLACs would be
	 * refused for a pin that actually fits several times over at Opus. Bitrate
	 * times duration is close enough for a capacity check.
	 *
	 * A video is the extreme case of the same thing: its `sizeBytes` is the
	 * whole film, several gigabytes for something that stores as a hundred
	 * megabytes, which would refuse a pin that fits many times over.
	 */
	private fun Song.storedSizeAt(quality: AudioQuality): Long {
		val effective = effectiveQuality(quality)
		return if (effective.format == AudioFormat.ORIGINAL) sizeBytes
		else (duration * effective.bitRate * 125L)
	}

	private fun PinEntity.toPin(): Pin? {
		val ref = ItemRef.decode(refKey) ?: return null
		val pinKind = runCatching { PinKind.valueOf(kind) }.getOrNull() ?: return null
		return Pin(ref, pinKind)
	}
}

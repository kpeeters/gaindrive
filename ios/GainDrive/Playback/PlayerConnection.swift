//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import Foundation

enum TrackState: Sendable {
	case idle, loading, current
}

/// **The UI's only route to playback.**
///
/// On Android that boundary is the framework's — the UI holds a
/// `MediaController` and cannot reach the player. iOS has no such boundary, so
/// this type is it. It is also what makes a future cast player a drop-in, so it
/// is not optional scaffolding.
///
/// `@MainActor` is not a concession to SwiftUI: `AVQueuePlayer`,
/// `AVPlayerItem` and `AVURLAsset` are not `Sendable`, so this is the only
/// place they may legally live. Every AVFoundation call in the app happens
/// here.
///
/// **No `PlayerState` struct.** Android needs one because a `StateFlow` emits a
/// single value; `@Observable` tracks reads per property, so publishing the
/// fields directly means a `TrackRow` that reads `current` and `loadingRef` is
/// not invalidated twice a second by `position`. The corollary to hold to:
/// `trackState(of:)` must never read `position`, or every track list starts
/// re-rendering at 2 Hz and nothing will say why.
@MainActor
@Observable
final class PlayerConnection {
	private(set) var current: Song?
	private(set) var isPlaying = false
	private(set) var isBuffering = false
	/// Set the instant a tap is handled, before anything is resolved — that
	/// wait is precisely what the spinner exists to explain.
	private(set) var loadingRef: ItemRef?
	private(set) var position: Double = 0
	private(set) var duration: Double = 0
	private(set) var canSeek = false
	private(set) var errorMessage: String?
	private(set) var queue: [Song] = []
	private(set) var queueIndex = 0
	private(set) var autoFrom = 0

	var hasNext: Bool { model.hasNext }
	var hasPrevious: Bool { model.hasPrevious }
	var isActive: Bool { current != nil }

	// MARK: - Machinery

	@ObservationIgnored private let player = AVQueuePlayer()
	@ObservationIgnored private var model = PlayQueue()
	/// Mirrors `player.items()`. `AVPlayerItem` carries no user data, so the
	/// mapping back to a ref has to be kept alongside.
	@ObservationIgnored private var window: [(ref: ItemRef, item: AVPlayerItem)] = []

	@ObservationIgnored private let registry: ServerRegistry
	@ObservationIgnored private let settings: SettingsStore
	/// Shared with `LibraryRepository`: the ceiling a stream URL needs and the
	/// roles the chip row needs come from the same one `getUser` per server.
	@ObservationIgnored private let accounts: Accounts
	@ObservationIgnored private let session = AudioSessionController()
	@ObservationIgnored private let nowPlaying = NowPlayingCenter()
	// The three that attach to the **role** rather than to the player: phase 9
	// swaps the player for a cast one and none of them may notice. `PLAN.md`
	// states the rule; Android is where it was learned.
	@ObservationIgnored private let scrobbler: Scrobbler
	@ObservationIgnored private let watchdog = PlaybackWatchdog()
	@ObservationIgnored private let prewarmer = TranscodePrewarmer()

	@ObservationIgnored private var observations: [NSKeyValueObservation] = []
	@ObservationIgnored private var timeObserver: Any?
	/// Set while we are editing the player's item list. `removeAllItems()`
	/// makes `currentItem` transiently `nil`, which the observer would
	/// otherwise read as "ran off the end of the queue".
	@ObservationIgnored private var applyingEdit = false
	@ObservationIgnored private var loadingTimeout: Task<Void, Never>?

	private static let loadingTimeoutSeconds: Double = 30
	private static let restartThreshold: Double = 3

	/// Built once, by the composition root, and never released — which is why
	/// there is no `deinit` removing the periodic time observer. That token
	/// must be removed before the player is deallocated or the process traps,
	/// so if this type ever becomes something with a shorter life, that is the
	/// first thing to add.
	init(
		registry: ServerRegistry, settings: SettingsStore, library: LibraryRepository,
		accounts: Accounts
	) {
		self.registry = registry
		self.settings = settings
		self.accounts = accounts
		self.scrobbler = Scrobbler(library: library)
		// Observers and command handlers only. No session activation and no
		// fetching: this initialiser starts no work, for the same reason the
		// view models' do not.
		observe()
		wireCallbacks()
	}

	// MARK: - Commands

	/// `startIndex` is an index into `songs`. Callers rendering a grouped list
	/// must map back to the flat order first — see `AlbumDetailView`.
	func play(_ songs: [Song], startIndex: Int) {
		guard songs.indices.contains(startIndex) else { return }
		beginLoading(songs[startIndex].ref)
		model.play(songs, startIndex: startIndex)
		publishQueue()
		Task { await reconcile(thenPlay: true) }
	}

	func addToQueue(_ song: Song) {
		let wasEmpty = model.isEmpty
		model.addToQueue(song)
		publishQueue()
		Task { await reconcile(thenPlay: wasEmpty) }
	}

	func playNext(_ song: Song) {
		let wasEmpty = model.isEmpty
		model.playNext(song)
		publishQueue()
		Task { await reconcile(thenPlay: wasEmpty) }
	}

	func togglePlayPause() {
		isPlaying ? pause() : resume()
	}

	func next() {
		guard model.hasNext else { return }
		beginLoading(model.songs[model.index + 1].ref)
		model.advance()
		publishQueue()
		// When the window already holds it, this is the pre-buffered item and
		// the transition is instant. `reconcile` then refills the tail.
		if window.count > 1 {
			applyEdit {
				player.advanceToNextItem()
				window.removeFirst()
			}
			Task { await reconcile(thenPlay: true) }
		} else {
			Task { await reconcile(thenPlay: true, forceRebuild: true) }
		}
	}

	/// Restarts the current track when more than three seconds in, which is the
	/// platform convention and what every transport does.
	func previous() {
		guard position <= Self.restartThreshold, model.hasPrevious else {
			seek(to: 0)
			return
		}
		beginLoading(model.songs[model.index - 1].ref)
		model.goBack()
		publishQueue()
		Task { await reconcile(thenPlay: true, forceRebuild: true) }
	}

	func jump(to index: Int) {
		guard model.songs.indices.contains(index) else { return }
		beginLoading(model.songs[index].ref)
		model.jump(to: index)
		publishQueue()
		Task { await reconcile(thenPlay: true, forceRebuild: true) }
	}

	func remove(at index: Int) {
		let wasCurrent = index == model.index
		model.remove(at: index)
		publishQueue()
		Task { await reconcile(thenPlay: wasCurrent && isPlaying, forceRebuild: wasCurrent) }
	}

	func move(from source: Int, to destination: Int) {
		model.move(from: source, to: destination)
		publishQueue()
		Task { await reconcile(thenPlay: false) }
	}

	func seek(to seconds: Double) {
		// The completion is delivered on an unspecified queue — unlike the
		// periodic time observer, which documents `queue: .main` — so this hops
		// rather than assuming.
		player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600)) { [weak self] _ in
			Task { @MainActor in
				guard let self else { return }
				self.position = seconds
				self.nowPlaying.setPlayback(isPlaying: self.isPlaying, position: seconds)
			}
		}
	}

	func stop() {
		// Explicitly, because `publishTransport` is not on this path and the
		// timer would otherwise outlive the queue it was armed against.
		watchdog.disarm()
		applyEdit {
			player.pause()
			player.removeAllItems()
			window = []
		}
		model.clear()
		clearLoading()
		errorMessage = nil
		publishQueue()
		nowPlaying.clear()
		session.deactivate()
	}

	func clearError() {
		errorMessage = nil
	}

	/// Cover art for a queue entry. Exposed here so the player surfaces need no
	/// registry of their own — the connection already holds one, and handing
	/// them a second route to it would be a second place to get the per-server
	/// lookup wrong.
	func coverSource(for song: Song, size: Int) -> CoverSource? {
		registry.clientsSnapshot().coverUrls.source(song.coverArt, size: size)
	}

	/// What was, or would be, asked of that track's own server.
	///
	/// Exposed for the track-info view, which answers "why does this sound
	/// different here" and cannot answer it without the *capped* quality — the
	/// account ceiling belongs to that track's server and is not a setting
	/// anyone can read off the Settings screen.
	func streamQuality(for song: Song) async -> AudioQuality? {
		await streamTarget(for: song)?.quality
	}

	/// Reads `loadingRef`, `current` and `isBuffering` — and deliberately not
	/// `position`.
	func trackState(of ref: ItemRef) -> TrackState {
		if loadingRef == ref { return .loading }
		guard current?.ref == ref else { return .idle }
		return isBuffering ? .loading : .current
	}

	// MARK: - Transport

	private func resume() {
		guard !model.isEmpty else { return }
		do {
			try session.activate()
		} catch {
			// The one case where "I pressed play and nothing happened" has an
			// explanation the user can act on: another app holds a non-mixable
			// session, which in practice means a phone call.
			errorMessage = "Another app is using the audio right now."
			return
		}
		player.play()
	}

	private func pause() {
		player.pause()
	}

	// MARK: - Reconciliation

	/// Brings the player's items in line with the queue.
	///
	/// `forceRebuild` is for the moves the window cannot express — going
	/// backwards, jumping, or replacing the track that is playing.
	private func reconcile(thenPlay: Bool, forceRebuild: Bool = false) async {
		let desired = model.window
		let edit =
			forceRebuild && !desired.isEmpty
			? WindowEdit.rebuild(desired)
			: PlayerWindow.plan(current: window.map(\.ref), desired: desired)

		switch edit {
		case .none:
			break
		case .clear:
			applyEdit {
				player.removeAllItems()
				window = []
			}
		case .rebuild(let refs):
			guard let built = await build(refs) else { return }
			applyEdit {
				player.removeAllItems()
				for entry in built { player.insert(entry.item, after: nil) }
				window = built
			}
		case .replaceTail(let refs):
			guard let built = await build(refs) else { return }
			applyEdit {
				for stale in window.dropFirst() { player.remove(stale.item) }
				var kept = Array(window.prefix(1))
				for entry in built {
					player.insert(entry.item, after: player.items().last)
					kept.append(entry)
				}
				window = kept
			}
		}

		if thenPlay { resume() }
		publishTrack()
		publishTransport()
	}

	/// Always builds **fresh** `AVPlayerItem`s. A consumed item cannot be
	/// re-enqueued, and reusing one is the classic `AVQueuePlayer` bug that
	/// surfaces as silence with nothing in the log.
	private func build(_ refs: [ItemRef]) async -> [(ref: ItemRef, item: AVPlayerItem)]? {
		var built: [(ref: ItemRef, item: AVPlayerItem)] = []
		for ref in refs {
			guard let song = model.song(for: ref), let target = await streamTarget(for: song) else {
				// The head is what the user asked for; failing to resolve it is
				// an error worth showing. A tail that cannot be resolved just
				// means no pre-buffering.
				if built.isEmpty {
					errorMessage = "That track could not be played."
					clearLoading()
					return nil
				}
				break
			}
			built.append((ref, AVPlayerItem(url: target.url)))
		}
		return built
	}

	/// Resolved **per track**, from that track's own server.
	///
	/// Nothing here closes over a "current server", which is the only reason a
	/// queue spanning two servers works: it crosses credentials and bitrate
	/// caps at every boundary.
	private func streamTarget(for song: Song) async -> StreamTarget? {
		let clients = registry.clientsSnapshot()
		guard let client = clients.client(for: song.ref.server) else { return nil }
		let cap = await accounts.cap(for: song.ref.server, using: clients)
		return StreamUrls.target(
			for: song.ref, client: client, wanted: settings.audioQuality, accountCap: cap)
	}

	private func applyEdit(_ body: () -> Void) {
		applyingEdit = true
		body()
		applyingEdit = false
	}

	// MARK: - Observation

	/// **Every callback says only "republish".**
	///
	/// A KVO block fires synchronously on whatever thread mutated the property
	/// — not the main actor — and its payload is a non-`Sendable`
	/// `AVPlayerItem?`. Carrying nothing across and re-reading the state on the
	/// main actor is what lets this whole class compile under complete
	/// concurrency checking without a single `@unchecked`. It is also
	/// structurally what Android does with `onEvents → publish()`.
	private func observe() {
		observations.append(
			player.observe(\.currentItem, options: [.new]) { [weak self] _, _ in
				Task { @MainActor in self?.currentItemChanged() }
			})
		observations.append(
			player.observe(\.timeControlStatus, options: [.new]) { [weak self] _, _ in
				Task { @MainActor in self?.publishTransport() }
			})

		// The one place `assumeIsolated` is correct: `queue: .main` is
		// documented to deliver on the main queue, and hopping twice a second
		// for a value we already have would be waste.
		timeObserver = player.addPeriodicTimeObserver(
			forInterval: CMTime(seconds: 0.5, preferredTimescale: 600), queue: .main
		) { [weak self] time in
			MainActor.assumeIsolated {
				guard let self, !time.seconds.isNaN else { return }
				self.position = time.seconds
				// Here rather than on a timer of its own: this fires only while
				// the timeline is advancing, which is exactly when a play is
				// accruing.
				self.scrobbler.tick(position: time.seconds, duration: self.duration)
			}
		}
	}

	private func wireCallbacks() {
		session.onPause = { [weak self] in self?.pause() }
		session.onResume = { [weak self] in self?.resume() }
		session.onReset = { [weak self] in
			guard let self else { return }
			Task { await self.reconcile(thenPlay: false, forceRebuild: true) }
		}

		nowPlaying.onPlay = { [weak self] in self?.resume() }
		nowPlaying.onPause = { [weak self] in self?.pause() }
		nowPlaying.onTogglePlayPause = { [weak self] in self?.togglePlayPause() }
		nowPlaying.onNext = { [weak self] in self?.next() }
		nowPlaying.onPrevious = { [weak self] in self?.previous() }
		nowPlaying.onSeek = { [weak self] in self?.seek(to: $0) }

		watchdog.sample = { [weak self] in
			guard let self else { return PlaybackWatchdog.Sample(stalled: false, position: 0) }
			// `isBuffering` is already "waiting to play in order to minimise
			// stalls", which is buffering *and* wanting to play — the pair the
			// watchdog needs, and the same predicate Android spells as
			// `STATE_BUFFERING && playWhenReady`.
			return PlaybackWatchdog.Sample(stalled: self.isBuffering, position: self.position)
		}
		watchdog.onStall = { [weak self] in
			guard let self else { return }
			// **Pause, not `stop()`.** Android stops because a wedged player
			// still counted as wanting to play, so its foreground service
			// survived a swipe away and re-opening re-bound to the same wedge.
			// There is no such service here, and `stop()` would additionally
			// throw the queue away. Pausing keeps it, and pressing play is the
			// retry.
			self.pause()
			self.errorMessage = "Playback stalled and was paused."
		}
	}

	private func currentItemChanged() {
		guard !applyingEdit else { return }

		guard let item = player.currentItem else {
			// Ran off the end. The queue is kept and the index parked on the
			// last track, so the mini player does not vanish and the track can
			// be replayed.
			model.parkAtEnd()
			publishQueue()
			publishTransport()
			return
		}

		if let advanced = window.firstIndex(where: { $0.item === item }), advanced > 0 {
			model.advance(by: advanced)
			window.removeFirst(advanced)
			publishQueue()
			// Refill the tail so the *next* transition is pre-buffered too.
			Task { await reconcile(thenPlay: false) }
		}
		publishTrack()
	}

	// MARK: - Publishing

	private func publishQueue() {
		queue = model.songs
		queueIndex = model.index
		autoFrom = model.autoFrom.value
		publishTrack()
	}

	private func publishTrack() {
		let song = model.current
		let changed = song?.ref != current?.ref
		current = song
		duration = Double(song?.duration ?? 0)

		// The one place in the class that means "a different track is current
		// now", which is what both of these hang off.
		if changed {
			scrobbler.trackChanged(to: song?.ref)
			prewarmNext()
		}

		guard let song else {
			nowPlaying.clear()
			return
		}
		if changed {
			let covers = registry.clientsSnapshot().coverUrls
			nowPlaying.setTrack(
				song, index: model.index, count: model.songs.count,
				cover: covers.source(song.coverArt, size: CoverSize.hero))
		}
		nowPlaying.setAvailability(hasNext: model.hasNext, canSeek: canSeek)
	}

	private func publishTransport() {
		isPlaying = player.timeControlStatus == .playing
		// The precise equivalent of Android's `STATE_BUFFERING && playWhenReady`.
		// `isPlaybackLikelyToKeepUp` is the wrong signal: it answers a different
		// question and is false in states that are not stalls.
		isBuffering =
			player.timeControlStatus == .waitingToPlayAtSpecifiedRate
			&& player.reasonForWaitingToPlay == .toMinimizeStalls

		// Seeking a chunked, length-less response silently does nothing, so the
		// scrubber and the lock-screen command are driven from what the item
		// actually offers.
		canSeek = !(player.currentItem?.seekableTimeRanges.isEmpty ?? true)

		if let item = player.currentItem, item.status == .failed {
			errorMessage = item.error?.userMessage ?? "That track could not be played."
			clearLoading()
		}
		if loadingRef != nil, current?.ref == loadingRef, isPlaying {
			clearLoading()
		}
		nowPlaying.setPlayback(isPlaying: isPlaying, position: position)
		nowPlaying.setAvailability(hasNext: model.hasNext, canSeek: canSeek)
		// **The watchdog is armed from here and nowhere else.** A poll over
		// `position` would compile and never fire: the periodic time observer
		// runs as the timeline advances, so during a stall — the one case that
		// matters — there is no tick to poll on. This is driven by the
		// `timeControlStatus` KVO instead.
		watchdog.update()
	}

	/// Asks the server to build the **next** track's transcode while this one
	/// plays, so the wait for it lands somewhere nobody is looking.
	///
	/// The first transition fires when playback begins, so the second track of
	/// a queue is prepared while the first plays — the case that matters.
	///
	/// A video reaching here is warmed too, and today that is right:
	/// `StreamUrls.target` sends an audio `format` for every track, which is
	/// the server's audio-only switch, so what is warmed is the soundtrack
	/// extraction — a blocking transcode of a multi-gigabyte source, and the
	/// case Android says needs this most. Phase 6 gives video its own URL, and
	/// the guard goes in with it.
	private func prewarmNext() {
		guard model.hasNext else { return }
		let next = model.songs[model.index + 1]
		Task { [weak self] in
			guard let self, let target = await self.streamTarget(for: next) else { return }
			await self.prewarmer.warm(target)
		}
	}

	// MARK: - Loading

	private func beginLoading(_ ref: ItemRef) {
		errorMessage = nil
		loadingRef = ref
		loadingTimeout?.cancel()
		// A load that neither succeeds nor reports an error must not leave a
		// row spinning for ever.
		loadingTimeout = Task { [weak self] in
			try? await Task.sleep(for: .seconds(Self.loadingTimeoutSeconds))
			guard !Task.isCancelled else { return }
			self?.clearLoading()
		}
	}

	private func clearLoading() {
		loadingTimeout?.cancel()
		loadingTimeout = nil
		loadingRef = nil
	}
}

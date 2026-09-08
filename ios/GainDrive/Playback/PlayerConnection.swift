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
/// this type is it.
///
/// **It owns the queue; an engine holds a window onto it.** `PlaybackEngine` is
/// the seam a cast player drops into, and this class is deliberately the half
/// that has nothing to do with how the sound is made: the queue, what every
/// command means, every published property, and the four collaborators that
/// attach to the **role** rather than to the player — `Scrobbler`,
/// `PlaybackWatchdog`, `TranscodePrewarmer` and `NowPlayingCenter`. Android
/// hung two of those off its `ExoPlayer` and had to repair it when the session
/// swapped; here they never touch an engine.
///
/// `@MainActor` because `LocalEngine` is: `AVQueuePlayer`, `AVPlayerItem` and
/// `AVURLAsset` are not `Sendable`.
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
	/// Whether the picture is on screen.
	///
	/// **Owned here because this is where a video becomes current**, whether by
	/// a tap or by the queue advancing — which is what "entered from one place
	/// in the shell" means. Leaving does not stop the film: a concert is
	/// listened to as often as it is watched, so this goes false and playback
	/// carries on.
	var showingVideo = false

	var hasNext: Bool { model.hasNext }
	var hasPrevious: Bool { model.hasPrevious }
	var isActive: Bool { current != nil }

	// MARK: - Machinery

	@ObservationIgnored private var model = PlayQueue()
	/// Whatever is making the sound. One implementation today; the whole reason
	/// the protocol exists is that there will be a second.
	@ObservationIgnored private var engine: any PlaybackEngine
	/// The same object, held concretely for the one thing that is genuinely
	/// local: the video surface, which attaches to an `AVPlayer`. Nothing else
	/// may reach through it.
	@ObservationIgnored private let local: LocalEngine

	/// Only for cover art: the stream URL belongs to the engine, which knows
	/// what kind of URL it can consume.
	@ObservationIgnored private let registry: ServerRegistry
	@ObservationIgnored private let nowPlaying = NowPlayingCenter()
	// The three that attach to the **role** rather than to the player: swapping
	// in a cast engine must not silence any of them. `PLAN.md` states the rule;
	// Android is where it was learned, by breaking it.
	@ObservationIgnored private let scrobbler: Scrobbler
	@ObservationIgnored private let watchdog = PlaybackWatchdog()
	@ObservationIgnored private let prewarmer = TranscodePrewarmer()

	@ObservationIgnored private var loadingTimeout: Task<Void, Never>?

	private static let loadingTimeoutSeconds: Double = 30
	private static let restartThreshold: Double = 3

	init(
		registry: ServerRegistry, library: LibraryRepository,
		targets: StreamTargets, store: AudioStore
	) {
		self.registry = registry
		self.scrobbler = Scrobbler(library: library)
		let local = LocalEngine(targets: targets, store: store)
		self.local = local
		self.engine = local
		// Observers and command handlers only. No session activation and no
		// fetching: this initialiser starts no work, for the same reason the
		// view models' do not.
		wireCallbacks()
		adopt(local)
	}

	/// Makes `next` the engine, and the only place that ever does.
	///
	/// **The old engine is unwired first.** Its callbacks are closures over this
	/// object, so an engine left connected goes on advancing a queue it is no
	/// longer playing — which is the shape of every "two players at once" bug.
	///
	/// Called once today, from `init`. When there is a second engine it is also
	/// where the handover happens, and two things will have to happen with it:
	/// the outgoing engine has to be stopped, and the queue re-applied to the
	/// incoming one at the position the outgoing one had reached. The queue
	/// itself needs no carrying — it never belonged to either.
	private func adopt(_ next: any PlaybackEngine) {
		engine.onStateChange = nil
		engine.onProgress = nil
		engine.onAdvanced = nil
		engine.onEnded = nil
		engine.onReset = nil
		engine = next
		wireEngine()
	}

	// MARK: - Commands

	/// `startIndex` is an index into `songs`. Callers rendering a grouped list
	/// must map back to the flat order first — see `AlbumDetailView`.
	///
	/// `startPosition` is what a chapter marker asks for: play this recording,
	/// but from the song inside it that was tapped. Defaulted, so every caller
	/// meaning "from the beginning" is unchanged. How it is honoured is the
	/// engine's business — locally it waits for the item to become seekable,
	/// on a receiver it rides in the LOAD.
	func play(_ songs: [Song], startIndex: Int, startPosition: Double = 0) {
		guard songs.indices.contains(startIndex) else { return }
		beginLoading(songs[startIndex].ref)
		model.play(songs, startIndex: startIndex)
		publishQueue()
		Task { await reconcile(thenPlay: true, offset: startPosition) }
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
		// When the engine already holds it, this is the pre-buffered item and
		// the transition is instant; `reconcile` then refills the tail. An
		// engine with a one-entry window always says no, and gets a rebuild.
		if engine.advanceToNext() {
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
		engine.seek(to: seconds)
	}

	func stop() {
		// Explicitly, because `publishTransport` is not on this path and the
		// timer would otherwise outlive the queue it was armed against.
		watchdog.disarm()
		engine.stop()
		model.clear()
		clearLoading()
		errorMessage = nil
		publishQueue()
		nowPlaying.clear()
	}

	func clearError() {
		errorMessage = nil
	}

	/// **The video surface attaches to this directly**, which is a deliberate
	/// hole in "the UI's only route to playback" and the same one Android
	/// punched: its `VideoSurface` attaches to the `ExoPlayer` rather than
	/// negotiating `COMMAND_SET_VIDEO_SURFACE` through the session. Nothing
	/// else may reach for it — the queue, the transport and the seek all go
	/// through this class as before.
	///
	/// It names the **local** engine rather than whatever is playing, and that
	/// is right rather than a shortcut: a picture on this screen is by
	/// definition local playback.
	var videoPlayer: AVPlayer { local.videoPlayer }

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
	/// anyone can read off the Settings screen. It asks the **current** engine,
	/// so once there is a second one this answers about the route in use.
	func streamQuality(for song: Song) async -> AudioQuality? {
		await engine.target(for: song)?.quality
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
		engine.resume()
	}

	private func pause() {
		engine.pause()
	}

	// MARK: - Reconciliation

	/// Brings the engine's window in line with the queue.
	///
	/// `forceRebuild` is for the moves the window cannot express — going
	/// backwards, jumping, or replacing the track that is playing.
	///
	/// **The window size is the engine's**, which is the whole of what differs
	/// between playing here and playing on a receiver: two entries locally so
	/// the next is pre-buffered, one on a receiver, which is told about a single
	/// track at a time.
	private func reconcile(
		thenPlay: Bool, forceRebuild: Bool = false, offset: Double = 0
	) async {
		let desired = model.window(size: engine.windowSize)
		let refEdit =
			forceRebuild && !desired.isEmpty
			? WindowEdit.rebuild(desired)
			: PlayerWindow.plan(current: engine.loaded, desired: desired)

		guard await engine.apply(resolve(refEdit), startingAt: offset > 0 ? offset : nil) else {
			// Only the head failing gets here, and it is the one worth a
			// message: the user asked for that track.
			errorMessage = "That track could not be played."
			clearLoading()
			return
		}

		if thenPlay { resume() }
		publishTrack()
		publishTransport()
	}

	private func resolve(_ edit: WindowEdit) -> EngineEdit {
		switch edit {
		case .none: return .none
		case .clear: return .clear
		case .rebuild(let refs): return .rebuild(refs.compactMap(model.song(for:)))
		case .replaceTail(let refs): return .replaceTail(refs.compactMap(model.song(for:)))
		}
	}

	// MARK: - The engine

	/// **Every callback says only "something changed".** No engine pushes a
	/// value, so there is one copy of the publishing logic whichever is playing
	/// — which is the rule that stopped this class needing `@unchecked` anywhere
	/// when the callbacks were KVO blocks, and the reason it will not need a
	/// second publish path when the callbacks are cast statuses.
	private func wireEngine() {
		engine.onStateChange = { [weak self] in self?.publishTransport() }
		engine.onProgress = { [weak self] seconds in
			guard let self else { return }
			self.position = seconds
			// Here rather than on a timer of its own: this fires only while the
			// timeline is advancing, which is exactly when a play is accruing.
			self.scrobbler.tick(position: seconds, duration: self.duration)
		}
		engine.onAdvanced = { [weak self] steps in
			guard let self else { return }
			self.model.advance(by: steps)
			self.publishQueue()
			// Refill the tail so the *next* transition is pre-buffered too.
			Task { await self.reconcile(thenPlay: false) }
		}
		engine.onEnded = { [weak self] in
			guard let self else { return }
			// Ran off the end. The queue is kept and the index parked on the
			// last track, so the mini player does not vanish and the track can
			// be replayed.
			self.model.parkAtEnd()
			self.publishQueue()
			self.publishTransport()
		}
		engine.onReset = { [weak self] in
			guard let self else { return }
			Task { await self.reconcile(thenPlay: false, forceRebuild: true) }
		}
	}

	private func wireCallbacks() {
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
			// The one place a video becomes current, however it got there — a
			// tap, or the queue reaching it.
			if song?.isVideo == true { showingVideo = true }
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
		isPlaying = engine.isPlaying
		isBuffering = engine.isBuffering
		canSeek = engine.canSeek
		position = engine.position

		// The engine states a failure once; the connection owns the words the
		// UI shows and takes it from there.
		if let failure = engine.failure {
			errorMessage = failure
			engine.clearFailure()
			clearLoading()
		}
		if loadingRef != nil, current?.ref == loadingRef, isPlaying {
			clearLoading()
		}
		nowPlaying.setPlayback(isPlaying: isPlaying, position: position)
		nowPlaying.setAvailability(hasNext: model.hasNext, canSeek: canSeek)
		// **The watchdog is armed from here and nowhere else.** A poll over
		// `position` would compile and never fire: progress is reported as the
		// timeline advances, so during a stall — the one case that matters —
		// there is no tick to poll on. This is driven by the engine's transport
		// notifications instead.
		watchdog.update()
	}

	/// Asks the server to build the **next** track's transcode while this one
	/// plays, so the wait for it lands somewhere nobody is looking.
	///
	/// The first transition fires when playback begins, so the second track of
	/// a queue is prepared while the first plays — the case that matters.
	private func prewarmNext() {
		guard model.hasNext else { return }
		let next = model.songs[model.index + 1]
		// **Not a video.** The comment below used to say a video was warmed
		// deliberately, and it was right while `StreamUrls` sent an audio
		// `format` for everything and the URL was therefore a soundtrack. Now
		// it is a film's own URL, and warming it would ask the server to begin
		// a re-encode nobody has asked to watch.
		guard !next.isVideo else { return }
		Task { [weak self] in
			guard let self, let target = await self.engine.target(for: next) else { return }
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

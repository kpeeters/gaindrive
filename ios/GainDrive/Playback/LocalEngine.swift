//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import Foundation

/// Playing through this device's own speakers.
///
/// **Every AVFoundation call in the app happens here**, which used to be true of
/// `PlayerConnection` and is the one sentence that had to keep being true when
/// the seam was cut. `AVQueuePlayer`, `AVPlayerItem` and `AVURLAsset` are not
/// `Sendable`, so `@MainActor` is not a concession to SwiftUI: it is the only
/// place they may legally live.
///
/// This is a **move**, not a rewrite. Everything here was `PlayerConnection`'s
/// and behaves as it did; what changed is who owns it. Read the protocol for why
/// the boundary is where it is.
@MainActor
final class LocalEngine: PlaybackEngine {
	/// Two: the current track and the next, because the second is what
	/// `AVQueuePlayer` pre-buffers and that is the whole of the gapless story
	/// available on this platform.
	let windowSize = 2

	private(set) var isPlaying = false
	private(set) var isBuffering = false
	private(set) var position: Double = 0
	private(set) var canSeek = false
	private(set) var failure: String?

	var onStateChange: (() -> Void)?
	var onProgress: ((Double) -> Void)?
	var onAdvanced: ((Int) -> Void)?
	var onEnded: (() -> Void)?
	var onReset: (() -> Void)?

	// MARK: - Machinery

	private let player = AVQueuePlayer()
	/// Mirrors `player.items()`. `AVPlayerItem` carries no user data, so the
	/// mapping back to a ref has to be kept alongside.
	///
	/// **The loader is held here and nowhere else.** `AVURLAsset` keeps its
	/// resource-loader delegate weakly, so nothing but this keeps it alive —
	/// and dropping a window entry is what cancels the fetch behind a track that
	/// has been skipped past.
	private var window: [(ref: ItemRef, item: AVPlayerItem, loader: CachingResourceLoader?)] = []

	var loaded: [ItemRef] { window.map(\.ref) }

	private let targets: StreamTargets
	private let store: AudioStore
	private let session = AudioSessionController()

	private var observations: [NSKeyValueObservation] = []
	private var timeObserver: Any?
	/// Set while we are editing the player's item list. `removeAllItems()` makes
	/// `currentItem` transiently `nil`, which the observer would otherwise read
	/// as "ran off the end of the queue".
	private var applyingEdit = false
	/// A start offset waiting for the item to become seekable.
	///
	/// Cleared by anything that changes what is playing, because an offset into
	/// one recording means nothing in the next: a queue advance while the first
	/// item was still loading would otherwise drop the second track a quarter of
	/// an hour in.
	private var pendingSeek: Double?

	/// Built once and never released — which is why there is no `deinit`
	/// removing the periodic time observer. That token must be removed before
	/// the player is deallocated or the process traps, so if this type ever
	/// becomes something with a shorter life, that is the first thing to add.
	init(targets: StreamTargets, store: AudioStore) {
		self.targets = targets
		self.store = store
		observe()
		wireSession()
	}

	/// **The video surface attaches to this directly**, which is a deliberate
	/// hole in "the UI's only route to playback" and the same one Android
	/// punched: its `VideoSurface` attaches to the `ExoPlayer` rather than
	/// negotiating `COMMAND_SET_VIDEO_SURFACE` through the session. Nothing else
	/// may reach for it — the queue, the transport and the seek all go through
	/// `PlayerConnection` as before.
	var videoPlayer: AVPlayer { player }

	func clearFailure() {
		failure = nil
	}

	// MARK: - Loading

	func apply(_ edit: EngineEdit, startingAt offset: Double?) async -> Bool {
		// Armed before the items are built, because a stored file can be ready
		// before this returns — and cleared by any edit that names no offset,
		// which is what stops one outliving the track it was meant for.
		pendingSeek = (offset ?? 0) > 0 ? offset : nil

		switch edit {
		case .none:
			break
		case .clear:
			applyEdit {
				player.removeAllItems()
				window = []
			}
		case .rebuild(let songs):
			guard let built = await build(songs) else { return false }
			applyEdit {
				player.removeAllItems()
				for entry in built { player.insert(entry.item, after: nil) }
				window = built
			}
		case .replaceTail(let songs):
			guard let built = await build(songs) else { return false }
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
		// **The already-ready case.** An offset asked for on a window that did
		// not change — tapping a chapter of the track already playing — produces
		// a `.none` edit and no new item, so no status change ever fires and the
		// deferred seek would wait for ever. Applying it here covers that; when
		// the item is not ready yet this does nothing and the status observer
		// picks it up.
		applyPendingSeek()
		return true
	}

	/// Always builds **fresh** `AVPlayerItem`s. A consumed item cannot be
	/// re-enqueued, and reusing one is the classic `AVQueuePlayer` bug that
	/// surfaces as silence with nothing in the log.
	private func build(_ songs: [Song]) async
		-> [(ref: ItemRef, item: AVPlayerItem, loader: CachingResourceLoader?)]?
	{
		var built: [(ref: ItemRef, item: AVPlayerItem, loader: CachingResourceLoader?)] = []
		for song in songs {
			guard let target = await target(for: song) else {
				// The head is what the user asked for; failing to resolve it is
				// an error worth showing. A tail that cannot be resolved just
				// means no pre-buffering.
				if built.isEmpty { return nil }
				break
			}
			built.append(item(for: song, target: target))
		}
		return built
	}

	/// A stored track plays from disk. Anything else plays **through the caching
	/// loader**, which fetches it once at network speed and keeps the copy — so
	/// hearing a track is what puts it there.
	///
	/// The rewritten scheme is not decoration: AVFoundation handles `http` and
	/// `https` itself and consults a delegate only for a scheme it does not
	/// know, so without it the loader is never called and nothing is cached.
	private func item(for song: Song, target: StreamTarget)
		-> (ref: ItemRef, item: AVPlayerItem, loader: CachingResourceLoader?)
	{
		guard !target.url.isFileURL else {
			return (song.ref, AVPlayerItem(url: target.url), nil)
		}
		// **Video is never cached**, and the test is the song rather than the
		// URL. One film evicts the whole stored library, and a re-encoded one
		// arrives with no `Content-Length` so completeness could never be
		// established — the rule has held since downloads landed, and routing a
		// film through the loader would break it silently.
		//
		// An HLS playlist is the second reason: the loader fetches one resource
		// in order, and a playlist is a list of others.
		guard !song.isVideo else {
			return (song.ref, AVPlayerItem(url: target.url), nil)
		}
		let loader = CachingResourceLoader(
			source: target.url, ref: song.ref, quality: target.quality, store: store)
		let asset = AVURLAsset(url: CachingResourceLoader.rewrite(target.url))
		asset.resourceLoader.setDelegate(loader, queue: loader.queue)
		return (song.ref, AVPlayerItem(asset: asset), loader)
	}

	/// Resolved **per track**, from that track's own server, and served from
	/// disk when a copy is there.
	///
	/// Nothing here closes over a "current server", which is the only reason a
	/// queue spanning two servers works: it crosses credentials and bitrate caps
	/// at every boundary. `StreamTargets` is shared with `PinRepository` so a
	/// download and a play cannot ask for different bytes.
	///
	/// **The stored copy wins whatever quality it is.** A track pinned at one
	/// setting must not stop being playable because the setting changed later;
	/// the quality is in the key so two copies can coexist without either being
	/// mislabelled, but the music is the same music.
	func target(for song: Song) async -> StreamTarget? {
		// A film asks a different question: no format, no ceiling, and a
		// transport chosen by `nativeSeek`. See `StreamUrls.video`.
		if song.isVideo { return targets.video(for: song) }
		guard var target = await targets.target(for: song.ref) else { return nil }
		if let local = await store.storedFile(for: song.ref) {
			target = StreamTarget(
				url: local, quality: target.quality, cacheKey: target.cacheKey,
				contentType: target.contentType)
		}
		return target
	}

	private func applyEdit(_ body: () -> Void) {
		applyingEdit = true
		body()
		applyingEdit = false
	}

	// MARK: - Transport

	/// True when the next item was already loaded, which is the pre-buffered
	/// transition and is instant.
	func advanceToNext() -> Bool {
		guard window.count > 1 else { return false }
		applyEdit {
			player.advanceToNextItem()
			window.removeFirst()
		}
		return true
	}

	func resume() {
		do {
			try session.activate()
		} catch {
			// The one case where "I pressed play and nothing happened" has an
			// explanation the user can act on: another app holds a non-mixable
			// session, which in practice means a phone call.
			failure = "Another app is using the audio right now."
			onStateChange?()
			return
		}
		player.play()
	}

	func pause() {
		player.pause()
	}

	func seek(to seconds: Double) {
		// The completion is delivered on an unspecified queue — unlike the
		// periodic time observer, which documents `queue: .main` — so this hops
		// rather than assuming.
		player.seek(to: CMTime(seconds: seconds, preferredTimescale: 600)) { [weak self] _ in
			Task { @MainActor in
				guard let self else { return }
				self.position = seconds
				self.onStateChange?()
			}
		}
	}

	func stop() {
		pendingSeek = nil
		applyEdit {
			player.pause()
			player.removeAllItems()
			window = []
		}
		failure = nil
		session.deactivate()
	}

	/// Applied on the item's own `readyToPlay`, which is the first moment a seek
	/// is honoured rather than dropped. Both entry points call it — the status
	/// observer and `currentItemChanged` — since an item that was already ready
	/// when it became current publishes no new status.
	private func applyPendingSeek() {
		guard let target = pendingSeek else { return }
		guard let item = player.currentItem, item.status == .readyToPlay else { return }
		pendingSeek = nil
		seek(to: target)
	}

	// MARK: - Observation

	/// **Every callback says only "republish".**
	///
	/// A KVO block fires synchronously on whatever thread mutated the property —
	/// not the main actor — and its payload is a non-`Sendable` `AVPlayerItem?`.
	/// Carrying nothing across and re-reading the state on the main actor is
	/// what lets this whole class compile under complete concurrency checking
	/// without a single `@unchecked`. It is also structurally what Android does
	/// with `onEvents → publish()`.
	private func observe() {
		observations.append(
			player.observe(\.currentItem, options: [.new]) { [weak self] _, _ in
				Task { @MainActor in self?.currentItemChanged() }
			})
		observations.append(
			player.observe(\.timeControlStatus, options: [.new]) { [weak self] _, _ in
				Task { @MainActor in self?.republish() }
			})
		// **The item's own status, which nothing else reports.** An item that
		// cannot be played does not necessarily move `timeControlStatus`: a
		// player told to play something it cannot type sits in
		// `waitingToPlayAtSpecifiedRate` indefinitely, so without this the
		// failure below is only ever noticed if some unrelated event happens to
		// republish. The symptom is a spinner that never stops and no error
		// anywhere — which is exactly what a file stored with no extension
		// produced.
		observations.append(
			player.observe(\.currentItem?.status, options: [.new]) { [weak self] _, _ in
				Task { @MainActor in self?.republish() }
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
				self.onProgress?(time.seconds)
			}
		}
	}

	/// Interruptions and route changes are a fact about **this device's** audio
	/// output, so they belong to this engine and not to the connection. A
	/// receiver has its own idea of both and tells us over the control channel.
	private func wireSession() {
		session.onPause = { [weak self] in self?.pause() }
		session.onResume = { [weak self] in self?.resume() }
		// A reset invalidates every item, so the window has to be built again
		// from scratch. The connection owns the queue, so it is the one that can
		// do that.
		session.onReset = { [weak self] in self?.onReset?() }
	}

	private func currentItemChanged() {
		guard !applyingEdit else { return }
		applyPendingSeek()

		guard let item = player.currentItem else {
			onEnded?()
			return
		}

		if let advanced = window.firstIndex(where: { $0.item === item }), advanced > 0 {
			// The queue moved on while an offset was still waiting: it named a
			// position inside the track that has just ended, and applying it to
			// the next one would start it somewhere arbitrary.
			pendingSeek = nil
			window.removeFirst(advanced)
			onAdvanced?(advanced)
		}
		republish()
	}

	private func republish() {
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

		applyPendingSeek()
		if let item = player.currentItem, item.status == .failed {
			failure = item.error?.userMessage ?? "That track could not be played."
		}
		onStateChange?()
	}
}

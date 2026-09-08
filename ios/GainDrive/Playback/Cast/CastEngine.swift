//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Playing on a Cast receiver.
///
/// **The app still owns the queue; the receiver is told about one track.** That
/// is what `windowSize == 1` says, and it is the decision everything else here
/// follows from: when the receiver reports `IDLE`/`FINISHED`, the connection is
/// told to advance and hands down the next one. It costs a gap between tracks
/// and buys a queue that is the same object whether playback is here or over
/// there — which is what makes swapping engines at any moment safe.
///
/// It never touches `AVFoundation`, and `LocalEngine` never touches the network
/// beyond a URL. Neither knows the other exists.
@MainActor
final class CastEngine: PlaybackEngine {
	/// One. See the class note.
	let windowSize = 1

	private(set) var isPlaying = false
	private(set) var isBuffering = false
	private(set) var position: Double = 0
	/// **Always true.** A receiver seeks using the file's own index, and every
	/// URL sent to one is a real file with a `Content-Length` — the re-encode
	/// tier, which is the only thing that would not be, is refused rather than
	/// sent. So unlike the local engine there is nothing to discover here.
	let canSeek = true
	private(set) var failure: String?

	var onStateChange: (() -> Void)?
	var onProgress: ((Double) -> Void)?
	var onAdvanced: ((Int) -> Void)?
	var onEnded: (() -> Void)?
	var onReset: (() -> Void)?

	private(set) var loaded: [ItemRef] = []

	private let session: CastSession
	private let urls: CastUrls
	/// **Warmed before the LOAD, not after it**, and awaited.
	///
	/// The server's transcode cache is blocking: it runs ffmpeg over the whole
	/// track and answers nothing at all until the file is complete. Point a
	/// receiver at a cold one and it sits with no data for as long as that
	/// takes, which trips its ~60 s no-data timeout and surfaces as a session
	/// that says it is loading and never plays — the universal end-state for
	/// every cast failure in this codebase, arriving long after its cause.
	///
	/// `src/gaindrive.cc`'s own cast path does exactly this for exactly this
	/// reason (see "Casting a video to a receiver that cannot show one" in the
	/// root `CLAUDE.md`), and `android/CAST.md` lists its absence as an unbuilt
	/// remedy. Waiting here costs nothing that was not going to be waited for
	/// anyway; it only moves the wait to a place where the receiver is not
	/// counting.
	private let prewarmer = TranscodePrewarmer()
	/// The song the receiver was last told to play, so a status push can be
	/// matched to something.
	private var playing: Song?
	/// **The media session we have already acted on the end of.** A receiver
	/// repeats its idle pushes, and without this each repeat would walk the
	/// queue forward another track — an album skipping to its end in a second,
	/// which is what this guard is worth.
	private var advancedFrom = 0
	/// Extrapolates between the receiver's roughly-1 Hz reports, so a seek bar
	/// moves smoothly and is corrected by the next push rather than drifting.
	/// The right way round for a clock we do not own.
	private var reportedAt: ContinuousClock.Instant?
	private var reported: Double = 0
	private var ticker: Task<Void, Never>?

	init(session: CastSession, urls: CastUrls) {
		self.session = session
		self.urls = urls
		session.onStatus = { [weak self] status in self?.received(status) }
	}

	func clearFailure() {
		failure = nil
		session.clearFailure()
	}

	// MARK: - Loading

	func apply(_ edit: EngineEdit, startingAt offset: Double?) async -> Bool {
		switch edit {
		case .none:
			// A window of one that has not changed still has to honour an
			// offset — tapping a chapter of the track already playing is
			// exactly that, and here it is a seek rather than a reload.
			if let offset, offset > 0 { seek(to: offset) }
			return true
		case .clear:
			loaded = []
			playing = nil
			session.stopPlayback()
			return true
		case .rebuild(let songs), .replaceTail(let songs):
			// `replaceTail` cannot arrive with a window of one — the tail of a
			// single entry is empty, so `PlayerWindow.plan` answers `.none` or
			// `.rebuild`. Handled together rather than with an unreachable
			// branch that would rot.
			guard let song = songs.first else {
				loaded = []
				playing = nil
				session.stopPlayback()
				return true
			}
			return await load(song, at: offset ?? 0)
		}
	}

	private func load(_ song: Song, at offset: Double) async -> Bool {
		// **A film the server can only re-encode is refused rather than sent.**
		// That tier is a chunked response with no length and no index, which a
		// receiver cannot seek and frequently cannot start; `nativeSeek` is the
		// server's own word for "this arrives as a real MP4". Saying so is far
		// better than a LOAD that fails a minute in.
		if song.isVideo, !song.nativeSeek {
			failure = "This video has to be converted as it plays, which a Cast device cannot do."
			onStateChange?()
			return false
		}
		guard let target = song.isVideo ? urls.video(for: song) : await urls.audio(for: song)
		else {
			return false
		}
		loaded = [song.ref]
		playing = song
		advancedFrom = 0
		reported = offset
		reportedAt = nil
		position = offset
		// Reported as buffering while this runs, which is what it is. A film is
		// not warmed: the tiers a receiver will take are served off disk or
		// remuxed, and a remux of a multi-gigabyte file is minutes in which
		// nothing could be shown anyway.
		if !song.isVideo {
			// Said explicitly rather than left to the next status push: nothing
			// arrives from the receiver during the warm, so a client would
			// otherwise show whatever it was showing before — which after a
			// track change is the previous track, playing.
			isBuffering = true
			isPlaying = false
			onStateChange?()
			await prewarmer.warm(target)
		}
		session.load(
			CastMedia(
				url: target.url,
				contentType: target.contentType,
				duration: song.duration > 0 ? Double(song.duration) : nil,
				startAt: offset,
				title: song.title,
				artist: song.artistName.isEmpty ? nil : song.artistName,
				album: song.albumTitle.isEmpty ? nil : song.albumTitle,
				artwork: urls.artwork(for: song),
				isVideo: song.isVideo,
				quality: song.isVideo ? nil : target.quality))
		startTicking()
		return true
	}

	/// Always false: with a window of one there is never a loaded next item to
	/// step to, so the connection rebuilds — which for this engine is the only
	/// way forward anyway.
	func advanceToNext() -> Bool { false }

	// MARK: - Transport

	func resume() {
		session.play()
	}

	func pause() {
		session.pause()
	}

	func seek(to seconds: Double) {
		reported = seconds
		reportedAt = ContinuousClock.now
		position = seconds
		session.seek(to: seconds)
		onStateChange?()
	}

	func stop() {
		ticker?.cancel()
		ticker = nil
		loaded = []
		playing = nil
		position = 0
		isPlaying = false
		isBuffering = false
		session.stopPlayback()
	}

	// MARK: - URLs

	/// What a **receiver** would be asked to fetch, which is not what this
	/// device would ask for: no stored file, no rewritten scheme, and paced. See
	/// `CastUrls`.
	func target(for song: Song) async -> StreamTarget? {
		song.isVideo ? urls.video(for: song) : await urls.audio(for: song)
	}

	// MARK: - Status

	private func received(_ status: CastStatus) {
		isPlaying = status.playerState == .playing
		// Everything that is neither playing nor paused is work in progress.
		// Saying otherwise would let a client claim a position that does not
		// exist yet — **but only while something is loaded**, or an idle
		// receiver with nothing sent to it would spin a buffering indicator for
		// ever.
		isBuffering =
			playing != nil && status.playerState != .playing && status.playerState != .paused
		reported = status.currentTime
		reportedAt = status.playerState == .playing ? ContinuousClock.now : nil
		position = status.currentTime

		if let message = session.failure {
			failure = message
			session.clearFailure()
		}

		// **The end of a track, acted on once.** `mediaSessionId != 0` excludes
		// the state a session starts in, and `advancedFrom` excludes the repeats
		// a receiver sends while it sits idle — without which one finished track
		// walks the queue to its end.
		if status.isIdleFinished, status.mediaSessionId != 0,
			status.mediaSessionId != advancedFrom
		{
			advancedFrom = status.mediaSessionId
			ticker?.cancel()
			ticker = nil
			// The connection decides what happens next: it holds the queue, and
			// with a window of one it will either hand down the next track or
			// find there is none. **There is no `onEnded` here** and there does
			// not need to be: `PlayQueue.advance` clamps at the last track, so
			// running out re-plans to the window already loaded, produces
			// `.none`, and simply stops — which is what parking at the end
			// means.
			onAdvanced?(1)
		}
		onStateChange?()
	}

	/// The receiver reports about once a second, which is too coarse to draw a
	/// seek bar from. This fills in between, on the same half-second cadence the
	/// local engine's time observer uses, so nothing above the seam can tell the
	/// two apart.
	private func startTicking() {
		guard ticker == nil else { return }
		ticker = Task { [weak self] in
			while !Task.isCancelled {
				try? await Task.sleep(for: .milliseconds(500))
				guard !Task.isCancelled, let self else { return }
				guard let since = self.reportedAt else { continue }
				let extrapolated = self.reported + ContinuousClock.now.durationSince(since)
				self.position = extrapolated
				self.onProgress?(extrapolated)
			}
		}
	}
}

extension ContinuousClock.Instant {
	/// Seconds since an earlier instant, as a `Double`. `Duration` divides into
	/// components rather than offering a `seconds` property, and doing that
	/// arithmetic at three call sites would be three chances to get it wrong.
	fileprivate func durationSince(_ earlier: ContinuousClock.Instant) -> Double {
		let elapsed = earlier.duration(to: self)
		return Double(elapsed.components.seconds)
			+ Double(elapsed.components.attoseconds) / 1e18
	}
}

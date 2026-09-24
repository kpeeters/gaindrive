//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What to do to the engine's loaded items to make them match the queue.
///
/// The ref-level counterpart is `WindowEdit`, which `PlayerWindow.plan` decides
/// and which is unit tested. This is the same decision with the songs resolved,
/// because an engine has to build something from each entry and has no queue to
/// look them up in - which is the point: **the app owns the queue**, and the
/// engine holds a window onto it.
enum EngineEdit {
	case none
	/// Keep the current item - and its buffer, and its playback position - and
	/// replace what follows it.
	case replaceTail([Song])
	case rebuild([Song])
	case clear
}

/// Something that can play a window of the queue.
///
/// **The seam a cast player drops into.** `PlayerConnection`'s own header, and
/// `ARCHITECTURE.md`, have said since phase 3 that the type exists to make that
/// possible; this is the boundary that makes it true rather than intended.
///
/// Android gets the same separation from Media3 for nothing: `CastPlayer` is a
/// `SimpleBasePlayer` and the service swaps `session.player`, so nothing in its
/// `ui/` changed. There is no such interface here, so it is written down - and
/// the shape is deliberately the *smaller* half of what Media3's `Player`
/// offers, because everything that can stay above the seam should.
///
/// What stays above it: the queue, every command's meaning, every published
/// property, and the four collaborators that attach to the **role** rather than
/// to the player - `Scrobbler`, `PlaybackWatchdog`, `TranscodePrewarmer` and
/// `NowPlayingCenter`. Android broke that rule and had to repair it; here it
/// held, and this protocol is what keeps it holding.
///
/// **Nothing is pushed.** Every callback says only "something changed, re-read
/// me", which is the rule `PlayerConnection` already applies to its KVO blocks
/// and Android to `onEvents → publish()`. An engine that pushed values would
/// need two copies of the publishing logic, and they would disagree.
@MainActor
protocol PlaybackEngine: AnyObject {
	/// How many entries of the queue this engine wants loaded at once.
	///
	/// Two locally, because that is what `AVQueuePlayer` pre-buffers. One on a
	/// receiver, which is told about a single track and sent the next when it
	/// says the first finished.
	var windowSize: Int { get }

	/// What the engine is **actually** holding, in queue order.
	///
	/// Read back rather than remembered by the caller, and that is not
	/// bookkeeping preference: the two drift in both directions.
	/// `advanceToNext()` drops an entry the caller did not ask it to, and a
	/// tail that would not resolve leaves one short - and a caller reasoning
	/// from its own plan would then compare the wrong `first` and rebuild,
	/// which **restarts the track somebody is listening to**. That is precisely
	/// the failure `PlayerWindow`'s `replaceTail` exists to prevent.
	var loaded: [ItemRef] { get }

	// MARK: - State, read rather than pushed

	var isPlaying: Bool { get }
	var isBuffering: Bool { get }
	var position: Double { get }
	/// Whether seeking will do anything. A chunked response of unknown length
	/// silently ignores one, so the scrubber and the lock-screen command are
	/// driven from what the engine actually offers rather than assumed.
	var canSeek: Bool { get }
	/// Something went wrong, in words a person can act on. Read and cleared by
	/// the connection, which owns the message the UI shows.
	var failure: String? { get }
	func clearFailure()

	// MARK: - Notifications

	/// Something changed; re-read the state above.
	var onStateChange: (() -> Void)? { get set }
	/// The playback position moved, on whatever cadence this engine reports.
	///
	/// **Separate from `onStateChange` deliberately.** This fires several times
	/// a second and a transition does not, so folding the two together would put
	/// the lock-screen write and the stall watchdog on a 2 Hz timer - and the
	/// watchdog in particular cannot be driven by position at all, since during
	/// a stall there is no tick to be driven by.
	var onProgress: ((Double) -> Void)? { get set }
	/// The engine finished the head of its window and moved on by that many
	/// entries. **The queue is advanced by the connection**, never by the
	/// engine, which holds no queue to advance.
	var onAdvanced: ((Int) -> Void)? { get set }
	/// The engine ran off the end of what it was given.
	var onEnded: (() -> Void)? { get set }
	/// Everything loaded has become invalid and the window must be built again -
	/// an audio-session reset locally, and a reconnected control channel on a
	/// receiver. Distinct from `onEnded`, which means the music finished.
	var onReset: (() -> Void)? { get set }

	// MARK: - Commands

	/// Loads the window. Returns false when the **head** could not be resolved,
	/// which is the only failure worth a message: a tail that will not resolve
	/// costs pre-buffering and nothing else.
	///
	/// `offset` starts the head partway in, and is what a chapter marker asks
	/// for. Each engine honours it its own way - locally it must wait for the
	/// item to become seekable, on a receiver it rides in the LOAD - and any
	/// offset still outstanding is dropped by the next edit, because a position
	/// inside one recording means nothing in the next.
	func apply(_ edit: EngineEdit, startingAt offset: Double?) async -> Bool

	/// Move to the next entry of the window **using what is already loaded**,
	/// keeping its buffer. False when there is nothing loaded to move to, which
	/// is the caller's signal to rebuild instead - and always the answer from an
	/// engine whose window is one entry long.
	func advanceToNext() -> Bool

	func resume()
	func pause()
	func seek(to seconds: Double)
	/// Give everything up. The queue is the connection's and is not touched.
	func stop()

	/// What this engine would ask that track's own server for.
	///
	/// On the engine because the answer differs by engine rather than by track:
	/// a local play prefers a stored copy and goes through the caching loader,
	/// while a receiver must be handed something *it* can fetch - no `file:`
	/// URL, no rewritten scheme, and paced.
	func target(for song: Song) async -> StreamTarget?
}

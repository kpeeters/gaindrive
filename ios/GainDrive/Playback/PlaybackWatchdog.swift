//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Stops playback that has stopped making progress, and says so.
///
/// There is a class of failure where **nothing fails**. The server keeps
/// sending, the player keeps loading, no read times out and no error is ever
/// raised — but the clock does not advance, so the spinner stays up for good.
/// HLS segments carrying the wrong timestamps did exactly this on Android. The
/// loading machinery cannot notice, because from its point of view everything
/// is working; only the position gives it away. That is a property of the
/// server and the network rather than of either platform, which is why the
/// port is needed here too.
///
/// `isPlaybackLikelyToKeepUp` and `isPlaybackBufferEmpty` are the wrong signal
/// for the same reason ExoPlayer's own flags were: they answer a different
/// question, and are false in states that are not stalls.
///
/// Shaped like `AudioSessionController` and `NowPlayingCenter` — callbacks as
/// properties, wired once in `PlayerConnection.wireCallbacks()`.
///
/// This is a safety net, not a diagnosis. It says only that something is
/// wrong, and leaves the queue in place so pressing play is the retry.
@MainActor
final class PlaybackWatchdog {
	struct Sample: Equatable, Sendable {
		/// Buffering **and** wanting to play. Neither alone is a stall: a
		/// paused player is not making progress either, and is fine.
		let stalled: Bool
		let position: Double
	}

	/// Reads the player's state. Set by `PlayerConnection`.
	var sample: (() -> Sample)?
	/// Pause, and publish a message.
	var onStall: (() -> Void)?

	/// An init parameter so the tests can drive it in milliseconds.
	///
	/// Thirty seconds is long enough that a slow link is not mistaken for a
	/// broken one — the player starts on a couple of seconds of buffer, so half
	/// a minute without reaching that is not a bandwidth problem — and short
	/// enough that nobody sits watching a spinner wondering.
	///
	/// **Video will need a second, much longer tier**, and phase 6 is where it
	/// arrives. gaindrive's transcode cache is blocking: it runs ffmpeg over
	/// the whole source and sends nothing until the file is complete, so a
	/// remux or a soundtrack extraction from a multi-gigabyte file is minutes
	/// of buffering in which no byte can arrive. That is indistinguishable from
	/// a wedge by every signal this class has. Android allows five minutes for
	/// it. Without the tier, the first film played would be cut off after half
	/// a minute.
	private let timeout: Duration
	private var countdown: Task<Void, Never>?

	init(timeout: Duration = .seconds(30)) {
		self.timeout = timeout
	}

	/// Called whenever the transport changes. Arms while stalled, disarms
	/// otherwise — so a slow link that manages a second of playback between
	/// stalls is never touched, because the timer measures one *continuous*
	/// stretch.
	func update() {
		guard let now = sample?(), now.stalled else {
			disarm()
			return
		}
		arm(from: now.position)
	}

	func disarm() {
		countdown?.cancel()
		countdown = nil
	}

	/// **Starts counting, or leaves an existing count alone.** Not restarted on
	/// every call: the transport publishes repeatedly through a stall, and
	/// re-arming each time would push the deadline out for ever.
	private func arm(from position: Double) {
		guard countdown == nil else { return }
		let deadline = timeout
		countdown = Task { [weak self] in
			try? await Task.sleep(for: deadline)
			guard !Task.isCancelled else { return }
			self?.expire(startedAt: position)
		}
	}

	/// Re-checked rather than assumed: `disarm` cancels the task, but the
	/// cancellation and a recovery can race, and stopping playback that has
	/// just recovered is the worse failure.
	///
	/// The position comparison is exact, and it can be. `PlayerConnection`
	/// publishes `position` from the periodic time observer, which fires as the
	/// timeline *advances* and therefore not at all during a stall — so a
	/// changed value here means playback resumed, and an unchanged one means
	/// nothing has moved since the timer was armed.
	private func expire(startedAt position: Double) {
		countdown = nil
		guard let now = sample?(), now.stalled, now.position == position else { return }
		onStall?()
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import Foundation

/// The audio session: category, activation, interruptions and route changes.
///
/// Owns no playback state. It reaches `PlayerConnection` through three
/// callbacks, so the rule that the facade is the only route to playback
/// survives - this type asks for a pause, it does not perform one.
@MainActor
final class AudioSessionController {
	var onPause: (() -> Void)?
	var onResume: (() -> Void)?
	/// The media server died and took the player with it. Everything must be
	/// rebuilt from the queue.
	var onReset: (() -> Void)?

	private var wasPlayingBeforeInterruption = false
	private var isActive = false

	init() {
		// Setting the category is free and has no effect on other apps.
		// *Activating* it is what takes the route, and that waits for the first
		// play - see `activate()`.
		//
		// `.playback` alone: not `.mixWithOthers` (we are the primary audio),
		// not `.duckOthers`, and none of the AirPlay or Bluetooth options,
		// which belong to `.playAndRecord` - `.playback` already routes to
		// AirPlay and A2DP.
		try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
		observe()
	}

	/// Activated on the first play rather than at launch.
	///
	/// An app that takes the session when it starts stops whatever the user was
	/// already listening to, for a launch that may never play anything.
	///
	/// Throws when another app holds a non-mixable session - a phone call. That
	/// has to surface rather than be swallowed: it is the one case where "I
	/// pressed play and nothing happened" has an explanation the user can act
	/// on.
	func activate() throws {
		guard !isActive else { return }
		try AVAudioSession.sharedInstance().setActive(true)
		isActive = true
	}

	/// Only on stop, never on pause. Deactivating releases the route and drops
	/// the Now Playing entry, and a paused track resumed from the lock screen
	/// is the ordinary case.
	func deactivate() {
		guard isActive else { return }
		try? AVAudioSession.sharedInstance().setActive(false, options: .notifyOthersOnDeactivation)
		isActive = false
	}

	private func observe() {
		let center = NotificationCenter.default

		// Async sequences rather than selectors: no `@objc`, and the handler
		// body is already on the main actor rather than having to hop.
		Task { [weak self] in
			for await note in center.notifications(named: AVAudioSession.interruptionNotification) {
				self?.handleInterruption(note)
			}
		}
		Task { [weak self] in
			for await note in center.notifications(named: AVAudioSession.routeChangeNotification) {
				self?.handleRouteChange(note)
			}
		}
		Task { [weak self] in
			for await _ in center.notifications(
				named: AVAudioSession.mediaServicesWereResetNotification)
			{
				self?.handleReset()
			}
		}
	}

	private func handleInterruption(_ note: Notification) {
		guard let raw = note.userInfo?[AVAudioSessionInterruptionTypeKey] as? UInt,
			let type = AVAudioSession.InterruptionType(rawValue: raw)
		else { return }

		switch type {
		case .began:
			// No `.appWasSuspended` guard. Older iOS delivered a spurious
			// interruption for a suspension that never was one, and clients had
			// to filter it out; the reason is deprecated precisely because that
			// interruption is no longer sent. At an iOS 18 floor the guard is
			// dead code that only reads as if it does something.
			wasPlayingBeforeInterruption = true
			isActive = false
			onPause?()
		case .ended:
			guard wasPlayingBeforeInterruption else { return }
			wasPlayingBeforeInterruption = false
			// Resume **only** when told to. Without `.shouldResume` the user
			// has chosen something else, and taking the route back would be
			// the app talking over it.
			guard let optionsRaw = note.userInfo?[AVAudioSessionInterruptionOptionKey] as? UInt,
				AVAudioSession.InterruptionOptions(rawValue: optionsRaw).contains(.shouldResume)
			else { return }
			try? activate()
			onResume?()
		@unknown default:
			return
		}
	}

	private func handleRouteChange(_ note: Notification) {
		guard let raw = note.userInfo?[AVAudioSessionRouteChangeReasonKey] as? UInt,
			let reason = AVAudioSession.RouteChangeReason(rawValue: raw)
		else { return }

		// The becoming-noisy equivalent: headphones pulled out must not carry
		// on through the speaker. Deliberately no `.newDeviceAvailable` case -
		// plugging headphones *in* is not a request to start playing.
		guard reason == .oldDeviceUnavailable else { return }
		onPause?()
	}

	private func handleReset() {
		// Rare, and the only recovery is to rebuild: the category is gone with
		// the media server, and so is the player.
		isActive = false
		try? AVAudioSession.sharedInstance().setCategory(.playback, mode: .default)
		onReset?()
	}
}

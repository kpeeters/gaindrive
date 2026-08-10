//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import MediaPlayer

/// The lock screen and Control Centre, maintained by hand.
///
/// None of this is free the way Media3's notification is on Android, which is
/// the whole reason it is a file rather than a line.
@MainActor
final class NowPlayingCenter {
	var onPlay: (() -> Void)?
	var onPause: (() -> Void)?
	var onTogglePlayPause: (() -> Void)?
	var onNext: (() -> Void)?
	var onPrevious: (() -> Void)?
	var onSeek: ((Double) -> Void)?

	private var currentRef: ItemRef?

	init() {
		registerCommands()
	}

	/// Rebuilt on a track change only.
	func setTrack(_ song: Song, index: Int, count: Int, cover: CoverSource?) {
		currentRef = song.ref
		var info: [String: Any] = [
			MPMediaItemPropertyTitle: song.title,
			MPMediaItemPropertyArtist: song.artistName,
			MPMediaItemPropertyAlbumTitle: song.albumTitle,
			// **The server's figure, not the asset's.** On the chunked fallback
			// `AVPlayerItem.duration` is `.indefinite`, and a lock screen that
			// trusted it would show no length exactly when something had
			// already gone slightly wrong.
			MPMediaItemPropertyPlaybackDuration: Double(song.duration),
			MPNowPlayingInfoPropertyMediaType: MPNowPlayingInfoMediaType.audio.rawValue,
			MPNowPlayingInfoPropertyIsLiveStream: false,
			MPNowPlayingInfoPropertyDefaultPlaybackRate: 1.0,
			MPNowPlayingInfoPropertyPlaybackQueueIndex: index,
			MPNowPlayingInfoPropertyPlaybackQueueCount: count,
		]
		if let track = song.track { info[MPMediaItemPropertyAlbumTrackNumber] = track }
		if let disc = song.discNumber { info[MPMediaItemPropertyDiscNumber] = disc }
		MPNowPlayingInfoCenter.default().nowPlayingInfo = info

		loadArtwork(cover, for: song.ref)
	}

	/// Patched on play, pause and seek — and **nowhere else**.
	///
	/// Not from the position tick: iOS extrapolates elapsed time from the rate
	/// and the last update, so writing it twice a second is wasted work that
	/// also makes the lock-screen scrubber visibly jitter.
	func setPlayback(isPlaying: Bool, position: Double) {
		var info = MPNowPlayingInfoCenter.default().nowPlayingInfo ?? [:]
		info[MPNowPlayingInfoPropertyElapsedPlaybackTime] = position
		info[MPNowPlayingInfoPropertyPlaybackRate] = isPlaying ? 1.0 : 0.0
		MPNowPlayingInfoCenter.default().nowPlayingInfo = info
		// Required for the Mac Catalyst controls to behave; harmless on iOS.
		MPNowPlayingInfoCenter.default().playbackState = isPlaying ? .playing : .paused
	}

	func setAvailability(hasNext: Bool, canSeek: Bool) {
		let centre = MPRemoteCommandCenter.shared()
		centre.nextTrackCommand.isEnabled = hasNext
		// Always meaningful: with nothing before it, previous restarts the
		// track.
		centre.previousTrackCommand.isEnabled = true
		// Inert exactly when a scrub would not work — which is the chunked
		// fallback, where the response has no length and no ranges.
		centre.changePlaybackPositionCommand.isEnabled = canSeek
	}

	func clear() {
		currentRef = nil
		MPNowPlayingInfoCenter.default().nowPlayingInfo = nil
		MPNowPlayingInfoCenter.default().playbackState = .stopped
	}

	/// Fetched in its own task so the rest of the info publishes immediately —
	/// a lock screen that waited for a download would be blank for as long as
	/// the download took.
	private func loadArtwork(_ cover: CoverSource?, for ref: ItemRef) {
		guard let cover else { return }
		Task { @MainActor in
			guard let image = await ImageStore.shared.image(for: cover) else { return }
			// The track moved on while this was in flight. Without the guard, a
			// fast skip through five tracks leaves the wrong cover on the lock
			// screen.
			guard currentRef == ref else { return }
			var info = MPNowPlayingInfoCenter.default().nowPlayingInfo ?? [:]
			// **Patched, not rebuilt.** A rebuild here would clobber the
			// elapsed time written a moment ago.
			info[MPMediaItemPropertyArtwork] = MPMediaItemArtwork(boundsSize: image.size) { _ in
				image
			}
			MPNowPlayingInfoCenter.default().nowPlayingInfo = info
		}
	}

	/// Registered **once**. A second `addTarget` on the same command fires the
	/// handler twice, which reads as "next skips two tracks".
	private func registerCommands() {
		let centre = MPRemoteCommandCenter.shared()

		centre.playCommand.addTarget { [weak self] _ in
			self?.onPlay?()
			return .success
		}
		centre.pauseCommand.addTarget { [weak self] _ in
			self?.onPause?()
			return .success
		}
		centre.togglePlayPauseCommand.addTarget { [weak self] _ in
			// Headphone controls and the CarPlay button send this rather than
			// play or pause, so it needs its own handler.
			self?.onTogglePlayPause?()
			return .success
		}
		centre.nextTrackCommand.addTarget { [weak self] _ in
			// No "is there a next track" guard: `setAvailability` disables the
			// command when there is not, and a disabled command is never
			// delivered.
			self?.onNext?()
			return .success
		}
		centre.previousTrackCommand.addTarget { [weak self] _ in
			self?.onPrevious?()
			return .success
		}
		centre.changePlaybackPositionCommand.addTarget { [weak self] event in
			guard let event = event as? MPChangePlaybackPositionCommandEvent else {
				return .commandFailed
			}
			self?.onSeek?(event.positionTime)
			return .success
		}

		// Disabled explicitly rather than left at their defaults, which would
		// put controls on the lock screen and in CarPlay that do nothing.
		for command in [
			centre.stopCommand, centre.seekForwardCommand, centre.seekBackwardCommand,
			centre.skipForwardCommand, centre.skipBackwardCommand,
			centre.changeRepeatModeCommand, centre.changeShuffleModeCommand,
			centre.ratingCommand, centre.likeCommand, centre.dislikeCommand,
			centre.bookmarkCommand,
		] {
			command.isEnabled = false
		}
	}
}

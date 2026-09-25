//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVFoundation
import MediaPlayer
import UIKit
import os

/// Brightness and volume by dragging a finger up or down one side of the
/// picture: the left side is brightness, the right the volume. Android's
/// `videoSideGestures`, and the way VLC for iOS does it.
///
/// It exists because the two controls most wanted during a film are the two
/// hardest to reach: the volume buttons are small and awkwardly placed on a
/// tablet, and brightness is behind Control Centre, which takes the viewer out
/// of the picture.
///
/// **A pan recogniser on the player controller's own view**, and not a SwiftUI
/// layer over it. `contentOverlayView` receives no touches, and a view on top
/// would swallow the taps that show the transport. A recogniser only claims a
/// touch once it has become a vertical drag starting in a side zone, so a tap
/// still shows the controls and a horizontal scrub still scrubs.
///
/// **Two routes, neither of them a setter Apple documents for this.**
/// Brightness is `UIScreen.brightness`, which is public but changes the whole
/// device, so it is handed back when the picture goes and when the app leaves
/// the foreground. Volume has no public setter at all; the slider inside an
/// `MPVolumeView` is the one that moves the system volume, and an
/// `MPVolumeView` in the hierarchy is also what stops the system drawing its
/// own volume panel over the film. VLC and others ship this.
@MainActor
final class VideoSideGestures: NSObject, UIGestureRecognizerDelegate {
	/// Reports what a swipe is doing, and nil when it ends - which is what
	/// starts the indicator's fade.
	var onAdjust: ((SideAdjustment?) -> Void)?

	private let volumeView = MPVolumeView(frame: CGRect(x: -1000, y: -1000, width: 1, height: 1))
	private weak var host: UIView?

	private var control: SideControl?
	private var level: Double = 0
	/// What the screen was at before the first swipe of this visit, so it can
	/// be handed back. Nil until a brightness swipe happens: a film nobody
	/// dimmed has nothing to restore.
	private var originalBrightness: CGFloat?

	private static let log = Logger(subsystem: "org.gaindrive.ios-player", category: "video")

	/// Clear of the screen edges, where the system's own gestures begin.
	private static let edgeInset: CGFloat = 24
	/// Dim, but never so dim that the way back cannot be found.
	private static let minBrightness: CGFloat = 0.02

	func attach(to view: UIView) {
		host = view
		// In the hierarchy, or the slider moves nothing and the system panel
		// appears. Nearly transparent rather than hidden, for the same reason.
		volumeView.alpha = 0.01
		view.addSubview(volumeView)

		let pan = UIPanGestureRecognizer(target: self, action: #selector(panned(_:)))
		pan.maximumNumberOfTouches = 1
		pan.delegate = self
		view.addGestureRecognizer(pan)

		NotificationCenter.default.addObserver(
			self, selector: #selector(resignedActive),
			name: UIApplication.willResignActiveNotification, object: nil)
	}

	/// Hands the brightness back. Called when the picture goes, and when the
	/// app leaves the foreground: a device left dimmed with nothing on screen
	/// saying why is the failure to avoid.
	func restoreBrightness() {
		guard let original = originalBrightness else { return }
		screen?.brightness = original
		originalBrightness = nil
	}

	@objc private func resignedActive() {
		restoreBrightness()
	}

	// MARK: - Recogniser

	/// Only a vertical drag that began in a side zone. Everything else is left
	/// to the player controller.
	func gestureRecognizerShouldBegin(_ recognizer: UIGestureRecognizer) -> Bool {
		guard let pan = recognizer as? UIPanGestureRecognizer, let view = pan.view else {
			return false
		}
		let velocity = pan.velocity(in: view)
		guard abs(velocity.y) > abs(velocity.x) else { return false }
		// Where the finger came down, not where it is now it has moved.
		let start = pan.location(in: view).x - pan.translation(in: view).x
		return VideoGestureMath.control(
			atX: start, width: view.bounds.width, edgeInset: Self.edgeInset) != nil
	}

	@objc private func panned(_ pan: UIPanGestureRecognizer) {
		guard let view = pan.view else { return }
		switch pan.state {
		case .began:
			let start = pan.location(in: view).x - pan.translation(in: view).x
			control = VideoGestureMath.control(
				atX: start, width: view.bounds.width, edgeInset: Self.edgeInset)
			// Read at the start of every swipe rather than carried over, so a
			// change made with the buttons in between is not undone.
			level = control.map(read) ?? 0
			pan.setTranslation(.zero, in: view)
			report()
		case .changed:
			guard let control else { return }
			let step = VideoGestureMath.travelFraction(
				dy: pan.translation(in: view).y, height: view.bounds.height)
			pan.setTranslation(.zero, in: view)
			level = min(max(level + step, 0), 1)
			write(level, to: control)
			report()
		default:
			control = nil
			onAdjust?(nil)
		}
	}

	private func report() {
		guard let control else { return }
		onAdjust?(SideAdjustment(control: control, level: level))
	}

	// MARK: - The two controls

	private var screen: UIScreen? {
		host?.window?.windowScene?.screen
	}

	private func read(_ control: SideControl) -> Double {
		switch control {
		case .brightness:
			return Double(screen?.brightness ?? 0.5)
		case .volume:
			return Double(AVAudioSession.sharedInstance().outputVolume)
		}
	}

	private func write(_ value: Double, to control: SideControl) {
		switch control {
		case .brightness:
			guard let screen else { return }
			if originalBrightness == nil { originalBrightness = screen.brightness }
			screen.brightness = max(CGFloat(value), Self.minBrightness)
		case .volume:
			guard let slider = volumeView.subviews.compactMap({ $0 as? UISlider }).first else {
				// A future iOS that rearranges `MPVolumeView` would make this a
				// swipe that silently does nothing; say so where it can be read.
				Self.log.error("no slider inside MPVolumeView; volume swipe does nothing")
				return
			}
			slider.value = Float(value)
		}
	}
}

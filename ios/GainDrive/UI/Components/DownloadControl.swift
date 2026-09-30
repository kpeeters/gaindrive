//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Keep this, or stop keeping it.
///
/// **It reports what is actually happening rather than what was asked for** -
/// `android/SCREENS.md`'s rule for the same control, and the reason it is a
/// state and not a checkbox. An outline for nothing yet, a ring while tracks
/// arrive, a red outline when one failed, a filled tick when it is all here.
///
/// No confirmation: it is a toggle with its state on its face, and tapping
/// again puts the download back. Settings → Storage is the managing surface,
/// where a mis-tap is less obviously reversible, and that one does confirm.
struct DownloadControl: View {
	let pin: Pin

	@Environment(PinRepository.self) private var pins

	var body: some View {
		Button {
			Task { await pins.toggle(pin) }
		} label: {
			icon
		}
		.buttonStyle(.borderless)
		.accessibilityLabel(label)
	}

	/// `absent` unless it is pinned: the repository answers for a pin's
	/// membership whether or not the pin is still there, and a removed one
	/// must not go on showing a tick.
	private var state: DownloadState {
		pins.isPinned(pin.ref, kind: pin.kind) ? pins.state(of: pin) : .absent
	}

	/// Accented when idle: here it is a button in a bar, drawn like the bar's
	/// other buttons, where the plain icon is a state mark.
	private var icon: some View { DownloadStateIcon(state: state, accented: true) }

	private var label: String {
		switch state {
		case .stored: return "Downloaded. Tap to remove"
		case .cached: return "Stored. Tap to remove"
		case .failed: return "Download failed. Tap to remove"
		case .running(let fraction):
			return "Downloading, \(Int(fraction * 100)) per cent. Tap to remove"
		case .waiting: return "Waiting for Wi-Fi. Tap to remove"
		case .absent: return "Download"
		}
	}
}

/// The state on its own, with nothing to tap.
///
/// Separate from the control because Settings → Storage shows the same four
/// states beside a row whose *swipe* is the way to remove, and confirms. A
/// control there would remove on a tap without confirming, which is the one
/// thing that screen is not supposed to do.
struct DownloadStateIcon: View {
	let state: DownloadState
	var accented = false

	var body: some View {
		switch state {
		case .absent:
			Image(systemName: "arrow.down.circle")
				.foregroundStyle(accented ? AnyShapeStyle(Color.accentColor) : AnyShapeStyle(.secondary))
		case .running(let fraction):
			// Determinate as soon as anything has arrived: a spinner says
			// "working", a ring says how much longer.
			ProgressView(value: max(fraction, 0.02))
				.progressViewStyle(.circular)
				.controlSize(.small)
		case .waiting:
			Image(systemName: "clock")
				.foregroundStyle(.secondary)
		case .stored:
			Image(systemName: "arrow.down.circle.fill")
				.foregroundStyle(Color.accentColor)
		case .cached:
			// Reachable only for a track pinned *and* evicted, which cannot
			// happen - pinned bytes are never victims. Drawn as the dot the
			// mark uses rather than as an assertion.
			Image(systemName: "circle.fill")
				.font(.caption2)
				.foregroundStyle(.secondary)
		case .failed:
			Image(systemName: "exclamationmark.circle")
				.foregroundStyle(.red)
		}
	}
}

/// Whether a track is here, and on what terms.
///
/// **Two marks, and the difference is a promise.** A tick means downloaded:
/// asked for, and safe from eviction. A dot means merely kept from having been
/// played, which can go tonight when the cap is reached. Collapsing them would
/// promise a permanence the dot does not have - which is why stage 1 shipped
/// one mark and said so rather than drawing a tick for both.
struct StoredMark: View {
	let song: ItemRef

	@Environment(PinRepository.self) private var pins

	var body: some View {
		switch pins.state(for: song) {
		case .stored:
			Image(systemName: "arrow.down.circle.fill")
				.font(.caption)
				.foregroundStyle(.secondary)
				.accessibilityLabel("Downloaded")
		case .cached:
			Image(systemName: "circle.fill")
				.font(.system(size: 5))
				.foregroundStyle(.secondary)
				.accessibilityLabel("Stored")
		// The same ring, clock and error the album's control shows, so a track
		// row never disagrees with the header above it about what is happening.
		case .running(let fraction):
			ProgressView(value: max(fraction, 0.02))
				.progressViewStyle(.circular)
				.controlSize(.mini)
				.accessibilityLabel("Downloading, \(Int(fraction * 100)) per cent")
		case .waiting:
			Image(systemName: "clock")
				.font(.caption)
				.foregroundStyle(.secondary)
				.accessibilityLabel("Waiting for Wi-Fi")
		case .failed:
			Image(systemName: "exclamationmark.circle")
				.font(.caption)
				.foregroundStyle(.red)
				.accessibilityLabel("Download failed")
		case .absent:
			EmptyView()
		}
	}
}

/// Where a whole album or playlist stands, beside its row in a listing.
///
/// **Only for a pin.** Nothing is drawn for one that is not pinned: the
/// control's own "absent" state is a download arrow, which on a button is an
/// invitation and down the side of a list is a column of buttons that do
/// nothing - `android/SCREENS.md`'s reason for the same rule. Android also
/// draws a dot for an unpinned album whose every track was kept from playing;
/// that needs the album's membership, which only a pin records here.
struct ContainerMark: View {
	let ref: ItemRef
	let kind: PinKind

	@Environment(PinRepository.self) private var pins

	var body: some View {
		if let state = pins.containerState(ref, kind: kind) {
			DownloadStateIcon(state: state)
				.font(.caption)
				.controlSize(.mini)
		}
	}
}

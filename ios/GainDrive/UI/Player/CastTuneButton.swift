//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The receiver's volume and, on a WiiM, its equalizer, one click from the
/// player as the web's tune button is. Not in the cast picker: that sheet says
/// *where* the sound goes, and these change what it sounds like once there.
///
/// **Only while casting.** The web's button doubles as the browser's own
/// equalizer when nothing is cast; `AVPlayer` has none, so here there is
/// nothing behind it then.
struct CastTuneButton: View {
	@Environment(PlayerConnection.self) private var player
	@State private var showing = false

	var body: some View {
		if let device = player.castDevice {
			Button {
				showing = true
			} label: {
				Image(systemName: "slider.horizontal.3")
			}
			.accessibilityLabel(
				device.kind == .wiim ? "Equalizer and volume" : "Receiver volume")
			// A popover so the panel stays beside the bar on a Mac or iPad, as
			// the web's does; a compact iPhone shows it as a sheet unasked.
			.popover(isPresented: $showing, arrowEdge: .top) {
				CastTunePanel(device: device, session: player.castSession)
			}
		}
	}
}

/// What the tune button opens. Handed the session rather than looking up
/// `PlayerConnection`: a Mac popover or sheet has been presented without its
/// environment before.
struct CastTunePanel: View {
	let device: CastDevice
	let session: CastSession
	@Environment(\.dismiss) private var dismiss

	var body: some View {
		content
			// Casting stopped, or moved to another receiver: what is drawn here
			// would address the wrong box.
			.onChange(of: session.device) { _, now in
				if now?.id != device.id { dismiss() }
			}
	}

	@ViewBuilder
	private var content: some View {
		#if os(macOS)
			VStack(alignment: .leading, spacing: 14) {
				panelBody
			}
			.padding(16)
			.frame(minWidth: 320)
		#else
			NavigationStack {
				panelBody
					.navigationTitle(device.kind == .wiim ? "Equalizer" : "Volume")
					.inlineTitle()
					.toolbar {
						ToolbarItem(placement: .confirmationAction) {
							Button("Done") { dismiss() }
						}
					}
			}
			// A `Form` has no ideal size, which collapses an iPad popover.
			// The iPhone shows a sheet and ignores this.
			.frame(minWidth: 360, minHeight: 480)
		#endif
	}

	/// The WiiM's equalizer when its address is known; until then, and for any
	/// other receiver, the volume alone.
	@ViewBuilder
	private var panelBody: some View {
		if device.kind == .wiim, let address = session.address {
			WiiMControlsView(deviceId: device.id, address: address) { volume }
		} else {
			#if os(macOS)
				volume
				if device.kind == .wiim { connecting }
			#else
				Form {
					volume
					if device.kind == .wiim { connecting }
				}
			#endif
		}
	}

	@ViewBuilder
	private var volume: some View {
		#if os(macOS)
			VStack(alignment: .leading, spacing: 6) {
				Text(device.name).font(.headline)
				CastVolumeRow(session: session)
			}
		#else
			Section {
				CastVolumeRow(session: session)
			} header: {
				Text(device.name)
			}
		#endif
	}

	/// The equalizer speaks HTTP to the address the Cast channel reached, so
	/// it waits for that connection.
	private var connecting: some View {
		Text("Equalizer: connecting…").foregroundStyle(.secondary)
	}
}

/// The receiver's device volume: a bar that shows it and two buttons that step
/// it, one per cent a tap, as on Android.
///
/// **Deliberately not a slider.** A slider can take an amplifier from quiet to
/// 100% in one careless drag or a stray tap at its end, and on a WiiM driving
/// real speakers that is a way to damage them or somebody's ears. Steps make
/// every change small and deliberate. The bar is information only.
///
/// Both buttons wait until the receiver has said what the volume is: stepping
/// from a guess could jump it. SET_VOLUME is one code path for every receiver,
/// a WiiM included.
struct CastVolumeRow: View {
	let session: CastSession

	/// One per cent. The receiver's own step size is unknowable, so this is a
	/// feel choice rather than a mapping - Android's `CAST_VOLUME_STEP`.
	static let step = 0.01

	var body: some View {
		HStack(spacing: 12) {
			Button {
				stepBy(-Self.step)
			} label: {
				Image(systemName: "speaker.minus.fill")
			}
			.disabled(level == nil || level == 0)
			.accessibilityLabel("Volume down")
			ProgressView(value: level ?? 0)
				.accessibilityLabel("Volume")
				.accessibilityValue(level.map { "\(Int(($0 * 100).rounded())) per cent" } ?? "Unknown")
			Text(level.map { "\(Int(($0 * 100).rounded()))%" } ?? "–")
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
				.frame(minWidth: 36, alignment: .trailing)
			Button {
				stepBy(Self.step)
			} label: {
				Image(systemName: "speaker.plus.fill")
			}
			.disabled(level == nil || level == 1)
			.accessibilityLabel("Volume up")
		}
		// In a List row, a plain button style keeps each button its own tap
		// target rather than the whole row answering for the first one.
		.buttonStyle(.borderless)
	}

	private var level: Double? { session.volume?.level }

	/// Rounded to the step, so repeated taps land on whole per cents rather
	/// than drifting with whatever fraction the receiver reported.
	private func stepBy(_ delta: Double) {
		guard let level else { return }
		let next = ((level + delta) * 100).rounded() / 100
		session.setVolume(min(max(next, 0), 1))
	}
}

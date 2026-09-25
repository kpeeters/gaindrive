//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

struct WiiMEqUi: Equatable {
	var enabled: Bool
	/// The ticked preset, device or saved; nil for a hand-shaped curve.
	var preset: String?
	var presets: [String]
	var bands: [Int]?
	var saved: [String]
}

/// The WiiM equalizer screen's state. A port of Android's
/// `WiiMControlsViewModel`, with the same rules:
///
/// * **Every mutation re-reads the device afterwards** and publishes what it
///   said, not what was assumed. The optimistic update in between only makes
///   the tick move under the finger, and is put back on failure.
/// * **Choosing a preset sends `EQOn` after `EQLoad`**, since it is undocumented
///   whether loading enables the equalizer, and tapping "Rock" with it off
///   must not be a silent no-op.
/// * Nothing polls: the screen re-reads whenever it opens.
@MainActor
@Observable
final class WiiMControlsModel {
	private(set) var state: Load<WiiMEqUi> = .loading
	private(set) var busy = false
	var error: String?

	@ObservationIgnored private let deviceId: String
	@ObservationIgnored private let address: String
	@ObservationIgnored private let client: WiiMClient
	@ObservationIgnored private let store: EqPresetStore
	@ObservationIgnored private var slots: [String: [Int]] = [:]

	private static let bandCount = WiiMEq.bands.count

	init(
		deviceId: String, address: String,
		client: WiiMClient = .shared, store: EqPresetStore = EqPresetStore()
	) {
		self.deviceId = deviceId
		self.address = address
		self.client = client
		self.store = store
	}

	func refresh() async {
		state = .loading
		do {
			// State first: it is the reachability test, which is what lets the
			// preset list fall back to the documented one without hiding a
			// device that is simply not there.
			let eq = try await client.eqState(address: address)
			let presets = await client.presets(address: address)
			slots = store.slots(for: deviceId, bands: Self.bandCount)
			state = .ready(
				WiiMEqUi(
					enabled: eq.enabled,
					preset: Self.shownPreset(
						device: eq, last: store.lastCurve(for: deviceId),
						presets: presets, saved: Set(slots.keys)),
					presets: presets,
					bands: eq.bands,
					saved: slots.keys.sorted()))
		} catch {
			state = .failed(error.userMessage)
		}
	}

	/// Which preset to tick.
	///
	/// **The name is this app's bookkeeping, not the device's `Name`**, which
	/// goes on naming the last `EQLoad` after the faders have moved and cannot
	/// know a preset saved here at all. So a device curve equal to the one this
	/// app last wrote keeps the name recorded with it (a saved preset, or none
	/// for hand-shaped); any other curve was set elsewhere, and the device's
	/// own `Name` is the best answer there is.
	nonisolated static func shownPreset(
		device: WiiMEqState, last: (levels: [Int], preset: String?)?,
		presets: [String], saved: Set<String>
	) -> String? {
		guard let bands = device.bands, let last, bands == last.levels else {
			return device.preset
		}
		return last.preset.flatMap { presets.contains($0) || saved.contains($0) ? $0 : nil }
	}

	func selectPreset(_ preset: String) {
		mutate(
			failure: "Could not load that preset",
			optimistic: {
				$0.enabled = true
				$0.preset = preset
			}
		) { client, address in
			guard try await client.loadPreset(address: address, preset) else {
				throw Refusal("The device would not load that preset.")
			}
			_ = try await client.setEnabled(address: address, true)
		}
	}

	func applySaved(_ name: String) {
		guard let levels = slots[name] else { return }
		mutate(
			failure: "Could not apply that preset",
			optimistic: {
				$0.enabled = true
				$0.preset = name
				$0.bands = levels
			}
		) { client, address in
			guard try await client.setBands(address: address, levels) else {
				throw Refusal("The device would not change the equalizer.")
			}
			_ = try await client.setEnabled(address: address, true)
		}
	}

	func setEnabled(_ enabled: Bool) {
		mutate(
			failure: enabled ? "Could not switch the equalizer on" : "Could not switch it off",
			optimistic: { $0.enabled = enabled }
		) { client, address in
			guard try await client.setEnabled(address: address, enabled) else {
				throw Refusal("The device would not change the equalizer.")
			}
		}
	}

	/// A fader being dragged. Local only; `commitBands` writes on release.
	func setBand(_ index: Int, to value: Int) {
		guard var ui = state.value, var bands = ui.bands, bands.indices.contains(index) else {
			return
		}
		bands[index] = min(max(value, WiiMEq.levelMin), WiiMEq.levelMax)
		ui.bands = bands
		// A moved fader is no longer any named preset's curve.
		ui.preset = nil
		state = .ready(ui)
	}

	func commitBands() {
		guard let bands = state.value?.bands else { return }
		mutate(
			failure: "Could not change the equalizer",
			optimistic: { $0.preset = nil }
		) { client, address in
			guard try await client.setBands(address: address, bands) else {
				throw Refusal("The device would not change the equalizer.")
			}
		}
	}

	/// Saves the current curve under `name`, on this device only. False, with
	/// `error` set, when the name cannot be used.
	func save(_ name: String) -> Bool {
		guard var ui = state.value, let bands = ui.bands else { return false }
		let trimmed = name.trimmingCharacters(in: .whitespaces)
		if trimmed.isEmpty {
			error = "A preset needs a name."
			return false
		}
		if ui.presets.contains(trimmed) {
			error = "“\(trimmed)” is already one of the device's presets."
			return false
		}
		slots[trimmed] = bands
		ui.preset = trimmed
		ui.saved = slots.keys.sorted()
		state = .ready(ui)
		store.saveSlot(trimmed, levels: bands, for: deviceId, bands: Self.bandCount)
		store.setLastCurve(bands, preset: trimmed, for: deviceId)
		return true
	}

	/// Deletes a saved preset. The curve stays on the device; if it was the
	/// ticked one, the curve is simply hand-shaped from now on.
	func deleteSaved(_ name: String) {
		guard var ui = state.value, slots[name] != nil else { return }
		slots[name] = nil
		if ui.preset == name { ui.preset = nil }
		ui.saved = slots.keys.sorted()
		state = .ready(ui)
		store.deleteSlot(name, for: deviceId, bands: Self.bandCount)
		if let bands = ui.bands { store.setLastCurve(bands, preset: ui.preset, for: deviceId) }
	}

	private struct Refusal: LocalizedError {
		let errorDescription: String?
		init(_ message: String) { errorDescription = message }
	}

	private func mutate(
		failure: String,
		optimistic: (inout WiiMEqUi) -> Void,
		_ block: @escaping @Sendable (WiiMClient, String) async throws -> Void
	) {
		guard !busy, case .ready(let before) = state else { return }
		var guessed = before
		optimistic(&guessed)
		state = .ready(guessed)
		busy = true
		let client = client
		let address = address
		Task {
			do {
				try await block(client, address)
				let eq = try await client.eqState(address: address)
				var confirmed = guessed
				confirmed.enabled = eq.enabled
				confirmed.bands = eq.bands ?? guessed.bands
				state = .ready(confirmed)
				if let bands = confirmed.bands {
					store.setLastCurve(bands, preset: confirmed.preset, for: deviceId)
				}
			} catch {
				state = .ready(before)
				self.error = "\(failure): \(error.userMessage)"
			}
			busy = false
		}
	}
}

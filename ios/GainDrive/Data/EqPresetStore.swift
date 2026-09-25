//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Equalizer curves kept on this device, per output device: the named presets
/// saved here, and the last curve this app wrote with the name it came from.
///
/// **Here and not on the speaker.** A WiiM's own preset store has no documented
/// write, so a preset saved in this app is a curve it replays with `EQSetBand`,
/// invisible to the WiiM app. Keyed by the Cast device id, which survives a
/// rename and a new address. Android's `EqStore` does the same, under the same
/// key names, holding millibels for its local equalizer and 0-99 here.
@MainActor
struct EqPresetStore {
	private let defaults: UserDefaults

	init(defaults: UserDefaults = .standard) {
		self.defaults = defaults
	}

	/// Saved presets by name. A curve of the wrong length could only come
	/// from a damaged entry and could not be written anyway, so it is dropped.
	func slots(for device: String, bands: Int) -> [String: [Int]] {
		guard let data = defaults.data(forKey: "eq_slots_\(device)"),
			let decoded = try? JSONDecoder().decode([String: [Int]].self, from: data)
		else { return [:] }
		return decoded.filter { $0.value.count == bands }
	}

	func saveSlot(_ name: String, levels: [Int], for device: String, bands: Int) {
		var all = slots(for: device, bands: bands)
		all[name] = levels
		write(all, for: device)
	}

	func deleteSlot(_ name: String, for device: String, bands: Int) {
		var all = slots(for: device, bands: bands)
		all[name] = nil
		write(all, for: device)
	}

	/// The curve this app last wrote, and the preset it came from - nil when
	/// it was shaped by hand. See `WiiMControlsModel.refresh` for why the name
	/// is the app's bookkeeping and not the device's.
	func lastCurve(for device: String) -> (levels: [Int], preset: String?)? {
		guard let raw = defaults.string(forKey: "eq_levels_\(device)") else { return nil }
		let levels = raw.split(separator: ",").compactMap { Int($0) }
		return (levels, defaults.string(forKey: "eq_preset_\(device)"))
	}

	/// Written together, as one fact: a curve and the name it came from.
	func setLastCurve(_ levels: [Int], preset: String?, for device: String) {
		defaults.set(levels.map(String.init).joined(separator: ","), forKey: "eq_levels_\(device)")
		defaults.set(preset, forKey: "eq_preset_\(device)")
	}

	private func write(_ slots: [String: [Int]], for device: String) {
		if slots.isEmpty {
			defaults.removeObject(forKey: "eq_slots_\(device)")
		} else if let data = try? JSONEncoder().encode(slots) {
			defaults.set(data, forKey: "eq_slots_\(device)")
		}
	}
}

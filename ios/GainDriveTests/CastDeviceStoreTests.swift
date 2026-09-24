//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing

@testable import GainDrive

/// Devices somebody named by address, and how they stop showing twice.
@MainActor
struct CastDeviceStoreTests {
	/// A defaults suite of its own per test, so nothing here reads or writes the
	/// user's own - the shape `ServerConfigTests` uses.
	private func store() -> CastDeviceStore {
		let suite = UserDefaults(suiteName: "cast-store-\(UUID().uuidString)")!
		return CastDeviceStore(defaults: suite)
	}

	@Test func savingAndRemoving() {
		let store = store()
		let device = ManualCastDevice(address: "192.168.1.50", name: "Kitchen")
		store.save(device)
		#expect(store.devices.count == 1)

		// Saving the same id edits rather than appending - the editor hands
		// back the value it was given.
		var edited = device
		edited.name = "Kitchen speaker"
		store.save(edited)
		#expect(store.devices.count == 1)
		#expect(store.devices.first?.name == "Kitchen speaker")

		store.remove(id: device.id)
		#expect(store.devices.isEmpty)
	}

	@Test func devicesSurviveARestart() {
		let suite = UserDefaults(suiteName: "cast-store-\(UUID().uuidString)")!
		let first = CastDeviceStore(defaults: suite)
		first.save(ManualCastDevice(address: "192.168.1.50", name: "Kitchen", port: 8010))

		let second = CastDeviceStore(defaults: suite)
		#expect(second.devices.count == 1)
		#expect(second.devices.first?.port == 8010)
	}

	/// A malformed document yields an empty list rather than a crash: the person
	/// sees no configured devices and can add one, which is recoverable, and a
	/// launch-time trap is not.
	@Test func aMalformedDocumentIsEmpty() {
		let suite = UserDefaults(suiteName: "cast-store-\(UUID().uuidString)")!
		suite.set(Data("not a device list".utf8), forKey: "cast_devices")
		#expect(CastDeviceStore(defaults: suite).devices.isEmpty)
	}

	/// The id is spelled exactly as the server spells it in `listCastDevices`,
	/// so the two describe the same device by the same name - and it is derived
	/// rather than random, so it survives a restart.
	@Test func theDerivedIdMatchesTheServersSpelling() {
		let device = ManualCastDevice(address: "192.168.1.50", name: "Kitchen")
		#expect(device.device.id == "manual:192.168.1.50:8009")
	}

	/// A device that announces nothing is *capable* as far as anything acting on
	/// it is concerned - refusing the picture on a guess is worse than the
	/// guess - so nil is carried rather than false.
	@Test func aConfiguredDeviceAnnouncesNoCapabilities() {
		let device = ManualCastDevice(address: "192.168.1.50", name: "Kitchen").device
		#expect(device.videoOut == nil)
		#expect(device.model == nil)
		#expect(device.kind == .generic)
	}

	/// A name is allowed to be empty and falls back to the address, which is the
	/// one label that is certainly true.
	@Test func anUnnamedDeviceIsLabelledByItsAddress() {
		#expect(ManualCastDevice(address: "192.168.1.50", name: "").device.name == "192.168.1.50")
	}

	/// **Deduplicated by address, discovery winning.** A discovered entry
	/// carries the device's own name and Cast id; a configured one has only what
	/// somebody typed.
	@Test func aDiscoveredDeviceSuppressesTheConfiguredOne() {
		let store = store()
		store.save(ManualCastDevice(address: "192.168.1.50", name: "Kitchen"))
		store.save(ManualCastDevice(address: "192.168.1.51", name: "Study"))

		let discovered = CastDevice(
			id: "abc", name: "WiiM Pro",
			endpoint: .service(name: "abc", type: "_googlecast._tcp", domain: nil),
			address: "192.168.1.50")
		#expect(store.notDiscovered(among: [discovered]).map(\.address) == ["192.168.1.51"])
	}

	/// A discovered device has no address until something has connected to it,
	/// since `NWBrowser` reports a service - so before that both entries show,
	/// which is untidy and not wrong.
	@Test func anUnprobedDiscoverySuppressesNothing() {
		let store = store()
		store.save(ManualCastDevice(address: "192.168.1.50", name: "Kitchen"))

		let discovered = CastDevice(
			id: "abc", name: "WiiM Pro",
			endpoint: .service(name: "abc", type: "_googlecast._tcp", domain: nil))
		#expect(store.notDiscovered(among: [discovered]).count == 1)
	}
}

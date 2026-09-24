//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A receiver somebody named by address, because discovery could not find it.
///
/// **It exists for a measured failure rather than as belt and braces.** A
/// Chromecast on the reference network answered `ping` and accepted TLS on 8009
/// while `dig @<ip> -p 5353 _googlecast._tcp.local PTR` timed out - so its mDNS
/// responder had stopped answering even a **direct unicast query**, which is the
/// one form no access point and no multicast suppression can be blamed for.
/// Chrome could not see it either. The same option exists on Android and in the
/// server's own configuration.
///
/// On iOS it is additionally the only route left when the Local Network
/// permission has been refused. The connection fails there too - the permission
/// gates reaching a LAN address, not merely browsing for one - but a list with
/// an entry in it can explain that, and an empty one cannot.
struct ManualCastDevice: Identifiable, Hashable, Sendable, Codable {
	let id: UUID
	/// **An IP literal.** A hostname is not refused here - `NWEndpoint.Host`
	/// will take one and Bonjour or DNS may even resolve it - but the server
	/// rejects one at startup for its own cast list, because `tls_connect()`
	/// calls `inet_pton` and never `getaddrinfo`. Keeping the same shape means
	/// the two lists describe devices the same way.
	var address: String
	var name: String
	var port: UInt16

	init(id: UUID = UUID(), address: String, name: String, port: UInt16 = 8009) {
		self.id = id
		self.address = address
		self.name = name
		self.port = port
	}

	/// Spelled exactly as the server spells it in `listCastDevices` - derived
	/// rather than random, so it survives a restart and so the two describe the
	/// same device by the same name.
	var derivedCastId: String { "manual:\(address):\(port)" }

	var device: CastDevice {
		CastDevice(
			id: derivedCastId,
			name: name.isEmpty ? address : name,
			endpoint: .host(address, port: port),
			model: nil,
			// A configured device announces nothing, and the server reads that
			// as *capable* - refusing the picture on a guess is worse than the
			// guess. Nil says "it never said", which is the same thing.
			videoOut: nil,
			address: address)
	}
}

/// The configured devices, persisted.
///
/// A `Codable` array through `JSONEncoder` into `UserDefaults`, which is what
/// `ServerRegistry` does with the server list and is the right shape for a short
/// list of things a person configured. (`Pins` writes a JSON file instead,
/// because it grows with the library; this cannot.)
@MainActor
@Observable
final class CastDeviceStore {
	private(set) var devices: [ManualCastDevice] = []

	@ObservationIgnored private let defaults: UserDefaults
	private static let storageKey = "cast_devices"

	init(defaults: UserDefaults = .standard) {
		self.defaults = defaults
		devices = Self.load(from: defaults)
	}

	func save(_ device: ManualCastDevice) {
		if let index = devices.firstIndex(where: { $0.id == device.id }) {
			devices[index] = device
		} else {
			devices.append(device)
		}
		persist()
	}

	func remove(id: UUID) {
		devices.removeAll { $0.id == id }
		persist()
	}

	func remove(atOffsets offsets: IndexSet) {
		devices.remove(atOffsets: offsets)
		persist()
	}

	/// The configured devices a discovered one does not already stand for.
	///
	/// **Deduplicated by address, discovery winning** - as on Android, and for
	/// its reason: a discovered entry carries the friendly name from the
	/// device's own `fn` record and its real Cast id, while a configured one has
	/// only what somebody typed. So an entry left behind after a device starts
	/// announcing itself again stops showing twice by itself, with no
	/// housekeeping asked of anyone.
	///
	/// **The address, not the Cast id**, although the id is what the two lists
	/// are keyed on elsewhere: a configured device has no announcement, so its
	/// real Cast id is exactly the thing that cannot be known about it. The
	/// address is the only fact both sides can hold - and a discovered device
	/// only acquires one once something has connected to it, since `NWBrowser`
	/// reports a service. Until then a device that is both configured and
	/// announcing appears twice, which is untidy and not wrong.
	func notDiscovered(among discovered: [CastDevice]) -> [ManualCastDevice] {
		let addresses = Set(discovered.compactMap(\.address))
		return devices.filter { !addresses.contains($0.address) }
	}

	// MARK: - Persistence

	private func persist() {
		guard let data = try? JSONEncoder().encode(devices) else { return }
		defaults.set(data, forKey: Self.storageKey)
	}

	/// A malformed document yields an empty list rather than a crash, the rule
	/// `ServerRegistry.load` states: the person sees no configured devices and
	/// can add one, which is recoverable, and a launch-time trap is not.
	private static func load(from defaults: UserDefaults) -> [ManualCastDevice] {
		guard let data = defaults.data(forKey: storageKey),
			let decoded = try? JSONDecoder().decode([ManualCastDevice].self, from: data)
		else {
			return []
		}
		return decoded
	}
}

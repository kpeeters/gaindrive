//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Network
import OSLog

/// Cast receivers announcing themselves on the local network.
///
/// **`NWBrowser`, and no resolve step.** Android's `CastDiscovery` carries a
/// serialised resolve queue, a five-second timeout and a workaround for
/// "listener already in use", all because `NsdManager.resolveService` cannot be
/// called concurrently; Network.framework resolves a Bonjour service when
/// something connects to it, so a browse result *is* an endpoint and none of
/// that machinery is needed.
///
/// The consequence is that a discovered device has **no address until something
/// connects to it** - `CastDevice.address` is filled in by `CastProbe`, and by
/// the session later. That is a fair trade: the address is diagnostic rather
/// than functional, and buying it here would mean a TCP connection to every
/// device every time the browser sees one.
///
/// `@MainActor @Observable`, the shape `ServerRegistry` uses: it feeds a SwiftUI
/// list and nothing else.
@MainActor
@Observable
final class CastDiscovery {
	/// Sorted by name, so the list does not reorder as devices answer.
	private(set) var devices: [CastDevice] = []

	/// What the browser last said about itself.
	///
	/// **Denial is a state, not a timeout**, which is the rule adopted
	/// for the Local Network permission and the one thing about this feature
	/// that has no Android counterpart at all. A denied prompt and a network
	/// with no receivers on it both produce an empty list, and only one of them
	/// is worth telling somebody about.
	private(set) var state: State = .idle

	enum State: Equatable {
		case idle
		case browsing
		/// The browser failed outright. On iOS a refused Local Network
		/// permission *can* arrive this way - as `NWError.dns(-65570)`,
		/// `kDNSServiceErr_PolicyDenied` - but see `quiet` below: it is not the
		/// shape that failure usually takes.
		case failed(String)
	}

	/// Browsing has been running a while and has found nothing.
	///
	/// **This is what a refused Local Network permission actually looks like**,
	/// and it is why the `failed` state above is not enough on its own. Denied,
	/// `NWBrowser` generally reaches `.ready` and then reports zero results for
	/// ever; it sometimes fails with `kDNSServiceErr_PolicyDenied` instead, but
	/// never while the prompt is still pending, and not dependably afterwards.
	/// There is **no API to ask whether the permission was granted**, so a
	/// silence that has gone on too long is the only signal available.
	///
	/// It says nothing certain, and must not: a network with no receivers on it
	/// looks identical. What it does is put the one thing worth checking in
	/// front of somebody who would otherwise watch a spinner.
	///
	/// Neither the simulator nor a Catalyst build launched from Xcode enforces
	/// the permission - the first not at all, the second under Xcode's own
	/// grant - so this path is reachable only on a real device.
	private(set) var quiet = false

	/// How long counts as too long. Generous: a receiver waking from standby is
	/// slow to announce, and crying wolf at somebody whose device was merely
	/// asleep is worse than a few more seconds of spinner.
	static let quietAfter = Duration.seconds(8)

	@ObservationIgnored private var browser: NWBrowser?
	@ObservationIgnored private var found: [String: CastDevice] = [:]
	@ObservationIgnored private var quietTimer: Task<Void, Never>?

	/// Bonjour's own spelling. `NSBonjourServices` in `Info.plist` must name the
	/// same string or the browser finds nothing and says nothing.
	nonisolated static let serviceType = "_googlecast._tcp"

	// `nonisolated` because the TXT parsing below runs off the browser's
	// callback, before anything has hopped to the main actor. `Logger` is
	// `Sendable`, so this is a statement of fact rather than an escape hatch.
	nonisolated private static let log = Logger(subsystem: "org.gaindrive.ios", category: "cast")

	// MARK: - Lifecycle

	/// Browsing runs only while something is looking at the list - multicast is
	/// not free, and the picker is the only consumer.
	func start() {
		guard browser == nil else { return }
		let parameters = NWParameters()
		parameters.includePeerToPeer = false
		let browser = NWBrowser(
			for: .bonjourWithTXTRecord(type: Self.serviceType, domain: nil), using: parameters)

		browser.stateUpdateHandler = { [weak self] state in
			Task { @MainActor in self?.browserChanged(state) }
		}
		browser.browseResultsChangedHandler = { [weak self] results, _ in
			// The whole result set every time, so this rebuilds rather than
			// diffing - which is also what makes a device disappearing work.
			let devices = results.compactMap(Self.device(from:))
			Task { @MainActor in self?.replace(with: devices) }
		}
		self.browser = browser
		state = .browsing
		quiet = false
		browser.start(queue: .main)

		quietTimer = Task { [weak self] in
			try? await Task.sleep(for: Self.quietAfter)
			guard !Task.isCancelled else { return }
			self?.noticeSilence()
		}
	}

	func stop() {
		quietTimer?.cancel()
		quietTimer = nil
		browser?.stateUpdateHandler = nil
		browser?.browseResultsChangedHandler = nil
		browser?.cancel()
		browser = nil
		state = .idle
		quiet = false
	}

	/// Tears the browser down and starts it again.
	///
	/// **iOS suspends the app and Mac Catalyst does not**, so a browser is dead
	/// after a background round trip on one platform and fine on the other. The
	/// view cannot notice with `onAppear`, which does not fire again for a view
	/// that never left the screen - it watches the scene phase and calls this.
	func restart() {
		stop()
		start()
	}

	private func noticeSilence() {
		guard case .browsing = state, devices.isEmpty else { return }
		quiet = true
	}

	/// Records the address a connection reached, so a device that has been
	/// probed can be told apart from a manual entry naming the same box.
	func note(address: String, for id: String) {
		guard var device = found[id], device.address != address else { return }
		device.address = address
		found[id] = device
		publish()
	}

	// MARK: - Machinery

	private func browserChanged(_ state: NWBrowser.State) {
		switch state {
		case .ready:
			self.state = .browsing
		case .failed(let error):
			// Not `.cancelled`, which is `stop()` doing its job.
			Self.log.warning("cast browse failed: \(String(describing: error))")
			self.state = .failed(
				"Could not look for devices. Check that GainDrive is allowed to "
					+ "find devices on your local network.")
		case .setup, .waiting, .cancelled:
			break
		@unknown default:
			break
		}
	}

	private func replace(with devices: [CastDevice]) {
		// Addresses learned by probing are kept: they came from a connection
		// this browser cannot make, and losing them on every announcement would
		// make them flicker.
		found = devices.reduce(into: [:]) { table, device in
			var device = device
			device.address = found[device.id]?.address ?? device.address
			table[device.id] = device
		}
		publish()
	}

	private func publish() {
		devices = found.values.sorted {
			$0.name.localizedCaseInsensitiveCompare($1.name) == .orderedAscending
		}
		// One device arriving answers the question the hint was raising, and a
		// list that then empties again - a receiver switched off - must not
		// bring back a permission warning that has been disproved.
		if !devices.isEmpty {
			quiet = false
			quietTimer?.cancel()
			quietTimer = nil
		}
	}

	// MARK: - Reading a browse result

	/// **The TXT record is the whole of what a device tells us about itself**,
	/// and it arrives at the instant the device does, at no extra request:
	///
	/// * `fn` - the friendly name somebody set on the device. The service name
	///   is a serial-number-ish string nobody would recognise, so it is only the
	///   fallback: a row must never be labelled by something meaningless, which
	///   for Android was an IP address and here would be a hex blob.
	/// * `id` - the Cast id, stable across a rename, which the service name is
	///   not. It is the deduplication key, matching what the server's own
	///   `listCastDevices` returns one entry per.
	/// * `md` - the model the receiver announces for itself, and the only thing
	///   on the wire that tells a WiiM from a television. See `CastDeviceKind`.
	/// * `ca` - capabilities. **Bit 0 is video output**, which the server reads
	///   to send a film's soundtrack to a screenless receiver and which Android
	///   does not read at all, so casting a film from that phone to a WiiM loses
	///   the picture silently. Nothing acts on it here yet; it is parsed now
	///   because a second pass over this record later is not the natural place
	///   for it.
	nonisolated private static func device(from result: NWBrowser.Result) -> CastDevice? {
		guard case .service(let name, let type, let domain, _) = result.endpoint else {
			return nil
		}
		guard case .bonjour(let record) = result.metadata else {
			// A browser asked for TXT records and given none has found something
			// that is not a Cast receiver, or has found it before it finished
			// announcing. Either way there is nothing to show yet.
			return nil
		}
		let text = record.dictionary
		// The whole record, once per appearance. This is not leftover debugging:
		// nothing else reports what a given make of receiver actually announces,
		// and every value the parsers key on is guesswork until it has been read
		// off the device in the room. `src/casttool.cc` dumps the same thing
		// from a machine on that network.
		log.info("cast discovered \(name, privacy: .public): \(String(describing: text), privacy: .public)")

		return CastDevice(
			id: value(text, "id") ?? name,
			name: value(text, "fn") ?? name,
			endpoint: .service(name: name, type: type, domain: domain),
			model: value(text, "md"),
			videoOut: videoOut(value(text, "ca")))
	}

	/// A TXT value may be absent, or present and empty; both are "no".
	nonisolated private static func value(_ text: [String: String], _ key: String) -> String? {
		guard let found = text[key]?.trimmingCharacters(in: .whitespacesAndNewlines),
			!found.isEmpty
		else {
			return nil
		}
		return found
	}

	/// Bit 0 of the decimal `ca` bitfield.
	///
	/// **Nil for a device that announced nothing**, which is not the same as
	/// false: the server treats an absent record as *capable*, because refusing
	/// the picture on a guess is worse than the guess, and a client that read it
	/// as false would silently drop the picture on every receiver whose firmware
	/// omits the record.
	nonisolated private static func videoOut(_ capabilities: String?) -> Bool? {
		guard let capabilities, let bits = Int(capabilities) else { return nil }
		return bits & 1 != 0
	}
}

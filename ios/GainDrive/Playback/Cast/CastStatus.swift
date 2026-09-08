//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Where a receiver is, in the form something can connect to.
///
/// Two cases because there are two ways to learn about a device, and
/// Network.framework takes either directly — **a discovered one needs no
/// resolution step at all**, which is most of what this port saves over
/// Android's. There, `NsdManager.resolveService` cannot be called concurrently,
/// so `CastDiscovery` carries a serialised resolve queue, a five-second timeout
/// and a workaround for "listener already in use"; here the browse result *is*
/// the endpoint.
enum CastEndpoint: Hashable, Sendable, Codable {
	case service(name: String, type: String, domain: String?)
	case host(String, port: UInt16)
}

/// A Cast receiver on the network.
///
/// `model` is the mDNS `md` record — the model name the receiver announces for
/// itself, `Chromecast` or `WiiM Pro` or a television's marketing name. It is
/// nil for a manually added device, which has no announcement to read.
struct CastDevice: Identifiable, Hashable, Sendable {
	/// The Cast id from the `id` TXT record, or `manual:<address>:<port>` for a
	/// configured one — **derived rather than random**, and spelled exactly as
	/// the server spells it in `listCastDevices`, so the two describe the same
	/// device by the same name.
	let id: String
	let name: String
	let endpoint: CastEndpoint
	var model: String? = nil
	/// Whether the device can show a picture, from **bit 0 of the `ca` record**.
	///
	/// `nil` means it announced nothing, which is every manually added device.
	/// The server treats that as *true* — refusing the picture on a guess is
	/// worse than the guess — and so must anything here that acts on it.
	///
	/// Nothing acts on it yet; it is read now because the parser is the natural
	/// place for it and a second pass over the TXT record later is not. See
	/// "Casting a video to a receiver that cannot show one" in the root
	/// `CLAUDE.md` for what the server does with the same bit.
	var videoOut: Bool? = nil
	/// Filled in once something has actually connected — `NWBrowser` reports a
	/// service, not an address. It is diagnostic rather than functional: nothing
	/// connects by it, but "which box did I just reach" is the question a device
	/// that half-works raises, and nothing else can answer it.
	var address: String? = nil

	var kind: CastDeviceKind { castDeviceKind(model) }
}

/// What the receiver last said it was doing.
///
/// `unknown` exists so a state we have never seen cannot be silently read as
/// `idle`, which is the one value that drives both the LOAD retry and the queue
/// advance.
enum CastPlayerState: String, Sendable {
	case idle, playing, paused, buffering, loading, unknown

	static func from(_ wire: String?) -> CastPlayerState {
		switch wire {
		case "IDLE", nil: return .idle
		case "PLAYING": return .playing
		case "PAUSED": return .paused
		case "BUFFERING": return .buffering
		case "LOADING": return .loading
		default: return .unknown
		}
	}
}

/// A parsed `MEDIA_STATUS`, mirroring `CastManager::CastStatus` in
/// `src/castmanager.cc`.
///
/// `idleReason` is only meaningful while idle, and only two values matter here:
/// `FINISHED` advances the queue, `ERROR` triggers the LOAD retry.
struct CastStatus: Hashable, Sendable {
	var playerState: CastPlayerState = .idle
	var currentTime: Double = 0
	var duration: Double = 0
	var mediaSessionId: Int = 0
	var idleReason: String?
	/// Which subtitle tracks the receiver has on, by `trackId`.
	///
	/// **Optional, and nil is not empty.** The receiver states this when the
	/// selection changes and omits it from the position pushes in between,
	/// exactly as it does with `duration` — so reading an absent field as "none
	/// selected" would make a caption picker's tick flicker off once a second.
	/// The session carries the last stated value forward. An empty array *is* a
	/// statement, and it means subtitles were turned off.
	var activeTrackIds: [Int]?

	var isIdleError: Bool { playerState == .idle && idleReason == "ERROR" }
	var isIdleFinished: Bool { playerState == .idle && idleReason == "FINISHED" }

	/// The receiver is doing something with our media, whatever it is.
	var isLive: Bool {
		playerState == .playing || playerState == .buffering || playerState == .loading
	}

	// MARK: - Parsing
	//
	//	Everything below reaches into JSON that came off the network from a
	//	device we do not control, so it goes through the tolerant accessors at
	//	the foot of this file rather than through `Codable`. That is the same
	//	rule `src/jsonread.hh` states for the server's provider responses, and
	//	for the same reason: a malformed push must cost one message, not the
	//	receive loop.

	/// Reads the first entry of a `MEDIA_STATUS`, or nil if the message carries
	/// no status at all.
	///
	/// The receiver sends a list, but it holds one entry for the single-item
	/// sessions we load.
	static func parse(_ message: [String: Any]) -> CastStatus? {
		guard let entry = message.array("status")?.first as? [String: Any] else { return nil }
		return CastStatus(
			playerState: CastPlayerState.from(entry.string("playerState")),
			currentTime: entry.number("currentTime") ?? 0,
			// A push during playback omits `media` entirely — the receiver only
			// repeats it when the item changes — so a zero here means "not
			// stated", not "zero seconds", and the session carries the last
			// known value forward.
			duration: entry.object("media")?.number("duration") ?? 0,
			mediaSessionId: entry.int("mediaSessionId") ?? 0,
			idleReason: entry.string("idleReason"),
			activeTrackIds: entry.array("activeTrackIds")?.compactMap { $0 as? Int })
	}

	/// The `transportId` of a running instance of `appId`, from a
	/// `RECEIVER_STATUS`. Nil means it is not running, which is the signal to
	/// LAUNCH it.
	static func transportId(in message: [String: Any], appId: String) -> String? {
		runningApp(in: message, appId: appId)?.string("transportId")
	}

	/// The `sessionId` of that same application, needed to STOP it.
	static func sessionId(in message: [String: Any], appId: String) -> String? {
		runningApp(in: message, appId: appId)?.string("sessionId")
	}

	/// **Matched on `appId` rather than taken as the first entry, which is a fix
	/// and not a refinement.**
	///
	/// A television that has been sitting idle is running its own ambient app —
	/// `E8C28D3C`, "Backdrop" — and it publishes a `transportId` like any
	/// other. Taking the first one makes that look like a media receiver ready
	/// to be loaded into, so the LOAD goes to a screensaver, which ignores the
	/// media namespace entirely: no `MEDIA_STATUS`, no fetch, no error, and
	/// nothing in any log. A player that simply does nothing.
	///
	/// It survived a long time on Android because whether it bites depends on
	/// what the television happened to be showing when somebody reached for it:
	/// a truly idle receiver reports no `applications` at all, and then
	/// everything works.
	private static func runningApp(in message: [String: Any], appId: String) -> [String: Any]? {
		message.object("status")?.array("applications")?
			.compactMap { $0 as? [String: Any] }
			.first { $0.string("appId") == appId }
	}

	/// A message's `type`, which is how every namespace says what it sent.
	static func type(of message: [String: Any]) -> String? { message.string("type") }
}

//	── Reading JSON somebody else wrote ────────────────────────────────────────
//
//	`JSONSerialization` rather than `Codable`, because these messages are
//	heterogeneous and only ever read a field at a time — a `Decodable` shape per
//	message type would be a dozen structs to reach six values. Every accessor
//	answers nil rather than throwing, which is the whole point.

/// Parses one payload. Nil for anything that is not a JSON object, which covers
/// a truncated frame and a device answering something else entirely.
func castJSON(_ text: String) -> [String: Any]? {
	guard let data = text.data(using: .utf8),
		let any = try? JSONSerialization.jsonObject(with: data)
	else {
		return nil
	}
	return any as? [String: Any]
}

extension Dictionary where Key == String, Value == Any {
	/// Non-empty strings only. An empty `transportId` or `appId` is not an
	/// answer, and treating it as one would send a LOAD to nowhere.
	func string(_ key: String) -> String? {
		guard let value = self[key] as? String, !value.isEmpty else { return nil }
		return value
	}

	func number(_ key: String) -> Double? {
		// A JSON number arrives as `NSNumber`, which bridges to `Double` — but
		// so does a JSON *boolean*, so `true` would read as 1. Nothing here
		// wants a number where a bool may appear, and being explicit costs
		// nothing.
		guard let value = self[key] as? NSNumber else { return nil }
		return value.doubleValue
	}

	func int(_ key: String) -> Int? {
		guard let value = self[key] as? NSNumber else { return nil }
		return value.intValue
	}

	func object(_ key: String) -> [String: Any]? { self[key] as? [String: Any] }

	func array(_ key: String) -> [Any]? { self[key] as? [Any] }
}

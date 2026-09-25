//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import os

/// The WiiM's private HTTP API: the equalizer, which the Cast protocol has no
/// message for. A port of Android's `WiiMClient.kt`.
///
/// **Not on the casting path.** It knows no session, no queue and no music
/// server, so being wrong about a device cannot break playback to it.
///
/// **Its own `URLSession`, never `HTTP.shared` or `HTTP.media`.** Those carry
/// nothing extra today, but the Subsonic client is where the music account's
/// credentials are added, and a speaker must never be the one place that
/// changes by accident. It also needs a trust rule nothing else should have.
///
/// **The LinkPlay certificate is self-signed** (`CN=www.linkplay.com`, the same
/// on every device), so an ordinary request fails at the handshake. The
/// delegate accepts whatever the device presents, for this session only: what
/// authenticates the exchange is that the user picked this device off their
/// own network, and nothing secret is sent to it. Accepting the trust object
/// also skips the hostname check, which a certificate for `www.linkplay.com`
/// would fail against a bare address. Pinning that one shared certificate was
/// rejected on Android for a reason that holds here: it proves "a LinkPlay
/// device" and not "this device", and every WiiM would stop working the day it
/// is rotated.
///
/// No Wi-Fi binding and no second attempt, unlike Android: that dance exists
/// for a full-tunnel VPN that forbids binding sockets, and `URLSession` has no
/// binding to forbid.
final class WiiMClient: Sendable {
	static let shared = WiiMClient()

	private let session: URLSession
	private static let log = Logger(subsystem: "org.gaindrive.ios-player", category: "wiim")

	init() {
		let config = URLSessionConfiguration.ephemeral
		// A device on the LAN answers at once or not at all; the defaults
		// would leave a sheet spinning for a minute over one that is off.
		config.timeoutIntervalForRequest = 5
		config.timeoutIntervalForResource = 10
		config.waitsForConnectivity = false
		session = URLSession(configuration: config, delegate: TrustAnyServer(), delegateQueue: nil)
	}

	enum Failure: LocalizedError {
		case noState
		case refused(Int)
		case badCommand

		var errorDescription: String? {
			switch self {
			case .noState: "The device did not report its equalizer state."
			case .refused(let code): "The device answered HTTP \(code)."
			case .badCommand: "That command could not be built."
			}
		}
	}

	/// `EQGetBand`, falling back to `EQGetStat`. Throws when neither answers,
	/// which is what makes this the reachability test.
	func eqState(address: String) async throws -> WiiMEqState {
		let body = try? await get(address, WiiMEq.getBand)
		if let body, let state = WiiMEq.parseBand(body) {
			// What was read, and not only failures: the reading and the writing
			// share one screen, so a write that lies looks exactly like a read
			// that fails, and this line is what tells them apart.
			Self.log.info(
				"EQGetBand at \(address, privacy: .public): enabled=\(state.enabled) preset=\(state.preset ?? "-", privacy: .public)")
			return state
		}
		Self.log.info(
			"EQGetBand unusable at \(address, privacy: .public), trying EQGetStat: \(Self.clip(body), privacy: .public)")
		guard let enabled = WiiMEq.parseStat(try await get(address, WiiMEq.getStat)) else {
			throw Failure.noState
		}
		return WiiMEqState(enabled: enabled, preset: nil, bands: nil)
	}

	/// Never throws: `eqState` has already answered whether the device is
	/// there, so a failure here is about the list, and the documented presets
	/// are a better sheet than an empty one.
	func presets(address: String) async -> [String] {
		let body = try? await get(address, WiiMEq.getList)
		if let body, let listed = WiiMEq.parsePresets(body) { return listed }
		// With the body, because the fallback is silent on screen: the owner's
		// own presets would simply be missing.
		Self.log.info(
			"EQGetList unusable at \(address, privacy: .public), using the documented list: \(Self.clip(body), privacy: .public)")
		return WiiMEq.documentedPresets
	}

	func loadPreset(address: String, _ preset: String) async throws -> Bool {
		try await command(address, WiiMEq.load(preset))
	}

	func setEnabled(address: String, _ enabled: Bool) async throws -> Bool {
		try await command(address, enabled ? WiiMEq.on : WiiMEq.off)
	}

	func setBands(address: String, _ levels: [Int]) async throws -> Bool {
		guard let command = WiiMEq.setBand(levels) else { throw Failure.badCommand }
		return try await self.command(address, command)
	}

	private func command(_ address: String, _ command: String) async throws -> Bool {
		let body = try await get(address, command)
		let ok = WiiMEq.isOk(body)
		// A third success shape would otherwise cost another round of guessing.
		if !ok {
			Self.log.warning(
				"\(command, privacy: .public) at \(address, privacy: .public) answered: \(Self.clip(body), privacy: .public)")
		}
		return ok
	}

	private func get(_ address: String, _ command: String) async throws -> String {
		guard let url = WiiMEq.url(address: address, command: command) else {
			throw Failure.badCommand
		}
		let (data, response) = try await session.data(from: url)
		if let http = response as? HTTPURLResponse, !(200..<300).contains(http.statusCode) {
			throw Failure.refused(http.statusCode)
		}
		return String(decoding: data, as: UTF8.self)
	}

	private static func clip(_ body: String?) -> String {
		guard let body else { return "(no answer)" }
		return String(body.trimmingCharacters(in: .whitespacesAndNewlines).prefix(300))
	}
}

/// Accepts the server's certificate, for `WiiMClient`'s session alone. See
/// the class note there for why that is sound for this device and nothing
/// else.
/// `@unchecked` only because `NSObject` is not declared `Sendable`; it holds
/// no state at all.
private final class TrustAnyServer: NSObject, URLSessionDelegate, @unchecked Sendable {
	func urlSession(
		_ session: URLSession, didReceive challenge: URLAuthenticationChallenge
	) async -> (URLSession.AuthChallengeDisposition, URLCredential?) {
		guard challenge.protectionSpace.authenticationMethod == NSURLAuthenticationMethodServerTrust,
			let trust = challenge.protectionSpace.serverTrust
		else { return (.performDefaultHandling, nil) }
		return (.useCredential, URLCredential(trust: trust))
	}
}

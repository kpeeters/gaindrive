//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import OSLog

/// What asking a device whether it is there produced.
///
/// **Three outcomes rather than a boolean**, on the reasoning `ConnectionTester`
/// already gives for servers: the three want different advice, and collapsing
/// them means the one piece of information the person needs is the one thrown
/// away.
enum CastProbeResult: Equatable, Sendable {
	/// It spoke Cast. `runningApp` is whatever the television is showing right
	/// now - often its own ambient app rather than anything of ours - and
	/// `address` is what the connection actually reached.
	case answered(runningApp: String?, address: String?)
	/// Something accepted a TLS connection on the cast port and then said
	/// nothing. Kept distinct from `unreachable` because the advice differs: the
	/// address is live and reachable, but whatever is there is not a Cast
	/// receiver.
	case silent
	/// No control channel at all - wrong address, device off, or blocked.
	case unreachable
}

/// Asks one device whether it is there, without disturbing anything.
///
/// **It must stay separate from the session that arrives in stage 3**, and that
/// is a rule rather than a preference. A session publishes the device it was
/// given and the player swaps onto it, so testing an address somebody has merely
/// typed would start casting to it. This runs the same opening exchange over its
/// own channel and throws the channel away.
///
/// It is also what proves, on real hardware, the one assumption everything
/// downstream rests on: that Network.framework will complete a TLS handshake
/// with a receiver whose certificate chains to a root the system does not trust.
/// See `CastChannel.parameters()`.
struct CastProbe: Sendable {
	/// Generous: the channel's own connect can already have spent several
	/// seconds, and a receiver waking from standby answers slowly.
	static let answerTimeout = Duration.seconds(6)

	private static let log = Logger(subsystem: "org.gaindrive.ios", category: "cast")

	/// Connect, ask for a receiver status, report what came back.
	///
	/// This exercises TCP reachability, the TLS handshake and the Cast framing -
	/// which are the three things a wrong address fails at, in that order.
	func probe(_ device: CastDevice) async -> CastProbeResult {
		let channel: CastChannel
		do {
			channel = try await CastChannel.open(to: device.endpoint)
		} catch {
			Self.log.info("cast probe \(device.name, privacy: .public): no channel")
			return .unreachable
		}
		defer { Task { await channel.close() } }

		let address = await channel.remoteAddress
		do {
			// The virtual connection to the platform must exist before anything
			// else is accepted - the same order the session's loop uses.
			try await channel.send(
				namespace: CastNamespace.connection, destination: CastNamespace.receiverId,
				payload: #"{"type":"CONNECT"}"#)
			try await channel.send(
				namespace: CastNamespace.receiver, destination: CastNamespace.receiverId,
				payload: #"{"type":"GET_STATUS","requestId":1}"#)
		} catch {
			Self.log.info("cast probe \(device.name, privacy: .public): connected but refused a write")
			return .silent
		}

		guard let answer = await awaitReceiverStatus(on: channel) else {
			Self.log.info("cast probe \(device.name, privacy: .public): connected but silent")
			return .silent
		}
		Self.log.info("cast probe \(device.name, privacy: .public): RECEIVER_STATUS received")
		return .answered(runningApp: answer.runningApp, address: address)
	}

	/// What the receiver said, reduced to the one fact worth reporting.
	///
	/// A `Sendable` value rather than the parsed message, so nothing
	/// non-`Sendable` crosses back out of this function - the same reason
	/// `CastChannel.receive()` hands back a `String` and lets its caller parse.
	private struct ReceiverAnswer: Sendable {
		let runningApp: String?
	}

	/// Reads until the receiver answers, keeping the heartbeat alive meanwhile.
	/// Nil means it never did.
	///
	/// **The deadline closes the channel rather than cancelling a task.**
	/// `CastChannel.receive()` waits indefinitely on an `NWConnection`, so
	/// abandoning the wait would leave the read outstanding; closing is what
	/// makes it return, and this is the caller the channel's documentation means
	/// when it says a deadline is the caller's to impose. Android has the
	/// mirror-image problem for the mirror-image reason - its read blocks a
	/// thread, so a `withTimeoutOrNull` around the loop finds no suspension
	/// point to cancel at.
	private func awaitReceiverStatus(on channel: CastChannel) async -> ReceiverAnswer? {
		let deadline = Task {
			try? await Task.sleep(for: Self.answerTimeout)
			guard !Task.isCancelled else { return }
			await channel.close()
		}
		defer { deadline.cancel() }

		while true {
			// **Two different nils, kept apart.** `receive()` throws when the
			// connection has gone, and answers nil for a frame that carried
			// nothing readable - which is not fatal and is simply skipped.
			// Folding both into `try?` would end the probe on the first frame
			// we did not understand.
			let received: String?
			do {
				received = try await channel.receive()
			} catch {
				return nil
			}
			guard let payload = received, let message = castJSON(payload) else { continue }
			switch CastStatus.type(of: message) {
			case "RECEIVER_STATUS":
				return ReceiverAnswer(runningApp: Self.displayName(in: message))
			case "PING":
				// A probe is short enough that a missed PONG would not drop the
				// connection, but answering costs one write and keeps this
				// identical to what the session does.
				try? await channel.send(
					namespace: CastNamespace.heartbeat, destination: CastNamespace.receiverId,
					payload: #"{"type":"PONG"}"#)
			default:
				continue
			}
		}
	}

	/// What the television is showing.
	///
	/// Unlike `CastStatus.transportId(in:appId:)` this deliberately takes the
	/// **first** application rather than matching on our own app id: an idle
	/// receiver is running its ambient app, and naming that is more informative
	/// to somebody testing an address than reporting nothing at all.
	private static func displayName(in message: [String: Any]) -> String? {
		message.object("status")?.array("applications")?
			.compactMap { ($0 as? [String: Any])?.string("displayName") }
			.first
	}
}

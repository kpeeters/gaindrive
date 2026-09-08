//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Network
import Security

/// A TLS connection to a Cast receiver's control port, framing Cast messages in
/// both directions.
///
/// Ported from the `Tls` struct and `cast_send`/`cast_recv` in
/// `src/castmanager.cc` by way of Android's `CastChannel.kt`, with the same
/// deliberate change the Kotlin made: the C++ opens a fresh connection for every
/// command, because a detached thread each was the simplest thing there. Here a
/// single connection is multiplexed — which is what pychromecast and node-castv2
/// do — so commands cost no handshake and cannot race a reconnect.
///
/// **An actor, with its `NWConnection` confined to it.** This is the app's first
/// socket and it lands under `SWIFT_STRICT_CONCURRENCY: complete`, so the
/// isolation is decided here rather than per call site: it is I/O and has no
/// business on the main actor, and every published value that leaves is a
/// `String` or a `Bool`.
actor CastChannel {
	enum Failure: Error {
		/// Nothing accepted a connection, or the handshake did not complete.
		case unreachable
		/// The connection went away, or the stream is out of step with the
		/// length prefix and there is no way to resync.
		case closed
	}

	/// The address the connection actually reached.
	///
	/// Diagnostic rather than functional — nothing connects by it. It exists
	/// because `NWBrowser` reports a *service*, so until something connects the
	/// app does not know which box on the network a name refers to, and "which
	/// one did I just reach" is the question a half-working device raises.
	private(set) var remoteAddress: String?

	/// The same thing, as something to connect to again.
	///
	/// **A reconnect should not re-resolve a name.** `CastEndpoint.service`
	/// costs an mDNS lookup every time it is used, and the responders this app
	/// has to deal with are exactly the ones that answer unreliably — a WiiM's
	/// had stopped answering even a direct unicast query, which is why
	/// configured devices exist at all. Once a connection has been made, the
	/// address it reached is a better thing to reconnect to than the name it
	/// started from.
	private(set) var resolvedEndpoint: CastEndpoint?

	/// Confined to this actor, which is what the rest of the file relies on.
	///
	/// **No `nonisolated(unsafe)` is needed**, and that was worth finding out
	/// rather than assuming: Network.framework annotates `NWConnection` as
	/// `Sendable`, so the deadline task below can capture it and the compiler
	/// needs no promise from us. Everything that touches it does so from an
	/// isolated method regardless.
	private let connection: NWConnection
	private var closed = false

	private static let queue = DispatchQueue(label: "org.gaindrive.cast", qos: .userInitiated)

	private init(_ connection: NWConnection) {
		self.connection = connection
	}

	// MARK: - Opening

	/// Opens the control channel, or throws.
	///
	/// **Whether this succeeds at all is the question stage 1 of the cast work
	/// exists to answer**, and it turns on `verifyBlock` below.
	///
	/// Note there is no Wi-Fi binding here, and its absence is a decision rather
	/// than an omission. Android must try a Wi-Fi-bound socket first, because
	/// under a full-tunnel VPN everything addressed to the LAN over the default
	/// route goes into the tunnel and dies — and must then fall back to an
	/// unbound one, because a `VpnService` that has not called `allowBypass()`
	/// refuses the binding outright with `EPERM`. Nothing here binds, so neither
	/// half applies. `NWParameters.requiredInterfaceType = .wifi` is the lever if
	/// a tunnel ever does swallow the LAN; it is written down rather than used,
	/// because a preference nobody has needed is a branch nobody has tested.
	static func open(to endpoint: CastEndpoint, timeout: Duration = .seconds(5)) async throws
		-> CastChannel
	{
		let connection = NWConnection(to: endpoint.nwEndpoint, using: parameters())
		let channel = CastChannel(connection)
		try await channel.start(timeout: timeout)
		return channel
	}

	private func start(timeout: Duration) async throws {
		// **Cancelling is what makes the wait below return.** An `NWConnection`
		// has no deadline of its own past the TCP connect, so a receiver that
		// accepts a socket and then says nothing would hang here for ever;
		// cancelling drives the state to `.cancelled`, which `awaitReady` reads
		// as unreachable.
		//
		// The connection is captured rather than `self`, which keeps the task
		// clear of any question about actor identity — and it is the connection
		// this needs to reach anyway. `close()` does the same thing plus a flag
		// that only refuses later writes.
		let connection = self.connection
		let deadline = Task {
			try? await Task.sleep(for: timeout)
			guard !Task.isCancelled else { return }
			connection.cancel()
		}
		defer { deadline.cancel() }

		connection.start(queue: Self.queue)
		do {
			try await awaitReady()
		} catch {
			close()
			throw error
		}
		let resolved = Self.resolved(connection)
		remoteAddress = resolved?.host
		resolvedEndpoint = resolved.map { .host($0.host, port: $0.port) }
	}

	private func awaitReady() async throws {
		try await withCheckedThrowingContinuation {
			(continuation: CheckedContinuation<Void, any Error>) in
			// Resumed exactly once: `OneShot` takes the continuation before
			// resuming, so a later state change cannot resume it a second time —
			// which traps rather than erring.
			let resume = OneShot(continuation)
			connection.stateUpdateHandler = { state in
				switch state {
				case .ready:
					resume.succeed()
				case .failed, .cancelled:
					resume.fail(Failure.unreachable)
				// `.waiting` is not a failure: the interface may still be coming
				// up, and the connection retries by itself. The deadline above
				// is what bounds it.
				case .setup, .preparing, .waiting:
					break
				@unknown default:
					break
				}
			}
		}
	}

	// MARK: - Sending

	/// Frames and writes one message. Serialised by actor isolation, which is
	/// what the Kotlin needs an explicit write lock for.
	func send(namespace: String, destination: String, payload: String) async throws {
		guard !closed else { throw Failure.closed }
		let bytes = CastMessage.frame(
			CastMessage.body(
				namespace: namespace, source: CastNamespace.sender,
				destination: destination, payload: payload))
		try await withCheckedThrowingContinuation {
			(continuation: CheckedContinuation<Void, any Error>) in
			let resume = OneShot(continuation)
			connection.send(
				content: bytes,
				completion: .contentProcessed { error in
					if error != nil { resume.fail(Failure.closed) } else { resume.succeed() }
				})
		}
	}

	// MARK: - Receiving

	/// One message's `payload_utf8`, or nil for a frame that carried nothing
	/// readable — which is not fatal and the caller simply skips.
	///
	/// **Waits indefinitely, deliberately.** The receiver only pushes on a state
	/// change, so quiet is the normal condition and a timeout here would mean
	/// nothing; Android needs a one-second socket timeout and a `CastRx.Idle`
	/// pseudo-event only because its read blocks a thread. A caller that wants a
	/// deadline — the probe does — races one and calls `close()`, which is what
	/// makes this return.
	func receive() async throws -> String? {
		let header = try await receiveExactly(4)
		guard let length = CastMessage.frameLength(header), length > 0 else {
			// A length we will not read is a stream we can no longer find our
			// place in, so there is nothing to do but drop the connection.
			throw Failure.closed
		}
		let body = try await receiveExactly(length)
		return CastMessage.payload(of: body)
	}

	private func receiveExactly(_ count: Int) async throws -> Data {
		try await withCheckedThrowingContinuation {
			(continuation: CheckedContinuation<Data, any Error>) in
			let resume = OneShot(continuation)
			connection.receive(minimumIncompleteLength: count, maximumLength: count) {
				data, _, _, error in
				if let data, data.count == count, error == nil {
					resume.succeed(data)
				} else {
					// Three ways to get here and one answer. An error and a
					// completed stream are both plainly the end; short of the
					// minimum with neither is a state `receive` does not
					// document, and treating that as a closed connection beats
					// looping on it. In every case the stream is out of step
					// with the length prefix and cannot be resynced.
					resume.fail(Failure.closed)
				}
			}
		}
	}

	// MARK: - Closing

	func close() {
		guard !closed else { return }
		closed = true
		connection.stateUpdateHandler = nil
		connection.cancel()
	}

	// MARK: - Parameters

	private static func parameters() -> NWParameters {
		let tls = NWProtocolTLS.Options()
		sec_protocol_options_set_verify_block(
			tls.securityProtocolOptions,
			{ _, _, complete in
				// **Accept any certificate, scoped to this connection.**
				//
				// Cast receivers present device certificates chaining to a
				// Google root that is in no system trust store, so ordinary
				// verification cannot succeed — `src/castmanager.cc` uses
				// `SSL_VERIFY_NONE` and Android installs a permissive
				// `X509TrustManager` for exactly this reason.
				//
				// What authenticates the exchange is not the certificate: the
				// person picked this device off their own network, and the
				// stream URL will carry a per-session token. This is the
				// documented per-connection hook and nothing else in the app
				// goes near these parameters — it is never a global default,
				// which is the distinction that matters.
				complete(true)
			},
			Self.queue)

		let tcp = NWProtocolTCP.Options()
		tcp.noDelay = true
		tcp.connectionTimeout = 5

		let parameters = NWParameters(tls: tls, tcp: tcp)
		// A Cast receiver is on the LAN by definition, and a cellular path
		// cannot reach one. Saying so keeps the connection from waiting on an
		// interface that will never serve it.
		parameters.prohibitedInterfaceTypes = [.cellular]
		return parameters
	}

	private static func resolved(_ connection: NWConnection) -> (host: String, port: UInt16)? {
		guard let remote = connection.currentPath?.remoteEndpoint,
			case .hostPort(let host, let port) = remote
		else {
			return nil
		}
		// An IPv6 host's description carries a `%en0` zone, which is noise in a
		// device list and not part of the address.
		guard let bare = "\(host)".split(separator: "%").first else { return nil }
		return (String(bare), port.rawValue)
	}
}

/// Every Cast receiver listens here; a multizone group gets a dynamic one, which
/// only ever arrives through discovery. It is the default a typed-in address
/// takes, and the same number `CastDeviceStore` writes.
let castDefaultPort: NWEndpoint.Port = 8009

extension CastEndpoint {
	var nwEndpoint: NWEndpoint {
		switch self {
		case .service(let name, let type, let domain):
			// Network.framework resolves a Bonjour service itself, which is why
			// this port needs no equivalent of Android's serialised resolve
			// queue.
			return .service(name: name, type: type, domain: domain ?? "local.", interface: nil)
		case .host(let host, let port):
			return .hostPort(
				host: NWEndpoint.Host(host),
				port: NWEndpoint.Port(rawValue: port) ?? castDefaultPort)
		}
	}
}

/// A continuation that may be resumed from more than one callback but must be
/// resumed exactly once.
///
/// Network.framework will happily call a handler again after a state change, and
/// resuming a `CheckedContinuation` twice is a trap rather than an error — so
/// this is a correctness device, not tidiness.
private final class OneShot<Value: Sendable>: @unchecked Sendable {
	private var continuation: CheckedContinuation<Value, any Error>?
	private let lock = NSLock()

	init(_ continuation: CheckedContinuation<Value, any Error>) {
		self.continuation = continuation
	}

	private func take() -> CheckedContinuation<Value, any Error>? {
		lock.lock()
		defer { lock.unlock() }
		let held = continuation
		continuation = nil
		return held
	}

	func succeed(_ value: Value) { take()?.resume(returning: value) }
	func fail(_ error: any Error) { take()?.resume(throwing: error) }
}

extension OneShot where Value == Void {
	func succeed() { succeed(()) }
}

/// Encodes a message payload. Nil only for a value `JSONSerialization` refuses,
/// which nothing here constructs.
func castJSONString(_ object: [String: Any]) -> String? {
	guard let data = try? JSONSerialization.data(withJSONObject: object) else { return nil }
	return String(data: data, encoding: .utf8)
}

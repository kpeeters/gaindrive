//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import OSLog

/// What to play, and where the receiver should fetch it from.
struct CastMedia: Sendable, Equatable {
	let url: URL
	/// Omitted from the LOAD when nil, leaving the receiver to sniff. Prefer
	/// supplying it: the receiver picks its decode pipeline from this, and a
	/// wrong guess surfaces as a decode error minutes later.
	var contentType: String?
	/// Omitted when unknown; a stated zero would be a lie.
	var duration: Double?
	/// Where to start. The receiver seeks using the file's own index.
	var startAt: Double = 0
	var title: String?
	var artist: String?
	var album: String?
	var artwork: URL?
	/// Selects which metadata block the receiver is given - see `metadata`.
	var isVideo = false
	/// What the server was asked for. Kept because a `MEDIA_STATUS` cannot
	/// answer it: the receiver reports no codec and no bitrate, and the
	/// `contentType` it echoes is only what our own LOAD told it.
	var quality: AudioQuality?
}

/// The Cast v2 control channel: one live connection to a receiver, its status,
/// and the commands that drive it.
///
/// Ported from `CastManager` in `src/castmanager.cc` by way of Android's
/// `CastSession.kt`. Two things are deliberately different and neither changes
/// the wire format: one multiplexed connection rather than a fresh one per
/// command, and Swift concurrency in place of a thread and a condition variable.
///
/// **`@MainActor`, with the blocking work inside `CastChannel`.** The state here
/// is small and read by SwiftUI, and the actor beneath it is where the socket
/// lives, so nothing needs guarding and no field is `nonisolated`. The load
/// generation stays an explicit counter rather than becoming task cancellation,
/// because it means something cancellation does not: it invalidates work already
/// in flight **on the receiver**.
///
/// What must not drift from the C++ is the retry behaviour in `LoadRetryWatcher`
/// and the diagnostics - the whole `MEDIA_STATUS` and `RECEIVER_STATUS` payloads
/// are logged for every push, because receiver-side regressions cannot be
/// diagnosed from summarised state.
@MainActor
@Observable
final class CastSession {
	/// The device being cast to, or nil when nothing is.
	private(set) var device: CastDevice?
	private(set) var status = CastStatus()
	/// The parameters of the last LOAD, so a retry can repeat it verbatim - and
	/// so a client can say what is playing and at what quality, neither of which
	/// is recoverable from a status push.
	private(set) var loaded: CastMedia?
	/// Something a person can act on. The engine turns it into a message.
	private(set) var failure: String?

	/// Fired for every status push, after `status` is updated.
	@ObservationIgnored var onStatus: ((CastStatus) -> Void)?

	@ObservationIgnored private var channel: CastChannel?
	@ObservationIgnored private var loop: Task<Void, Never>?
	/// Non-nil only while the receiver has our media session open.
	@ObservationIgnored private var transportId: String?
	@ObservationIgnored private var retry = LoadRetryWatcher()
	/// Bumped by every user-initiated LOAD, so work started for an earlier one
	/// abandons itself. **A retry deliberately does not bump it** - a retry is
	/// the same intent as the load it repeats.
	@ObservationIgnored private var loadGen = 0
	/// The media session already given up on, so a receiver's repeated idle
	/// pushes produce one STOP rather than a stream of them. The same shape as
	/// `CastEngine.advancedFrom`, and for the same reason.
	@ObservationIgnored private var gaveUpOn = 0
	@ObservationIgnored private var requestId = 1

	private static let log = Logger(subsystem: "org.gaindrive.ios", category: "cast")
	private static let reconnectDelay = Duration.milliseconds(500)
	private static let pollInterval = Duration.seconds(1)
	/// Long enough for a receiver that is already running our app to answer,
	/// short enough not to delay a launch that is going to be needed anyway.
	private static let channelWait = Duration.seconds(8)
	private static let statusWait = Duration.milliseconds(1500)
	private static let launchWait = Duration.seconds(10)
	private static let stopGrace = Duration.milliseconds(300)

	var isActive: Bool { device != nil }

	// MARK: - Lifecycle

	func connect(to target: CastDevice) {
		guard device != target || loop == nil else { return }
		// Switching devices tears down at once rather than going through
		// `disconnect()`: the courtesy STOP there is asynchronous, and waiting
		// for it would delay the connection somebody actually asked for.
		teardown()
		device = target
		Self.log.info("cast connecting to \(target.name, privacy: .public)")
		loop = Task { await runLoop(target) }
	}

	/// Leaves the receiver, stopping playback first so the television does not
	/// sit on an abandoned session.
	///
	/// The STOP has to go out **before** the loop is cancelled, because
	/// cancelling closes the socket it would travel on. So it all happens in one
	/// task, which gives up its claim if a `connect` has replaced the channel
	/// meanwhile.
	func disconnect() {
		guard let open = channel, let transport = transportId else {
			teardown()
			return
		}
		let msid = status.mediaSessionId
		let id = nextRequestId()
		// **Forgotten now, torn down in a moment.** The grace period below is
		// three hundred milliseconds during which somebody may well choose a
		// device again, and `connect` refuses a target it believes it is already
		// connected to - so leaving this set makes the next cast a silent
		// no-op. The pending teardown checks the channel is still the one it
		// started with, so it cannot take a newer session down with it.
		device = nil
		Task { [weak self] in
			try? await open.send(
				namespace: CastNamespace.media, destination: transport,
				payload: castJSONString([
					"type": "STOP", "requestId": id, "mediaSessionId": msid,
				]) ?? #"{"type":"STOP"}"#)
			// Long enough for the receiver to act on it; the socket dies next.
			try? await Task.sleep(for: Self.stopGrace)
			guard let self, self.channel === open else { return }
			self.teardown()
		}
	}

	/// The synchronous half of leaving: cancel, close, forget.
	private func teardown() {
		loop?.cancel()
		loop = nil
		let closing = channel
		channel = nil
		Task { await closing?.close() }
		device = nil
		transportId = nil
		status = CastStatus()
		retry.disarm()
		gaveUpOn = 0
		loaded = nil
		failure = nil
	}

	// MARK: - Commands

	func load(_ media: CastMedia) {
		loadGen += 1
		let generation = loadGen
		Self.log.info(
			"cast load gen=\(generation) start=\(media.startAt) url=\(media.url.absoluteString, privacy: .public)"
		)
		// Arm the retry against the session this LOAD is about to replace, and
		// seed the duration we were told so a client has one before the receiver
		// reports its own.
		retry.arm(superseding: status.mediaSessionId)
		status = CastStatus(duration: media.duration ?? 0)
		loaded = media
		Task { await sendLoad(media, generation: generation) }
	}

	func play() { mediaCommand("PLAY") }
	func pause() { mediaCommand("PAUSE") }
	func stopPlayback() { mediaCommand("STOP") }

	func seek(to seconds: Double) {
		mediaCommand("SEEK", extra: ["currentTime": seconds])
	}

	/// **Refused while there is no media session**, which is not defensiveness:
	/// the media namespace requires a valid `mediaSessionId`, and 0 is what
	/// `load()` resets the status to until the receiver answers with its own. A
	/// command sent in that window is rejected as an invalid request - and the
	/// one that lands there by construction is the PLAY that follows a LOAD,
	/// which the LOAD's own `autoplay` has already made unnecessary.
	private func mediaCommand(_ type: String, extra: [String: Any] = [:]) {
		guard status.mediaSessionId != 0 else {
			Self.log.info("cast \(type, privacy: .public) skipped - no media session yet")
			return
		}
		Task { [weak self] in
			guard let self, let open = await self.awaitChannel(),
				let transport = self.transportId
			else {
				return
			}
			var payload: [String: Any] = [
				"type": type, "requestId": self.nextRequestId(),
				"mediaSessionId": self.status.mediaSessionId,
			]
			payload.merge(extra) { _, new in new }
			guard let text = castJSONString(payload) else { return }
			Self.log.info("cast \(type, privacy: .public) sent")
			try? await open.send(
				namespace: CastNamespace.media, destination: transport, payload: text)
		}
	}

	// MARK: - The connection

	private func runLoop(_ target: CastDevice) async {
		// **A reconnect uses the address the last connection reached**, not the
		// Bonjour name it started from. Every use of a service endpoint is an
		// mDNS lookup, and the responders that matter here are the ones that
		// answer unreliably - `nw_resolver … did not receive all answers in
		// time` in the middle of a session is that, and it turns a reconnect
		// that should be instant into one that may not happen at all.
		var endpoint = target.endpoint
		while !Task.isCancelled {
			guard let open = try? await CastChannel.open(to: endpoint) else {
				Self.log.warning("cast connect failed, retrying")
				// Falling back to the name is what recovers a device that has
				// changed address - the one case the remembered one is wrong.
				endpoint = target.endpoint
				try? await Task.sleep(for: Self.reconnectDelay)
				continue
			}
			if let resolved = await open.resolvedEndpoint { endpoint = resolved }
			guard !Task.isCancelled else {
				await open.close()
				return
			}
			channel = open
			// The virtual connection to the platform must exist before anything
			// else is accepted; our own transport gets its own CONNECT once a
			// `RECEIVER_STATUS` names it.
			try? await open.send(
				namespace: CastNamespace.connection, destination: CastNamespace.receiverId,
				payload: #"{"type":"CONNECT"}"#)
			try? await open.send(
				namespace: CastNamespace.receiver, destination: CastNamespace.receiverId,
				payload: request("GET_STATUS"))

			let poll = pollTask(open)
			await pump(open)
			poll.cancel()

			// Reached only when the receiver closed on us. Logged because an
			// idle connection and one silently reconnecting every few seconds
			// look identical from outside - and this app sends nothing at all
			// between tracks, which is when a receiver is most likely to hang
			// up.
			Self.log.info("cast connection closed by receiver, reconnecting")
			await open.close()
			if channel === open {
				channel = nil
				transportId = nil
			}
			guard !Task.isCancelled else { return }
			try? await Task.sleep(for: Self.reconnectDelay)
		}
	}

	/// **The receiver pushes only on state changes**, so steady playback would
	/// report no position at all without an explicit poll. Android gets this for
	/// free from a socket read timeout; here the read waits indefinitely, so the
	/// poll is a task of its own - which is the tidier half of that trade.
	private func pollTask(_ open: CastChannel) -> Task<Void, Never> {
		Task { [weak self] in
			while !Task.isCancelled {
				try? await Task.sleep(for: Self.pollInterval)
				guard !Task.isCancelled, let self, let transport = self.transportId else { continue }
				try? await open.send(
					namespace: CastNamespace.media, destination: transport,
					payload: self.request("GET_STATUS"))
			}
		}
	}

	private func pump(_ open: CastChannel) async {
		while !Task.isCancelled {
			let received: String?
			do {
				received = try await open.receive()
			} catch {
				return
			}
			// A frame we could not read is skipped, not fatal - the two nils
			// `CastChannel.receive()` distinguishes.
			guard let payload = received, let message = castJSON(payload) else { continue }
			await handle(message, on: open)
		}
	}

	private func handle(_ message: [String: Any], on open: CastChannel) async {
		switch CastStatus.type(of: message) {
		case "PING":
			try? await open.send(
				namespace: CastNamespace.heartbeat, destination: CastNamespace.receiverId,
				payload: #"{"type":"PONG"}"#)
		case "RECEIVER_STATUS":
			// Logged whole: a STOP and an idle teardown both invalidate our
			// transport, and this is the only warning of either.
			Self.log.info("cast rx RECEIVER_STATUS: \(String(describing: message), privacy: .public)")
			await onReceiverStatus(message, on: open)
		case "MEDIA_STATUS":
			Self.log.info("cast rx MEDIA_STATUS: \(String(describing: message), privacy: .public)")
			if let parsed = CastStatus.parse(message) { onMediaStatus(parsed) }
		case "ERROR":
			// A refused LOAD arrives here rather than as a status, so without
			// this the client would park on a dead progress bar with nothing
			// anywhere saying why.
			Self.log.warning("cast rx ERROR: \(String(describing: message), privacy: .public)")
			failure = "The receiver refused that track."
		default:
			break
		}
	}

	private func onReceiverStatus(_ message: [String: Any], on open: CastChannel) async {
		guard
			let transport = CastStatus.transportId(
				in: message, appId: CastNamespace.defaultMediaApp)
		else {
			transportId = nil
			return
		}
		guard transport != transportId else { return }
		// Connect to the app's transport **before** publishing it: everything
		// waiting on it goes straight on to send a media command, and the
		// receiver drops those until the virtual connection exists.
		try? await open.send(
			namespace: CastNamespace.connection, destination: transport,
			payload: #"{"type":"CONNECT"}"#)
		transportId = transport
		try? await open.send(
			namespace: CastNamespace.media, destination: transport,
			payload: request("GET_STATUS"))
	}

	private func onMediaStatus(_ fresh: CastStatus) {
		// A push mid-playback carries no `media` block, so the duration arrives
		// once and must be carried forward rather than collapsing to zero. The
		// active subtitle tracks are stated on the same terms and carried
		// forward for the same reason.
		var merged = fresh
		if merged.duration == 0 { merged.duration = status.duration }
		if merged.activeTrackIds == nil { merged.activeTrackIds = status.activeTrackIds }
		status = merged

		if retry.consume(merged), let media = loaded {
			let generation = loadGen
			Self.log.warning("cast auto-retry LOAD (gen=\(generation)) - receiver went IDLE/ERROR")
			Task { await sendLoad(media, generation: generation) }
		} else if merged.isIdleError, merged.mediaSessionId != 0,
			merged.mediaSessionId != gaveUpOn
		{
			// **Give up out loud, once, and tell the receiver to stop.**
			//
			// The retry above is for the Default Media Receiver's habit of
			// failing the first LOAD that lands while another session is
			// playing; a second error is a different thing and repeating the
			// LOAD will not fix it. What matters is that leaving it alone is not
			// neutral: a receiver that cannot decode what it was given goes on
			// fetching by itself, resetting the connection and asking again from
			// a fresh offset, for as long as the session stands. Measured
			// against a WiiM handed an AV1 film - thousands of ranged GETs, a
			// saturated link, and an app that reported only "loading".
			//
			// So the session is stopped rather than abandoned, and the failure
			// is stated. Guarded on the media session so the receiver's repeated
			// idle pushes produce one STOP rather than a stream of them.
			gaveUpOn = merged.mediaSessionId
			Self.log.warning("cast receiver failed the LOAD twice; stopping")
			failure = "That track would not play on this device."
			stopPlayback()
		}
		onStatus?(merged)
	}

	// MARK: - LOAD

	private func sendLoad(_ media: CastMedia, generation: Int) async {
		guard loadGen == generation else { return }
		// **Waited for rather than required.** Choosing a device connects and
		// loads in the same breath, so the first LOAD of a session is issued
		// while the TLS handshake is still in progress; a version of this that
		// gave up on a nil channel would drop precisely the load somebody just
		// asked for, silently, and only ever the first one.
		guard let open = await awaitChannel() else {
			Self.log.warning("cast no channel; LOAD abandoned")
			failure = "Could not reach that device."
			return
		}
		guard loadGen == generation else { return }
		guard let transport = await ensureTransport(on: open) else {
			Self.log.warning("cast no transportId from receiver; LOAD abandoned")
			failure = "The receiver would not start."
			return
		}
		guard loadGen == generation else { return }

		var content: [String: Any] = [
			"contentId": media.url.absoluteString,
			"streamType": "BUFFERED",
		]
		if let type = media.contentType { content["contentType"] = type }
		// Redundant against the receiver's own parsing, but it gives an early
		// hint before any byte-range request is made.
		if let duration = media.duration, duration > 0 { content["duration"] = duration }
		if let metadata = metadata(for: media) { content["metadata"] = metadata }

		var payload: [String: Any] = [
			"type": "LOAD",
			"requestId": nextRequestId(),
			// Explicit even though the spec defaults it true - some receiver
			// versions have been quirky about it, and it costs nothing.
			"autoplay": true,
			"media": content,
		]
		if media.startAt > 0 { payload["currentTime"] = media.startAt }

		guard let text = castJSONString(payload) else { return }
		Self.log.info("cast LOAD \(text, privacy: .public)")
		try? await open.send(
			namespace: CastNamespace.media, destination: transport, payload: text)
	}

	/// The transport of a running Default Media Receiver, launching one if there
	/// is none.
	///
	/// **Asking before launching is not politeness**: LAUNCH against an already
	/// running app tears it down and recreates it, which loses the session we
	/// may be about to load into.
	private func ensureTransport(on open: CastChannel) async -> String? {
		if let transport = transportId { return transport }

		// **Ask before launching.** The connect already sent one GET_STATUS, but
		// a LOAD can arrive before the answer does, and guessing wrong here is
		// not a lost second - it is a torn-down session.
		try? await open.send(
			namespace: CastNamespace.receiver, destination: CastNamespace.receiverId,
			payload: request("GET_STATUS"))
		if let transport = await awaitTransport(for: Self.statusWait) { return transport }

		try? await open.send(
			namespace: CastNamespace.receiver, destination: CastNamespace.receiverId,
			payload: request("LAUNCH", extra: ["appId": CastNamespace.defaultMediaApp]))
		return await awaitTransport(for: Self.launchWait)
	}

	private func awaitChannel() async -> CastChannel? {
		let deadline = ContinuousClock.now.advanced(by: Self.channelWait)
		while ContinuousClock.now < deadline {
			if let open = channel { return open }
			try? await Task.sleep(for: .milliseconds(50))
			if Task.isCancelled { return nil }
		}
		return channel
	}

	/// Polls rather than awaiting a stream, because the value is set on this
	/// actor by the receive loop and there is nothing to subscribe to.
	///
	/// The launch wait is generous on purpose: starting the Default Media
	/// Receiver makes the television fetch a web app from Google, and several
	/// seconds of nothing there is normal rather than a stall - it is the
	/// gap between `LAUNCH_STATUS` and the LOAD.
	private func awaitTransport(for limit: Duration) async -> String? {
		let deadline = ContinuousClock.now.advanced(by: limit)
		while ContinuousClock.now < deadline {
			if let transport = transportId { return transport }
			try? await Task.sleep(for: .milliseconds(100))
			if Task.isCancelled { return nil }
		}
		return transportId
	}

	/// What the receiver shows on the television while playing.
	///
	/// The C++ sends none, because the web client is the only thing looking at
	/// the browser's own UI. From a phone the television **is** the screen
	/// people are watching.
	///
	/// A branch rather than a changed constant: `artist` and `albumName` are
	/// fields of a music track and not of a movie, so sending them under
	/// `metadataType: 1` puts nothing on screen at all.
	private func metadata(for media: CastMedia) -> [String: Any]? {
		guard media.title != nil || media.artist != nil || media.album != nil
			|| media.artwork != nil
		else {
			return nil
		}
		var out: [String: Any] = [:]
		if media.isVideo {
			out["metadataType"] = 1  // MovieMediaMetadata
			if let title = media.title { out["title"] = title }
			if let album = media.album { out["subtitle"] = album }
		} else {
			out["metadataType"] = 3  // MusicTrackMediaMetadata
			if let title = media.title { out["title"] = title }
			if let artist = media.artist { out["artist"] = artist }
			if let album = media.album { out["albumName"] = album }
		}
		if let artwork = media.artwork {
			out["images"] = [["url": artwork.absoluteString]]
		}
		return out
	}

	// MARK: - Small things

	private func nextRequestId() -> Int {
		defer { requestId += 1 }
		return requestId
	}

	private func request(_ type: String, extra: [String: Any] = [:]) -> String {
		var payload: [String: Any] = ["type": type, "requestId": nextRequestId()]
		payload.merge(extra) { _, new in new }
		return castJSONString(payload) ?? #"{"type":"\#(type)"}"#
	}

	func clearFailure() {
		failure = nil
	}
}

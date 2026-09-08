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

/// Reading what a receiver says.
///
/// These are payloads a device we do not control puts on the wire, so what is
/// tested is mostly what happens when one of them is not the shape the happy
/// path assumes — a malformed push must cost one message, never the receive
/// loop.
struct CastStatusTests {
	private func json(_ text: String) throws -> [String: Any] {
		try #require(castJSON(text))
	}

	// MARK: - MEDIA_STATUS

	@Test func aPlayingStatusParses() throws {
		let status = try #require(
			CastStatus.parse(
				json(
					#"""
					{"type":"MEDIA_STATUS","status":[{"mediaSessionId":3,
					 "playerState":"PLAYING","currentTime":12.5,
					 "media":{"duration":261.0}}]}
					"""#)))
		#expect(status.playerState == .playing)
		#expect(status.currentTime == 12.5)
		#expect(status.duration == 261)
		#expect(status.mediaSessionId == 3)
		#expect(status.isLive)
	}

	/// A push during playback omits `media` entirely — the receiver only repeats
	/// it when the item changes — so zero here means "not stated", and the
	/// session carries the last known value forward rather than believing it.
	@Test func aPushWithoutMediaReportsNoDuration() throws {
		let status = try #require(
			CastStatus.parse(
				json(#"{"status":[{"mediaSessionId":3,"playerState":"PLAYING","currentTime":40}]}"#)))
		#expect(status.duration == 0)
	}

	/// The two idle reasons that matter are the two that drive everything else:
	/// one advances the queue, the other retries the load.
	@Test func idleReasonsAreDistinguished() throws {
		let finished = try #require(
			CastStatus.parse(
				json(#"{"status":[{"playerState":"IDLE","idleReason":"FINISHED"}]}"#)))
		#expect(finished.isIdleFinished)
		#expect(!finished.isIdleError)

		let failed = try #require(
			CastStatus.parse(json(#"{"status":[{"playerState":"IDLE","idleReason":"ERROR"}]}"#)))
		#expect(failed.isIdleError)
		#expect(!failed.isIdleFinished)

		// Neither, which is what an interrupted session reports on its way out.
		let interrupted = try #require(
			CastStatus.parse(
				json(#"{"status":[{"playerState":"IDLE","idleReason":"INTERRUPTED"}]}"#)))
		#expect(!interrupted.isIdleError)
		#expect(!interrupted.isIdleFinished)
	}

	/// A state we have never seen must not be read as `idle`, which is the one
	/// value that drives both the retry and the queue advance.
	@Test func anUnknownStateIsNotIdle() throws {
		let status = try #require(
			CastStatus.parse(json(#"{"status":[{"playerState":"SOMETHING_NEW"}]}"#)))
		#expect(status.playerState == .unknown)
		#expect(!status.isIdleError)
		#expect(!status.isLive)
	}

	/// Absent is not empty. The receiver states `activeTrackIds` when the
	/// selection changes and omits it from the pushes in between, so reading an
	/// absent field as "none selected" would make a caption picker's tick
	/// flicker off once a second.
	@Test func activeTrackIdsDistinguishAbsentFromEmpty() throws {
		let absent = try #require(CastStatus.parse(json(#"{"status":[{"playerState":"PLAYING"}]}"#)))
		#expect(absent.activeTrackIds == nil)

		let off = try #require(
			CastStatus.parse(json(#"{"status":[{"playerState":"PLAYING","activeTrackIds":[]}]}"#)))
		#expect(off.activeTrackIds == [])

		let on = try #require(
			CastStatus.parse(json(#"{"status":[{"playerState":"PLAYING","activeTrackIds":[2]}]}"#)))
		#expect(on.activeTrackIds == [2])
	}

	@Test func aMessageWithNoStatusArrayIsNotAStatus() throws {
		let empty = try json(#"{"type":"MEDIA_STATUS","status":[]}"#)
		#expect(CastStatus.parse(empty) == nil)
		let pong = try json(#"{"type":"PONG"}"#)
		#expect(CastStatus.parse(pong) == nil)
	}

	@Test func garbageIsNotAMessage() {
		#expect(castJSON("not json at all") == nil)
		// Valid JSON that is not an object — a peer is allowed to be wrong in
		// more ways than one.
		#expect(castJSON("[1,2,3]") == nil)
	}

	// MARK: - RECEIVER_STATUS

	/// **The trap this exists to prevent.** A television sitting idle runs its
	/// own ambient app — `E8C28D3C`, "Backdrop" — which publishes a
	/// `transportId` like any other. Taking the first entry sends the LOAD to a
	/// screensaver, which ignores the media namespace: no status, no fetch, no
	/// error and nothing in any log.
	@Test func backdropIsNotAMediaReceiver() throws {
		let message = try json(
			#"""
			{"type":"RECEIVER_STATUS","status":{"applications":[
			 {"appId":"E8C28D3C","displayName":"Backdrop","transportId":"web-5",
			  "sessionId":"s-5"}]}}
			"""#)
		#expect(CastStatus.transportId(in: message, appId: CastNamespace.defaultMediaApp) == nil)
		#expect(CastStatus.sessionId(in: message, appId: CastNamespace.defaultMediaApp) == nil)
	}

	@Test func ourOwnAppIsFoundBesideBackdrop() throws {
		let message = try json(
			#"""
			{"type":"RECEIVER_STATUS","status":{"applications":[
			 {"appId":"E8C28D3C","displayName":"Backdrop","transportId":"web-5"},
			 {"appId":"CC1AD845","displayName":"Default Media Receiver",
			  "transportId":"web-9","sessionId":"s-9"}]}}
			"""#)
		#expect(CastStatus.transportId(in: message, appId: CastNamespace.defaultMediaApp) == "web-9")
		#expect(CastStatus.sessionId(in: message, appId: CastNamespace.defaultMediaApp) == "s-9")
	}

	/// A truly idle receiver reports no applications at all, which is the signal
	/// to launch ours — and is why the Backdrop bug survived so long, since
	/// whether it bit depended on what the television happened to be showing.
	@Test func anIdleReceiverOffersNoTransport() throws {
		let message = try json(#"{"type":"RECEIVER_STATUS","status":{"volume":{"level":1}}}"#)
		#expect(CastStatus.transportId(in: message, appId: CastNamespace.defaultMediaApp) == nil)
	}

	/// An empty `transportId` is not an answer, and treating it as one would
	/// send a LOAD to nowhere.
	@Test func anEmptyTransportIdIsNoTransport() throws {
		let message = try json(
			#"{"status":{"applications":[{"appId":"CC1AD845","transportId":""}]}}"#)
		#expect(CastStatus.transportId(in: message, appId: CastNamespace.defaultMediaApp) == nil)
	}

	@Test func theMessageTypeIsRead() throws {
		let ping = try json(#"{"type":"PING"}"#)
		#expect(CastStatus.type(of: ping) == "PING")
		// A reply carrying no type is not a message any namespace routes on.
		let reply = try json(#"{"requestId":1}"#)
		#expect(CastStatus.type(of: reply) == nil)
	}
}

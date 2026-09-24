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

/// The Cast v2 wire format.
///
/// Hand-encoded protobuf is exactly the kind of code that is either right or
/// silently wrong - a receiver answers a malformed frame by saying nothing at
/// all - so the encoder and the decoder are checked against each other and
/// against the shapes a hostile or broken peer can send.
struct CastMessageTests {
	private func roundTrip(_ payload: String) -> String? {
		let body = CastMessage.body(
			namespace: CastNamespace.receiver, source: CastNamespace.sender,
			destination: CastNamespace.receiverId, payload: payload)
		return CastMessage.payload(of: body)
	}

	@Test func aPayloadSurvivesTheRoundTrip() {
		#expect(roundTrip(#"{"type":"GET_STATUS","requestId":1}"#) == #"{"type":"GET_STATUS","requestId":1}"#)
	}

	@Test func aNonAsciiPayloadSurvivesIt() {
		// The length prefix is in **bytes, not characters**. Getting that wrong
		// produces a frame the receiver cannot parse, and only for the users
		// whose libraries contain one.
		#expect(roundTrip(#"{"title":"Björk - Jóga"}"#) == #"{"title":"Björk - Jóga"}"#)
	}

	@Test func anEmptyPayloadIsStillAPayload() {
		#expect(roundTrip("") == "")
	}

	@Test func theFrameHeaderIsBigEndian() {
		let body = Data(repeating: 0, count: 0x0102)
		let framed = CastMessage.frame(body)
		#expect(framed.count == 4 + 0x0102)
		#expect(Array(framed.prefix(4)) == [0, 0, 0x01, 0x02])
		#expect(CastMessage.frameLength(framed.prefix(4)) == 0x0102)
	}

	/// **A bad length must not allocate.** The four bytes come off the network
	/// before anything has authenticated them, so a peer announcing four
	/// gigabytes has to cost nothing.
	@Test func anOversizedLengthIsRefused() {
		#expect(CastMessage.frameLength(Data([0xff, 0xff, 0xff, 0xff])) == nil)
		#expect(CastMessage.frameLength(Data([0x00, 0x20, 0x00, 0x00])) == nil)
		// One byte under the cap is still readable, so the refusal is a
		// boundary rather than a blanket.
		#expect(CastMessage.frameLength(Data([0x00, 0x0f, 0xff, 0xff])) == 0x0f_ffff)
	}

	@Test func aShortHeaderIsRefused() {
		#expect(CastMessage.frameLength(Data([0x00, 0x00, 0x01])) == nil)
		#expect(CastMessage.frameLength(Data()) == nil)
	}

	/// The decoder walks the fields rather than assuming an order, so a message
	/// carrying only the fields before ours must still be read to the end
	/// without wandering into them.
	@Test func aMessageWithNoPayloadFieldReadsAsNone() {
		var body = Data()
		body.appendCastVarintField(1, 0)
		body.appendCastStringField(2, CastNamespace.sender)
		body.appendCastStringField(4, CastNamespace.heartbeat)
		#expect(CastMessage.payload(of: body) == nil)
	}

	/// Truncation is the ordinary shape of a broken frame, and reading past the
	/// end of one would be the interesting kind of bug.
	@Test func aTruncatedLengthDelimitedFieldIsRefused() {
		var body = CastMessage.body(
			namespace: CastNamespace.media, source: CastNamespace.sender,
			destination: "transport-0", payload: #"{"type":"MEDIA_STATUS"}"#)
		body = body.prefix(body.count - 5)
		#expect(CastMessage.payload(of: body) == nil)
	}

	/// A wire type we do not write means this is not a message we understand;
	/// stopping beats walking off into the payload.
	@Test func anUnknownWireTypeStops() {
		// Field 6, wire type 5 (32-bit) - a shape the real protocol never uses.
		let body = Data([6 << 3 | 5, 0, 0, 0, 0])
		#expect(CastMessage.payload(of: body) == nil)
	}
}

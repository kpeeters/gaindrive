//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The Cast v2 wire format: a protobuf `CastMessage` inside a four-byte
/// big-endian length frame.
///
/// Hand-encoded rather than generated, and ported from `src/castmanager.cc`
/// (`pb_varint`, `pb_string_field`, `pb_payload`) by way of Android's
/// `CastMessage.kt`. The schema is six fields, so a protobuf dependency and its
/// code generation would buy nothing — and this app has no package
/// dependencies at all, which is worth keeping.
///
/// * field 1 varint — `protocol_version` (0 = `CASTV2_1_0`)
/// * field 2 string — `source_id`
/// * field 3 string — `destination_id`
/// * field 4 string — `namespace`
/// * field 5 varint — `payload_type` (0 = `STRING`)
/// * field 6 string — `payload_utf8`
///
/// Only field 6 is ever read back. The rest are written and never inspected,
/// which is why `payload(of:)` skips fields rather than parsing a whole
/// message.
enum CastMessage {
	/// Refuse a frame claiming more than this. **A bad length must not
	/// allocate**: the four bytes come off the network before anything has
	/// authenticated them, and a receiver — or something pretending to be one —
	/// announcing four gigabytes must cost nothing.
	static let maxFrame = 1 << 20

	private static let protocolVersion: UInt64 = 0
	private static let payloadTypeString: UInt64 = 0
	private static let wireVarint: UInt64 = 0
	private static let wireLengthDelimited: UInt64 = 2

	// MARK: - Writing

	static func body(
		namespace: String, source: String, destination: String, payload: String
	) -> Data {
		var out = Data()
		out.appendCastVarintField(1, protocolVersion)
		out.appendCastStringField(2, source)
		out.appendCastStringField(3, destination)
		out.appendCastStringField(4, namespace)
		out.appendCastVarintField(5, payloadTypeString)
		out.appendCastStringField(6, payload)
		return out
	}

	/// Big-endian length prefix, which is how the receiver delimits messages.
	static func frame(_ body: Data) -> Data {
		var out = Data(capacity: 4 + body.count)
		let length = UInt32(body.count)
		out.append(UInt8(truncatingIfNeeded: length >> 24))
		out.append(UInt8(truncatingIfNeeded: length >> 16))
		out.append(UInt8(truncatingIfNeeded: length >> 8))
		out.append(UInt8(truncatingIfNeeded: length))
		out.append(body)
		return out
	}

	// MARK: - Reading

	/// The length a four-byte header announces, or nil if it is not four bytes
	/// or claims more than `maxFrame`.
	///
	/// Returning nil rather than clamping is deliberate: a frame we will not
	/// read is a stream we can no longer find our place in, so the caller's
	/// only correct response is to drop the connection.
	static func frameLength(_ header: Data) -> Int? {
		guard header.count == 4 else { return nil }
		let bytes = [UInt8](header)
		let length =
			Int(bytes[0]) << 24 | Int(bytes[1]) << 16 | Int(bytes[2]) << 8 | Int(bytes[3])
		guard length >= 0, length <= maxFrame else { return nil }
		return length
	}

	/// The `payload_utf8` field, or nil if the message carries none.
	///
	/// Walks the fields rather than assuming an order: the receiver is free to
	/// emit them in any, and a length-delimited field has to be skipped by its
	/// length whether or not we want it.
	static func payload(of body: Data) -> String? {
		var reader = Reader(body)
		while reader.hasMore {
			guard let tag = reader.varint() else { return nil }
			let field = Int(tag >> 3)
			switch tag & 7 {
			case wireLengthDelimited:
				guard let raw = reader.varint(), let length = Int(exactly: raw),
					length >= 0, reader.position + length <= body.count
				else {
					return nil
				}
				if field == 6 {
					return String(data: reader.slice(length), encoding: .utf8)
				}
				reader.position += length
			case wireVarint:
				guard reader.varint() != nil else { return nil }
			default:
				// Any other wire type means this is not a message we wrote or
				// understand; stopping beats walking off into the payload.
				return nil
			}
		}
		return nil
	}

	private struct Reader {
		private let bytes: Data
		var position: Int

		init(_ bytes: Data) {
			// `Data` slices keep their parent's indices, so a sub-range would
			// make every offset here wrong. Re-basing once is cheaper than
			// remembering not to index it.
			self.bytes = Data(bytes)
			position = 0
		}

		var hasMore: Bool { position < bytes.count }

		mutating func slice(_ length: Int) -> Data {
			defer { position += length }
			return bytes.subdata(in: position..<(position + length))
		}

		mutating func varint() -> UInt64? {
			var result: UInt64 = 0
			var shift: UInt64 = 0
			while position < bytes.count {
				let byte = bytes[position]
				position += 1
				result |= UInt64(byte & 0x7f) << shift
				if byte & 0x80 == 0 { return result }
				shift += 7
				if shift > 63 { return nil }
			}
			return nil
		}
	}
}

//	Internal rather than fileprivate so `CastMessageTests` can build the shapes a
//	receiver may send and this file never writes — a message with no payload
//	field, a truncated one. The names carry `cast` for the same reason the
//	visibility is wider than it wants to be: these sit on `Data`, and a bare
//	`appendVarint` would read as a general utility.
extension Data {
	mutating func appendCastVarint(_ value: UInt64) {
		var v = value
		while v > 127 {
			append(UInt8(v & 0x7f) | 0x80)
			v >>= 7
		}
		append(UInt8(v))
	}

	mutating func appendCastVarintField(_ field: UInt64, _ value: UInt64) {
		appendCastVarint(field << 3 | 0)
		appendCastVarint(value)
	}

	mutating func appendCastStringField(_ field: UInt64, _ value: String) {
		let bytes = Data(value.utf8)
		appendCastVarint(field << 3 | 2)
		// The length is in **bytes, not characters** — a non-ASCII device name
		// or track title would otherwise produce a frame the receiver cannot
		// parse, and it would do so only for some users' libraries.
		appendCastVarint(UInt64(bytes.count))
		append(bytes)
	}
}

/// The four namespaces and the two fixed endpoint ids the protocol uses.
///
/// `CC1AD845` is Google's **Default Media Receiver**, a generic app any Cast
/// device can launch, which is why none of this needs a Cast developer
/// registration — `src/castmanager.cc` launches the same one. It is also the app
/// *we* launch, so the LAUNCH and the `appId` match in `CastStatus` both read
/// this constant and cannot drift.
enum CastNamespace {
	static let connection = "urn:x-cast:com.google.cast.tp.connection"
	static let heartbeat = "urn:x-cast:com.google.cast.tp.heartbeat"
	static let receiver = "urn:x-cast:com.google.cast.receiver"
	static let media = "urn:x-cast:com.google.cast.media"

	static let sender = "sender-0"
	static let receiverId = "receiver-0"
	static let defaultMediaApp = "CC1AD845"
}

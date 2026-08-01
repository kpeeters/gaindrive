package org.gaindrive.android.playback.cast

import java.io.ByteArrayOutputStream

/**
 * The Cast v2 wire format: a protobuf `CastMessage` inside a four-byte
 * big-endian length frame.
 *
 * Hand-encoded rather than generated, and ported from `src/castmanager.cc`
 * (`pb_varint`, `pb_string_field`, `pb_payload`). The schema is six fields, so
 * a protobuf dependency and its codegen would buy nothing:
 *
 * * field 1 varint — protocol_version (0 = CASTV2_1_0)
 * * field 2 string — source_id
 * * field 3 string — destination_id
 * * field 4 string — namespace
 * * field 5 varint — payload_type (0 = STRING)
 * * field 6 string — payload_utf8
 *
 * Only field 6 is ever read back. The rest are written and never inspected,
 * which is why [payloadOf] skips fields rather than parsing a whole message.
 */
internal object CastMessage {

	/** Refuse a frame claiming more than this; a bad length must not allocate. */
	const val MAX_FRAME = 1 shl 20

	private const val PROTOCOL_VERSION = 0L
	private const val PAYLOAD_TYPE_STRING = 0L

	fun body(
		namespace: String,
		source: String,
		destination: String,
		payload: String,
	): ByteArray {
		val out = ByteArrayOutputStream()
		out.varintField(1, PROTOCOL_VERSION)
		out.stringField(2, source)
		out.stringField(3, destination)
		out.stringField(4, namespace)
		out.varintField(5, PAYLOAD_TYPE_STRING)
		out.stringField(6, payload)
		return out.toByteArray()
	}

	/** Big-endian length prefix, which is how the receiver delimits messages. */
	fun frame(body: ByteArray): ByteArray {
		val out = ByteArray(4 + body.size)
		val len = body.size
		out[0] = (len ushr 24).toByte()
		out[1] = (len ushr 16).toByte()
		out[2] = (len ushr 8).toByte()
		out[3] = len.toByte()
		body.copyInto(out, 4)
		return out
	}

	fun frameLength(header: ByteArray): Int =
		((header[0].toInt() and 0xff) shl 24) or
			((header[1].toInt() and 0xff) shl 16) or
			((header[2].toInt() and 0xff) shl 8) or
			(header[3].toInt() and 0xff)

	/**
	 * The `payload_utf8` field, or null if the message carries none.
	 *
	 * Walks the fields rather than assuming an order: the receiver is free to
	 * emit them in any, and length-delimited fields have to be skipped by their
	 * length whether or not we want them.
	 */
	fun payloadOf(body: ByteArray): String? {
		val reader = Reader(body)
		while (reader.hasMore) {
			val tag = reader.varint() ?: return null
			val field = (tag ushr 3).toInt()
			when ((tag and 7L).toInt()) {
				WIRE_LENGTH_DELIMITED -> {
					val len = (reader.varint() ?: return null).toInt()
					if (len < 0 || reader.pos + len > body.size) return null
					if (field == 6) {
						return String(body, reader.pos, len, Charsets.UTF_8)
					}
					reader.pos += len
				}
				WIRE_VARINT -> reader.varint() ?: return null
				// Any other wire type means this is not a message we wrote or
				// understand; stopping beats walking off into the payload.
				else -> return null
			}
		}
		return null
	}

	private const val WIRE_VARINT = 0
	private const val WIRE_LENGTH_DELIMITED = 2

	private fun ByteArrayOutputStream.varint(value: Long) {
		var v = value
		while (v > 127) {
			write(((v and 0x7f) or 0x80).toInt())
			v = v ushr 7
		}
		write(v.toInt())
	}

	private fun ByteArrayOutputStream.varintField(field: Int, value: Long) {
		varint((field.toLong() shl 3) or WIRE_VARINT.toLong())
		varint(value)
	}

	private fun ByteArrayOutputStream.stringField(field: Int, value: String) {
		val bytes = value.toByteArray(Charsets.UTF_8)
		varint((field.toLong() shl 3) or WIRE_LENGTH_DELIMITED.toLong())
		// The length is in bytes, not characters — a non-ASCII device name or
		// track title would otherwise produce a frame the receiver cannot parse.
		varint(bytes.size.toLong())
		write(bytes)
	}

	private class Reader(private val bytes: ByteArray) {
		var pos = 0
		val hasMore: Boolean get() = pos < bytes.size

		fun varint(): Long? {
			var result = 0L
			var shift = 0
			while (pos < bytes.size) {
				val b = bytes[pos++].toInt() and 0xff
				result = result or ((b and 0x7f).toLong() shl shift)
				if (b and 0x80 == 0) return result
				shift += 7
				if (shift > 63) return null
			}
			return null
		}
	}
}

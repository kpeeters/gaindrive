package org.gaindrive.android.data

import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.playback.MEDIA3_AUDIO
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The audio half of `playable`: what this device says it can play as it
 * stands, and - the half worth testing - what it must never say.
 *
 * Declaring too little wastes a re-encode, which is slow and invisible.
 * Declaring something media3 cannot decode hands over a file that plays as
 * silence; declaring anything at all on the cast route hands a receiver a
 * container its `LOAD` did not announce.
 */
class PlayableAudioTest {

	private val opus160 = AudioQuality(AudioFormat.OPUS, 160)

	/**
	 * Lossless is declared whatever the setting: size is `maxBitRate`'s job,
	 * and with no ceiling a declared FLAC is what an unplayable original is
	 * converted to.
	 */
	@Test
	fun `the lossless formats are declared`() {
		assertTrue(MEDIA3_AUDIO.contains("flac/flac"))
		assertTrue(MEDIA3_AUDIO.contains("mp4/alac"))
		assertTrue(MEDIA3_AUDIO.contains("ogg/flac"))
	}

	/**
	 * media3 has no Speex decoder, and the server refuses wav and wma in any
	 * form. Declaring any of the three would be a claim nothing acts on at
	 * best, and silence at worst.
	 */
	@Test
	fun `speex wav and wma are never declared`() {
		val everything = MEDIA3_AUDIO
		assertFalse(everything.contains("ogg/speex"))
		assertTrue(everything.none { it.startsWith("riff/") })
		assertTrue(everything.none { it.startsWith("asf/") })
	}

	/**
	 * The server refuses a bare `m4a` or `ogg` - the extension does not settle
	 * the codec, and it must not guess. Declaring one bare would be dropped
	 * silently, so this is the only thing that would notice.
	 */
	@Test
	fun `the ambiguous containers are declared for each codec`() {
		val everything = MEDIA3_AUDIO
		assertEquals(
			setOf("mp4/aac", "mp4/alac"),
			everything.filter { it.startsWith("mp4/") }.toSet(),
		)
		assertEquals(
			setOf("ogg/vorbis", "ogg/opus", "ogg/flac"),
			everything.filter { it.startsWith("ogg/") }.toSet(),
		)
	}

	/**
	 * A container is named once, whatever extension it is written under. The
	 * server stores the container it observed, so `.ogg`, `.oga` and `.opus`
	 * are all `ogg` - and a token naming an *extension* would reach none of
	 * them. This is the assertion that fails if someone reintroduces one.
	 */
	@Test
	fun `no token names a file extension`() {
		val everything = MEDIA3_AUDIO
		val containers = everything.map { it.substringBefore('/') }.toSet()
		listOf("oga", "m4a", "mp3", "opus", "wav", "wma").forEach {
			assertFalse("$it is an extension, not a container", it in containers)
		}
	}

	/**
	 * Every audio token is a pair. A bare one would be read by the server as a
	 * *video* container and match nothing at all - silently, which is why this
	 * is asserted rather than left to the reader of the set above.
	 */
	@Test
	fun `every token is one container over one codec`() {
		MEDIA3_AUDIO.forEach {
			assertTrue("$it is not lowercase", it == it.lowercase())
			assertEquals("$it is not one pair", 1, it.count { c -> c == '/' })
			assertFalse("$it has an empty half", it.startsWith("/") || it.endsWith("/"))
		}
	}

	// ---- the query itself ------------------------------------------------

	@Test
	fun `a declared set becomes one sorted comma list`() {
		assertEquals(
			mapOf(
				"id" to "7",
				"format" to "opus",
				"maxBitRate" to "160",
				"playable" to "adts/aac,mpeg/mp3",
			),
			audioStreamParams("7", opus160, setOf("mpeg/mp3", "adts/aac")),
		)
	}

	/**
	 * A `Set`'s iteration order is not a promise, and this string lands in
	 * OkHttp's cache key and the server's log, so one request must build
	 * exactly one URL however the set was assembled.
	 */
	@Test
	fun `the order of the set does not reach the url`() {
		assertEquals(
			audioStreamParams("7", opus160, setOf("mpeg/mp3", "ogg/vorbis")),
			audioStreamParams("7", opus160, setOf("ogg/vorbis", "mpeg/mp3")),
		)
	}

	/**
	 * The audio twin of `the cast route declares nothing`. A receiver handed a
	 * URL that declares is sent the original while its `LOAD` announced the
	 * transcode's type, and refuses the media. The obvious refactor - moving
	 * the declaration into the builder's default "because both callers want
	 * it" - is exactly that bug, and this is what stands in its way.
	 */
	@Test
	fun `the cast route declares nothing`() {
		assertEquals(
			mapOf("id" to "7", "format" to "opus", "maxBitRate" to "160"),
			audioStreamParams("7", opus160, emptySet()),
		)
	}

	/**
	 * The original is spelled by sending no `format` at all, so a request for
	 * it carries no bitrate either - the server serves the file directly with
	 * no ffmpeg involved.
	 */
	@Test
	fun `the original sends no format and no bitrate`() {
		assertEquals(
			mapOf("id" to "7", "playable" to "flac/flac,mpeg/mp3"),
			audioStreamParams("7", AudioQuality.ORIGINAL, setOf("mpeg/mp3", "flac/flac")),
		)
	}
}

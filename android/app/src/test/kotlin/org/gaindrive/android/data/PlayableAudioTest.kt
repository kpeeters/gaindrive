package org.gaindrive.android.data

import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.playback.MEDIA3_AUDIO_LOSSLESS
import org.gaindrive.android.playback.MEDIA3_AUDIO_LOSSY
import org.gaindrive.android.playback.playableAudioFor
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The audio half of `playable`: what this device says it will take as it
 * stands, and - the half worth testing - what it must never say.
 *
 * Two failures are being guarded against and they are opposites. Declaring too
 * little wastes a re-encode, which is slow and invisible. Declaring too much
 * hands someone who asked for Opus 160 a lossless file over mobile data, and
 * hands a Cast receiver a container its `LOAD` did not announce - which it
 * refuses outright, on a television, with nothing on the phone to say why.
 */
class PlayableAudioTest {

	private val opus160 = AudioQuality(AudioFormat.OPUS, 160)

	/**
	 * The rule the feature turns on, and the one a future edit is most likely
	 * to undo by adding a token to the wrong set.
	 */
	@Test
	fun `a lossy setting declares nothing lossless`() {
		val declared = playableAudioFor(opus160)
		MEDIA3_AUDIO_LOSSLESS.forEach {
			assertFalse("declared $it at Opus 160", declared.contains(it))
		}
		assertEquals(MEDIA3_AUDIO_LOSSY, declared)
	}

	@Test
	fun `original declares the lossless formats too`() {
		val declared = playableAudioFor(AudioQuality.ORIGINAL)
		assertTrue(declared.containsAll(MEDIA3_AUDIO_LOSSLESS))
		assertTrue(declared.containsAll(MEDIA3_AUDIO_LOSSY))
	}

	/**
	 * ALAC and FLAC-in-Ogg are lossless in exactly the way `.flac` is, and
	 * declaring either at a lossy setting is the same mistake wearing a
	 * different extension. Named individually because the sets above could be
	 * edited to agree with each other and still be wrong.
	 */
	@Test
	fun `alac and ogg flac are treated as lossless`() {
		assertTrue(MEDIA3_AUDIO_LOSSLESS.contains("mp4/alac"))
		assertTrue(MEDIA3_AUDIO_LOSSLESS.contains("ogg/flac"))
		assertTrue(MEDIA3_AUDIO_LOSSLESS.contains("flac/flac"))
		assertFalse(playableAudioFor(opus160).contains("mp4/alac"))
		assertFalse(playableAudioFor(opus160).contains("ogg/flac"))
	}

	/**
	 * `cappedBy` turns a request for the original into mp3 at the cap, so
	 * reading the capped value would silently stop declaring lossless for every
	 * capped account - the one group whose originals the server was going to
	 * convert anyway, but for a reason that has nothing to do with this.
	 */
	@Test
	fun `the account cap is not what the declaration reads`() {
		val capped = AudioQuality.ORIGINAL.cappedBy(192)
		assertEquals(AudioFormat.MP3, capped.format)
		val uncapped = playableAudioFor(AudioQuality.ORIGINAL)
		assertTrue(uncapped.containsAll(MEDIA3_AUDIO_LOSSLESS))
		assertFalse(playableAudioFor(capped).containsAll(MEDIA3_AUDIO_LOSSLESS))
	}

	/**
	 * "The original" for a film played as audio is the film, which is why
	 * `forVideoAudio` substitutes before this is asked - and why asking after
	 * it gives the lossy set even for someone whose setting says Original.
	 */
	@Test
	fun `a film played as audio declares nothing lossless`() {
		val asked = AudioQuality.ORIGINAL.forVideoAudio()
		assertEquals(MEDIA3_AUDIO_LOSSY, playableAudioFor(asked))
	}

	/**
	 * media3 has no Speex decoder, and the server refuses wav and wma in any
	 * form. Declaring any of the three would be a claim nothing acts on at
	 * best, and silence at worst.
	 */
	@Test
	fun `speex wav and wma are never declared`() {
		val everything = MEDIA3_AUDIO_LOSSY + MEDIA3_AUDIO_LOSSLESS
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
		val everything = MEDIA3_AUDIO_LOSSY + MEDIA3_AUDIO_LOSSLESS
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
		val everything = MEDIA3_AUDIO_LOSSY + MEDIA3_AUDIO_LOSSLESS
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
		(MEDIA3_AUDIO_LOSSY + MEDIA3_AUDIO_LOSSLESS).forEach {
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

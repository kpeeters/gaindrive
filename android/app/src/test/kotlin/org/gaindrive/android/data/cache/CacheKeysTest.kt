package org.gaindrive.android.data.cache

import org.gaindrive.android.data.model.AudioFormat
import org.gaindrive.android.data.model.AudioQuality
import org.gaindrive.android.data.model.ItemRef
import org.gaindrive.android.data.model.ServerId
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotEquals
import org.junit.Assert.assertNull
import org.junit.Test

/**
 * The failure this guards against is silent: a quality suffix reaching the DAO
 * queries that match `serverId || '/' || id` returns an empty list rather than
 * throwing, and the symptom is offline availability marks disappearing with
 * nothing in the log.
 */
class CacheKeysTest {

	private val ref = ItemRef(ServerId("3f2b7c10-0000-4000-8000-000000000001"), "42")

	@Test
	fun `ref key survives a round trip through a cache key`() {
		val key = CacheKeys.of(ref, AudioQuality(AudioFormat.OPUS, 160))
		assertEquals(ref.encode(), CacheKeys.refKeyOf(key))
		assertEquals(ref, ItemRef.decode(CacheKeys.refKeyOf(key)))
	}

	@Test
	fun `quality is recoverable from a cache key`() {
		val quality = AudioQuality(AudioFormat.OPUS, 160)
		val key = CacheKeys.of(ref, quality)
		assertEquals(quality, AudioQuality.parse(CacheKeys.tagOf(key)!!))
	}

	@Test
	fun `keys written before quality existed still yield their ref`() {
		assertEquals(ref.encode(), CacheKeys.refKeyOf(ref.encode()))
		assertNull(CacheKeys.tagOf(ref.encode()))
	}

	@Test
	fun `the same track at two qualities is two keys`() {
		val a = CacheKeys.of(ref, AudioQuality(AudioFormat.OPUS, 160))
		val b = CacheKeys.of(ref, AudioQuality(AudioFormat.OPUS, 96))
		assertNotEquals(a, b)
		assertEquals(CacheKeys.refKeyOf(a), CacheKeys.refKeyOf(b))
	}

	@Test
	fun `original is distinguishable from every encoded quality`() {
		val orig = CacheKeys.of(ref, AudioQuality.ORIGINAL)
		assertEquals("orig", CacheKeys.tagOf(orig))
		assertNotEquals(orig, CacheKeys.of(ref, AudioQuality(AudioFormat.OPUS, 160)))
	}
}

class AudioQualityTest {

	@Test
	fun `tags round trip`() {
		listOf(
			AudioQuality.ORIGINAL,
			AudioQuality(AudioFormat.OPUS, 96),
			AudioQuality(AudioFormat.OPUS, 160),
			AudioQuality(AudioFormat.MP3, 192),
		).forEach { assertEquals(it, AudioQuality.parse(it.tag)) }
	}

	@Test
	fun `unknown tags parse to null rather than throwing`() {
		listOf("", "opus", "opusx", "flac128", "160", "opus0").forEach {
			assertNull("expected null for '$it'", AudioQuality.parse(it))
		}
	}

	@Test
	fun `an account cap lowers the bitrate`() {
		val capped = AudioQuality(AudioFormat.OPUS, 256).cappedBy(128)
		assertEquals(AudioQuality(AudioFormat.OPUS, 128), capped)
	}

	@Test
	fun `a cap below nothing leaves the quality alone`() {
		val quality = AudioQuality(AudioFormat.OPUS, 160)
		assertEquals(quality, quality.cappedBy(0))
		assertEquals(quality, quality.cappedBy(320))
	}

	/**
	 * The server answers a capped request for the original with mp3 at the cap,
	 * so a key claiming `orig` would be filed against bytes that are not.
	 */
	@Test
	fun `original under a cap becomes mp3 at the cap`() {
		val capped = AudioQuality.ORIGINAL.cappedBy(192)
		assertEquals(AudioQuality(AudioFormat.MP3, 192), capped)
		assertEquals("mp3192", capped.tag)
	}
}

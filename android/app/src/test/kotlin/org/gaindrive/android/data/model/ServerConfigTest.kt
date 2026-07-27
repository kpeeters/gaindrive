package org.gaindrive.android.data.model

import org.junit.Assert.assertEquals
import org.junit.Test

class ServerConfigTest {

	/** Retrofit needs the base URL to end in '/', which it appends itself. */
	@Test
	fun `trailing slashes are stripped`() {
		assertEquals("https://music.example.com", ServerConfig.normaliseUrl("https://music.example.com/"))
		assertEquals("https://music.example.com", ServerConfig.normaliseUrl("https://music.example.com///"))
		assertEquals("https://music.example.com", ServerConfig.normaliseUrl("  https://music.example.com  "))
	}

	@Test
	fun `name falls back to the host`() {
		assertEquals("music.example.com", ServerConfig.defaultName("https://music.example.com"))
		assertEquals("192.168.1.10", ServerConfig.defaultName("http://192.168.1.10:4040"))
	}

	/** A half-typed URL must still yield something printable, not an exception. */
	@Test
	fun `name falls back sanely for an unparseable url`() {
		assertEquals("not a url", ServerConfig.defaultName("not a url"))
	}
}

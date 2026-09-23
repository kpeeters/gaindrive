package org.gaindrive.android.data.crypto

import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class PairingCipherTest {

	@Test
	fun `what one key encrypts it decrypts`() {
		val key = PairingCipher.newKey()
		val stored = PairingCipher.encrypt(key, "the payload")
		assertEquals("the payload", PairingCipher.decryptOrNull(key, stored))
	}

	@Test
	fun `another key gets nothing`() {
		val stored = PairingCipher.encrypt(PairingCipher.newKey(), "the payload")
		assertNull(PairingCipher.decryptOrNull(PairingCipher.newKey(), stored))
	}

	@Test
	fun `a tampered body gets nothing`() {
		val key = PairingCipher.newKey()
		val stored = PairingCipher.encrypt(key, "the payload")
		// Flipping one character of the ciphertext must fail authentication,
		// not decrypt to garbage: GCM is here for exactly that.
		val body = stored.substringAfter(':')
		val flipped = if (body[0] != 'A') 'A' else 'B'
		val tampered = stored.substringBefore(':') + ":" + flipped + body.drop(1)
		assertNull(PairingCipher.decryptOrNull(key, tampered))
	}

	@Test
	fun `noise gets nothing`() {
		assertNull(PairingCipher.decryptOrNull(PairingCipher.newKey(), "not a payload"))
		assertNull(PairingCipher.decryptOrNull(PairingCipher.newKey(), ""))
	}
}

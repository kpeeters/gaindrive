package org.gaindrive.android.data.crypto

import java.security.SecureRandom
import java.util.Base64
import javax.crypto.Cipher
import javax.crypto.spec.GCMParameterSpec
import javax.crypto.spec.SecretKeySpec
import org.gaindrive.android.data.PAIR_KEY_BYTES

/**
 * AES-GCM for the pairing payload, with a key that travels in the QR code.
 *
 * [CredentialCipher] cannot serve here although the wire format is its
 * `base64(iv):base64(ciphertext)`: its key lives in the AndroidKeyStore,
 * non-exportable and per-device, and pairing is exactly the case where both
 * devices must hold the same key. `java.util.Base64` rather than Android's,
 * so the round trip runs as a plain JVM unit test.
 */
object PairingCipher {

	private val random = SecureRandom()

	fun newKey(): ByteArray = ByteArray(PAIR_KEY_BYTES).also { random.nextBytes(it) }

	/** Returns `base64(iv):base64(ciphertext)`. */
	fun encrypt(key: ByteArray, plain: String): String {
		val cipher = Cipher.getInstance(TRANSFORMATION)
		cipher.init(Cipher.ENCRYPT_MODE, SecretKeySpec(key, "AES"))
		val body = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
		return "${to64(cipher.iv)}:${to64(body)}"
	}

	/**
	 * Null for anything that does not decrypt cleanly - a stale QR's key, a
	 * tampered body, or noise that was never a payload. GCM authenticates, so
	 * null is a verdict and not merely a parse failure.
	 */
	fun decryptOrNull(key: ByteArray, stored: String): String? = runCatching {
		val (iv, body) = stored.split(':', limit = 2).let { it[0] to it[1] }
		val cipher = Cipher.getInstance(TRANSFORMATION)
		cipher.init(
			Cipher.DECRYPT_MODE,
			SecretKeySpec(key, "AES"),
			GCMParameterSpec(TAG_BITS, from64(iv)),
		)
		String(cipher.doFinal(from64(body)), Charsets.UTF_8)
	}.getOrNull()

	private fun to64(b: ByteArray): String = Base64.getEncoder().encodeToString(b)
	private fun from64(s: String): ByteArray = Base64.getDecoder().decode(s)

	private const val TRANSFORMATION = "AES/GCM/NoPadding"
	private const val TAG_BITS = 128
}

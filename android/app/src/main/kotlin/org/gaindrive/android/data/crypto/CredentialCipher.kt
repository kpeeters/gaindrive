package org.gaindrive.android.data.crypto

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey
import javax.crypto.spec.GCMParameterSpec
import javax.inject.Inject
import javax.inject.Singleton

/**
 * Encrypts server passwords at rest with an AES-256-GCM key held in the
 * AndroidKeyStore.
 *
 * The key deliberately does not require user authentication: playback and
 * background sync have to work while the device is locked.
 *
 * `androidx.security:security-crypto` is not used - it is deprecated, and the
 * direct Keystore path is this file.
 */
@Singleton
class CredentialCipher @Inject constructor() {

	private fun key(): SecretKey {
		val store = KeyStore.getInstance(PROVIDER).apply { load(null) }
		(store.getEntry(ALIAS, null) as? KeyStore.SecretKeyEntry)?.let { return it.secretKey }

		val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, PROVIDER)
		generator.init(
			KeyGenParameterSpec.Builder(
				ALIAS,
				KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT,
			)
				.setBlockModes(KeyProperties.BLOCK_MODE_GCM)
				.setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
				.setKeySize(256)
				.setUserAuthenticationRequired(false)
				.build()
		)
		return generator.generateKey()
	}

	/** Returns `base64(iv):base64(ciphertext)`. */
	fun encrypt(plain: String): String {
		val cipher = Cipher.getInstance(TRANSFORMATION)
		cipher.init(Cipher.ENCRYPT_MODE, key())
		val body = cipher.doFinal(plain.toByteArray(Charsets.UTF_8))
		return "${body64(cipher.iv)}:${body64(body)}"
	}

	/**
	 * Null when the stored value cannot be decrypted - which happens when the
	 * Keystore key is gone but the ciphertext survived, e.g. after a restore
	 * onto another device. The caller's job is to ask for the password again,
	 * not to crash.
	 */
	fun decryptOrNull(stored: String): String? = runCatching {
		val (iv, body) = stored.split(':', limit = 2).let { it[0] to it[1] }
		val cipher = Cipher.getInstance(TRANSFORMATION)
		cipher.init(Cipher.DECRYPT_MODE, key(), GCMParameterSpec(TAG_BITS, from64(iv)))
		String(cipher.doFinal(from64(body)), Charsets.UTF_8)
	}.getOrNull()

	private fun body64(b: ByteArray) = Base64.encodeToString(b, Base64.NO_WRAP)
	private fun from64(s: String) = Base64.decode(s, Base64.NO_WRAP)

	private companion object {
		const val PROVIDER = "AndroidKeyStore"
		const val ALIAS = "gaindrive.credentials"
		const val TRANSFORMATION = "AES/GCM/NoPadding"
		const val TAG_BITS = 128
	}
}

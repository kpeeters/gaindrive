package org.gaindrive.android.playback

import android.content.Context
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.media3.common.util.BitmapLoader
import coil3.SingletonImageLoader
import coil3.request.ImageRequest
import coil3.request.allowHardware
import coil3.request.SuccessResult
import coil3.toBitmap
import com.google.common.util.concurrent.ListenableFuture
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.guava.future
import org.gaindrive.android.data.Connectivity
import org.gaindrive.android.data.cache.artCaching

/**
 * Notification and lockscreen artwork, loaded through Coil like everything
 * else on screen.
 *
 * Without this Media3 falls back to `DataSourceBitmapLoader`, which fetches
 * the `artworkUri` over an HTTP data source of its own with no cache behind
 * it. That is a third copy of a picture the app has already downloaded twice,
 * and offline it is a guaranteed blank sleeve even for a pinned album whose
 * cover is sitting in `filesDir`.
 *
 * Going through Coil means `artCaching` applies, so the notification shares
 * the memory cache, the disk cache and the pinned files with the UI.
 */
class CoilBitmapLoader(
	private val context: Context,
	private val scope: CoroutineScope,
	private val connectivity: Connectivity,
) : BitmapLoader {

	override fun supportsMimeType(mimeType: String): Boolean =
		mimeType.startsWith("image/")

	override fun decodeBitmap(data: ByteArray): ListenableFuture<Bitmap> =
		scope.future {
			BitmapFactory.decodeByteArray(data, 0, data.size)
				?: error("Could not decode embedded artwork")
		}

	override fun loadBitmap(uri: Uri): ListenableFuture<Bitmap> = scope.future {
		val url = uri.toString()
		val request = ImageRequest.Builder(context)
			.data(url)
			.artCaching(url, connectivity.online.value)
			.size(ARTWORK_PX)
			// A hardware bitmap has no pixels to read back, and this one is
			// parcelled out to notification and lockscreen controllers.
			.allowHardware(false)
			.build()
		when (val result = SingletonImageLoader.get(context).execute(request)) {
			is SuccessResult -> result.image.toBitmap()
			else -> error("No artwork for $url")
		}
	}
}

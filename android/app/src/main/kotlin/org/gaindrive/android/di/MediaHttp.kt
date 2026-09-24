package org.gaindrive.android.di

import javax.inject.Qualifier

/**
 * The OkHttp client for media bytes, as opposed to API calls and cover art.
 *
 * It differs from the shared one in a single respect - a read timeout measured
 * in minutes - and that is not tuning, it is what makes several features work
 * at all. gaindrive's transcode cache is **blocking**: it runs ffmpeg to a file
 * and sends nothing at all until the file is complete, which buys a real
 * `Content-Length`, byte ranges and correct container headers. For a music
 * track that costs a few seconds. For a whole film - a `-c copy` remux, or the
 * soundtrack extracted for `SettingsStore.videoAudioOnly` - it is minutes, and
 * every one of them passes before the first byte of the response arrives.
 *
 * Against the shared client's 30 s read timeout, the request simply fails.
 * There is nothing to see in a log except a timeout, and the failure looks like
 * the server being broken rather than the server being busy.
 *
 * Derived from the shared client rather than built beside it, so it keeps the
 * connection pool, the User-Agent interceptor and the HTTP cache; Retrofit and
 * Coil keep the short timeout, which is right for them.
 */
// The targets are explicit, and leaving them out is a silent bug rather than a
// compile error. Kotlin picks a use site from the applicable set in the order
// parameter, property, field - so an annotation that also targets PROPERTY
// lands on the property, where Dagger (which reads the Java view) cannot see
// it. Field injection then quietly receives the unqualified client, and the
// only symptom is a read timeout on the first play of a large file.
@Qualifier
@Retention(AnnotationRetention.BINARY)
@Target(
	AnnotationTarget.FIELD,
	AnnotationTarget.FUNCTION,
	AnnotationTarget.VALUE_PARAMETER,
)
annotation class MediaHttp

/**
 * Generous, because the cost of being wrong is asymmetric: too long only delays
 * noticing a connection that has died mid-stream, while too short breaks the
 * first play of every large file outright.
 */
const val MEDIA_READ_TIMEOUT_MINUTES = 10L

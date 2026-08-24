package org.gaindrive.android.net

import kotlinx.serialization.Serializable

/**
 * DTOs for the URL-fetch endpoints — the gaindrive extension that has the
 * server download a pasted URL into the user's own upload area. Shaped to what
 * `src/gaindrive.cc` emits; the wire contract is in `API.md`.
 *
 * As in [BrowseDto.kt], everything is optional with a default. The server
 * always writes real JSON arrays, so the single-element collapsing that bites
 * other Subsonic implementations does not arise here — but the defaults mean a
 * server that omitted a container entirely would still parse.
 */

@Serializable
data class UrlHandlerDto(
	val name: String = "",
	val audio: Boolean = false,
	val video: Boolean = false,
)

/**
 * One fetch job.
 *
 * [state] is left a plain string rather than an enum: the server's set is
 * `queued | running | scanning | done | error | cancelled`, and a newer one
 * adding to it must not fail the whole response. [FetchState] is where it is
 * given meaning.
 *
 * [percent] is the tool's own progress and is deliberately allowed to stall —
 * the server keeps the last figure rather than resetting to zero for a line
 * carrying no percentage, because post-processing is the slowest visible part
 * of an audio fetch and a bar snapping back to 0 there reads as a failure.
 */
@Serializable
data class FetchJobDto(
	val id: String = "",
	/** Stored-form path of the batch: `<uploads root>/<user>/<uuid>`. */
	val batch: String = "",
	val handler: String = "",
	/** "audio" or "video". */
	val mode: String = "",
	val artist: String = "",
	val album: String = "",
	val url: String = "",
	val state: String = "",
	val percent: Int = 0,
	/** The tool's last output line, with server paths rewritten. */
	val detail: String = "",
	val error: String = "",
	val files: Int = 0,
	val started: Long = 0,
	val finished: Long = 0,
)

@Serializable
data class UrlHandlersContainer(
	val urlHandler: List<UrlHandlerDto> = emptyList(),
)

@Serializable
data class FetchJobsContainer(
	val fetchJob: List<FetchJobDto> = emptyList(),
)

// ── Response bodies ─────────────────────────────────────────────────────────

@Serializable
data class GetUrlHandlersBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val urlHandlers: UrlHandlersContainer? = null,
) : SubsonicBody

@Serializable
data class FetchUrlBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	/** The job as queued. Its `id` is the only way to follow this one job. */
	val fetchJob: FetchJobDto? = null,
) : SubsonicBody

@Serializable
data class GetFetchJobsBody(
	override val status: String = "failed",
	override val error: SubsonicError? = null,
	val fetchJobs: FetchJobsContainer? = null,
) : SubsonicBody

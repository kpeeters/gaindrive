//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

//	The uploads endpoints: having the server fetch a pasted URL into the
//	account's own uploads, and moving or deleting what is there. gaindrive
//	extensions, shaped to what `src/gaindrive.cc` emits; Android's `FetchDto.kt`
//	and `SubsonicApi.kt` hold the same contract. Every field `@Loose` and
//	optional, as everywhere in `Net`.

struct UrlHandlerDto: Decodable, Sendable {
	@Loose var name: String?
	@Loose var audio: Bool?
	@Loose var video: Bool?
}

/// One fetch job.
///
/// `state` stays a string rather than an enum - `queued | running | scanning |
/// done | error | cancelled`, and a newer server adding to it must not fail the
/// response. `FetchState` gives it meaning.
///
/// `percent` is the tool's own progress and is allowed to stall: the server
/// keeps the last figure rather than resetting, because post-processing is the
/// slowest visible part of an audio fetch and a bar snapping to zero there
/// reads as a failure.
struct FetchJobDto: Decodable, Sendable {
	@Loose var id: String?
	@Loose var handler: String?
	/// "audio" or "video".
	@Loose var mode: String?
	@Loose var artist: String?
	@Loose var album: String?
	@Loose var url: String?
	@Loose var state: String?
	@Loose var percent: Int?
	/// The tool's last output line, with server paths rewritten.
	@Loose var detail: String?
	@Loose var error: String?
	@Loose var files: Int?
}

struct GetUrlHandlersBody: Decodable, Sendable {
	struct Container: Decodable, Sendable {
		@Listed var urlHandler: [UrlHandlerDto] = []
	}
	let urlHandlers: Container?
}

struct FetchUrlBody: Decodable, Sendable {
	/// The job as queued. Its `id` is the only way to follow this one job.
	let fetchJob: FetchJobDto?
}

struct GetFetchJobsBody: Decodable, Sendable {
	struct Container: Decodable, Sendable {
		@Listed var fetchJob: [FetchJobDto] = []
	}
	let fetchJobs: Container?
}

extension SubsonicClient {
	/// Which URLs this server can fetch. Needs the upload role: without it the
	/// server answers error 50, and a server too old to know the endpoint an
	/// error of its own - so this one call decides whether a server can be
	/// offered at all. An empty list means no handler is configured.
	func urlHandlers() async throws -> [UrlHandlerDto] {
		try await perform("getUrlHandlers", expecting: GetUrlHandlersBody.self)
			.urlHandlers?.urlHandler ?? []
	}

	/// Queues a fetch and returns as soon as it is queued.
	///
	/// `artist` and `album` are **omitted, not sent blank**, when nothing was
	/// typed: the server tells "not sent" (keep what the handler parsed from
	/// the title) from a value it cannot use, which is error 10.
	func fetchUrl(url: String, audio: Bool, artist: String?, album: String?) async throws
		-> FetchJobDto?
	{
		var parameters = ["url": url, "mode": audio ? "audio" : "video"]
		if let artist { parameters["artist"] = artist }
		if let album { parameters["album"] = album }
		return try await perform("fetchUrl", parameters: parameters, expecting: FetchUrlBody.self)
			.fetchJob
	}

	/// The caller's own jobs, newest first, kept briefly after they end.
	func fetchJobs() async throws -> [FetchJobDto] {
		try await perform("getFetchJobs", expecting: GetFetchJobsBody.self)
			.fetchJobs?.fetchJob ?? []
	}

	func cancelFetch(id: String) async throws {
		_ = try await perform("cancelFetch", parameters: ["id": id], expecting: EmptyBody.self)
	}

	/// Moves an upload into the shared library. Admin only, which the server
	/// enforces. **Both halves of the destination are required**: each was
	/// briefly optional on the server with a default that filed things wrongly.
	/// `folder` is the level under the root - an artist, or a category - and
	/// is created if it does not exist.
	func moveAlbum(id: String, musicFolderId: String, folder: String) async throws {
		_ = try await perform(
			"moveAlbum", parameters: ["id": id, "musicFolderId": musicFolderId, "folder": folder],
			expecting: EmptyBody.self)
	}

	/// Removes one of the caller's own uploads, files and all. Owner only;
	/// the server refuses anything not under this account's uploads.
	func deleteUpload(id: String) async throws {
		_ = try await perform("deleteUpload", parameters: ["id": id], expecting: EmptyBody.self)
	}
}

/// Where a job has got to, mapped from the server's string so a state a newer
/// server invents shows as `unknown` instead of failing the response.
enum FetchState: Sendable {
	case queued, running, scanning, done, error, cancelled, unknown

	init(_ raw: String?) {
		switch raw {
		case "queued": self = .queued
		case "running": self = .running
		case "scanning": self = .scanning
		case "done": self = .done
		case "error": self = .error
		case "cancelled": self = .cancelled
		default: self = .unknown
		}
	}

	var isLive: Bool { self == .queued || self == .running || self == .scanning }

	/// Only these two may be cancelled; the server refuses the rest.
	var isCancellable: Bool { self == .queued || self == .running }

	var label: String {
		switch self {
		case .queued: "Waiting"
		case .running: "Fetching"
		case .scanning: "Adding to the library"
		case .done: "Done"
		case .error: "Failed"
		case .cancelled: "Cancelled"
		case .unknown: "Unknown"
		}
	}
}

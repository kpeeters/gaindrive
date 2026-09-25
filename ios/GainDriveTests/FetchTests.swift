//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing

@testable import GainDrive

/// The URL fetch: reading jobs, telling when one lands, and taking a link out
/// of whatever was pasted.
struct FetchTests {
	private let server = ServerId()

	private func job(_ id: String, _ state: String) throws -> FetchJob {
		let body = try SubsonicClient.decode(
			Data(
				#"{"subsonic-response":{"status":"ok","fetchJob":{"id":"\#(id)","state":"\#(state)","mode":"audio","url":"https://example.org/x","percent":40}}}"#
					.utf8),
			expecting: FetchUrlBody.self, httpStatus: 200)
		let dto = try #require(body.fetchJob)
		return FetchJob(server: server, serverName: "Home", dto: dto)
	}

	@Test func aJobDecodes() throws {
		let decoded = try job("7", "running")
		#expect(decoded.jobId == "7")
		#expect(decoded.state == .running)
		#expect(decoded.audio)
		#expect(decoded.percent == 40)
	}

	@Test func aListOfJobsDecodes() throws {
		let body = try SubsonicClient.decode(
			Data(
				#"{"subsonic-response":{"status":"ok","fetchJobs":{"fetchJob":[{"id":"1","state":"done"},{"id":"2","state":"queued"}]}}}"#
					.utf8),
			expecting: GetFetchJobsBody.self, httpStatus: 200)
		#expect(body.fetchJobs?.fetchJob.count == 2)
	}

	/// A state a newer server invents must not fail anything.
	@Test func statesMapAndUnknownIsTolerated() {
		#expect(FetchState("scanning").isLive)
		#expect(FetchState("queued").isCancellable)
		#expect(!FetchState("scanning").isCancellable)
		#expect(!FetchState("done").isLive)
		#expect(FetchState("paused") == .unknown)
		#expect(FetchState(nil) == .unknown)
	}

	/// Noticed once, on the transition, and never for a job first seen done -
	/// or every finished job the server still lists would count at launch.
	@Test func aCompletionIsNoticedOnceOnTheTransition() throws {
		var seen: [String: FetchState] = [:]
		#expect(!Fetches.completed(since: &seen, in: [try job("1", "done")]))
		#expect(!Fetches.completed(since: &seen, in: [try job("2", "running")]))
		#expect(Fetches.completed(since: &seen, in: [try job("2", "done")]))
		#expect(!Fetches.completed(since: &seen, in: [try job("2", "done")]))
	}

	/// A shared YouTube item is a title, then the link on the next line.
	@Test func theFirstLinkIsTakenFromPastedText() {
		#expect(
			FetchUrlView.firstLink(in: "Some video title\nhttps://youtu.be/abc123")
				== "https://youtu.be/abc123")
		#expect(FetchUrlView.firstLink(in: "  https://example.org/a  ") == "https://example.org/a")
		#expect(FetchUrlView.firstLink(in: "no link here") == "no link here")
	}
}

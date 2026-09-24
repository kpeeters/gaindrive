//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Testing
import UniformTypeIdentifiers

@testable import GainDrive

/// Reading a file that is still arriving.
///
/// The whole of the cache's bookkeeping is one integer, and this is the
/// arithmetic over it. Every failure here is a stall rather than an error: a
/// read served short when it should have been held, or held when it could have
/// been served, is a player that waits with nothing to say.
struct PartialReadTests {
	@Test func nothingIsServedBeforeAnythingHasArrived() {
		#expect(PartialRead.chunk(currentOffset: 0, end: 100, received: 0) == nil)
	}

	@Test func aReadBelowWhatArrivedIsServedWhole() {
		#expect(PartialRead.chunk(currentOffset: 0, end: 50, received: 100) == 0..<50)
	}

	/// **Served short rather than refused**, which is what keeps playback
	/// moving while the rest arrives. The request stays open and is topped up.
	@Test func aReadStraddlingTheBoundaryIsServedUpToIt() {
		#expect(PartialRead.chunk(currentOffset: 40, end: 200, received: 100) == 40..<100)
	}

	@Test func aReadEntirelyAheadIsHeld() {
		#expect(PartialRead.chunk(currentOffset: 100, end: 200, received: 100) == nil)
	}

	/// **The read the whole thing hung on.** The server writes MP4 with its
	/// index at the end, so AVFoundation asks for the tail early and that
	/// request cannot be answered until the last byte lands - it has to become
	/// satisfiable the moment it does.
	///
	/// Worth being clear that this test would not have caught the bug: the
	/// arithmetic was right, and what was wrong is that the loader stopped
	/// serving when the download finished. It is here because the case is the
	/// one to think about first when this stalls again.
	@Test func theTailBecomesReadableOnceEverythingHasArrived() {
		#expect(PartialRead.chunk(currentOffset: 990, end: 1000, received: 500) == nil)
		#expect(PartialRead.chunk(currentOffset: 990, end: 1000, received: 1000) == 990..<1000)
	}

	@Test func aReadAlreadyAnsweredAsksForNothing() {
		#expect(PartialRead.chunk(currentOffset: 50, end: 50, received: 100) == nil)
		#expect(PartialRead.isSatisfied(currentOffset: 50, end: 50))
		#expect(!PartialRead.isSatisfied(currentOffset: 49, end: 50))
	}

	/// **`requestsAllDataToEndOfResource` means the end**, and the requested
	/// length is a nominal figure that must not be believed. Falling for it
	/// truncates the last read of every track, which plays and then stops
	/// short.
	@Test func aReadToTheEndIgnoresTheStatedLength() {
		#expect(
			PartialRead.end(
				requestedOffset: 10, requestedLength: 1, toEnd: true, total: 900) == 900)
	}

	@Test func anOrdinaryReadUsesItsStatedLength() {
		#expect(
			PartialRead.end(
				requestedOffset: 10, requestedLength: 40, toEnd: false, total: 900) == 50)
	}

	/// Before the length is known there is nothing better to go on.
	@Test func aReadToTheEndOfAnUnknownLengthFallsBack() {
		#expect(
			PartialRead.end(
				requestedOffset: 10, requestedLength: 40, toEnd: true, total: nil) == 50)
	}
}

/// What gives way when the cap is reached.
struct EvictionTests {
	private func victim(_ name: String, _ size: Int64, _ age: TimeInterval) -> AudioStore.Victim {
		AudioStore.Victim(
			url: URL(filePath: "/tmp/\(name)"), size: size,
			modified: Date(timeIntervalSince1970: age))
	}

	@Test func nothingGoesWhileThereIsRoom() {
		let files = [victim("a", 10, 1), victim("b", 10, 2)]
		#expect(AudioStore.victims(files, total: 20, cap: 100).isEmpty)
	}

	/// Oldest first, and **only as many as it takes**: a cache that emptied
	/// itself every time it filled would re-fetch everything on the next play.
	@Test func theOldestGoFirstAndOnlyEnoughOfThem() {
		let files = [victim("new", 10, 300), victim("old", 10, 100), victim("mid", 10, 200)]
		let chosen = AudioStore.victims(files, total: 30, cap: 15)
		#expect(chosen.map(\.lastPathComponent) == ["old", "mid"])
	}

	/// The caller passes only what may go, so an empty list over the cap is a
	/// library that is entirely pinned - which is a refusal to pin more, not a
	/// reason to delete something.
	@Test func nothingEvictableMeansNothingEvicted() {
		#expect(AudioStore.victims([], total: 500, cap: 100).isEmpty)
	}
}

/// Reading the layout back.
struct StoreLayoutTests {
	private let server = ServerId()

	@Test func aStoredFileNamesItsTrack() {
		let ref = ItemRef(server: server, id: "4212")
		let url = URL(filePath: "/tmp/Media/\(server.description)/4212@m4a160.m4a")
		#expect(AudioStore.ref(of: url) == ref)
	}

	/// **The transformation has to be the same in both directions.** An id that
	/// was percent-encoded on the way in and not decoded on the way out reads
	/// as a different track, and every such file counts as absent for ever.
	@Test func anEncodedIdIsDecodedBack() {
		let ref = ItemRef(server: server, id: "a/b")
		let key = CacheKeys.of(ref, quality: .original)
		let stored = AudioStore.path(root: URL(filePath: "/tmp/Media"), key: key)
			.appendingPathExtension("flac")
		#expect(AudioStore.ref(of: stored) == ref)
	}

	@Test func somethingThatIsNotOursNamesNothing() {
		#expect(AudioStore.ref(of: URL(filePath: "/tmp/Media/notauuid/1@m4a160.m4a")) == nil)
		#expect(AudioStore.ref(of: URL(filePath: "/tmp/Media/\(server)/noatsign.m4a")) == nil)
	}
}

/// What AVFoundation is told the bytes are.
struct ContentTypeTests {
	/// **A UTI, not a MIME type.** AVFoundation takes a MIME type here without
	/// complaint and then plays nothing - the same silent stall a file stored
	/// without an extension produces, and with as little to catch.
	@Test func aKnownFormatAnswersAUti() {
		let uti = CachingResourceLoader.uti(
			for: nil, quality: AudioQuality(format: .m4a, bitRate: 160))
		#expect(uti != nil)
		#expect(uti?.contains("/") == false)
	}

	/// The original's container is whatever the server holds, so with no
	/// response to read there is nothing honest to claim.
	@Test func theOriginalNeedsTheResponseToSayAnything() {
		#expect(CachingResourceLoader.uti(for: nil, quality: .original) == nil)
	}

	/// **The response outranks the requested quality**, and that order is the
	/// recent one. Since a request may declare what it takes as it stands, an
	/// AAC 160 request can be answered with the MP3 the server already holds;
	/// telling AVFoundation `audio/mp4` over those bytes plays nothing, with
	/// no error either side.
	@Test func theResponseOutranksTheRequestedQuality() {
		#expect(
			CachingResourceLoader.uti(
				for: mp3Response, quality: AudioQuality(format: .m4a, bitRate: 160))
				== UTType.mp3.identifier)
	}

	/// The same inversion where it fails worst: a stored file is typed by its
	/// path extension, and a wrong one is not an error but a track that never
	/// starts.
	@Test func aStoredFileIsNamedByItsResponse() {
		#expect(
			DownloadQueue.fileExtension(
				for: mp3Response, quality: AudioQuality(format: .m4a, bitRate: 160))
				== "mp3")
	}

	/// With nothing to read, the format is still the best answer available -
	/// the fallback the inversion above kept rather than replaced.
	@Test func withNoResponseTheFormatStillNames() {
		#expect(
			DownloadQueue.fileExtension(
				for: nil, quality: AudioQuality(format: .m4a, bitRate: 160))
				== "m4a")
	}

	private var mp3Response: URLResponse {
		URLResponse(
			url: URL(string: "https://example.invalid/rest/stream.view")!,
			mimeType: "audio/mpeg", expectedContentLength: -1, textEncodingName: nil)
	}
}

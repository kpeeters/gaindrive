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

/// The two pure halves of chapters: which marker is playing, and what the album
/// listing turns into once a recording's markers stand in for its row.
///
/// Both are rules the web client and the Android app also implement, so a
/// disagreement here is three clients disagreeing about which song is playing.
struct ChapterTests {
	private let server = ServerId()

	private func chapter(_ index: Int, at start: Double, _ name: String = "") -> Chapter {
		Chapter(index: index, start: start, duration: 0, name: name)
	}

	private var set: [Chapter] {
		[chapter(1, at: 0), chapter(2, at: 100), chapter(3, at: 250)]
	}

	private func song(_ id: String, title: String = "T", track: Int? = nil, disc: Int? = nil)
		-> Song
	{
		Song(
			ref: ItemRef(server: server, id: id), title: title, artistName: "A",
			albumTitle: "B", albumRef: nil, track: track, discNumber: disc, year: nil,
			duration: 400, bitRate: nil, suffix: nil, contentType: nil, sizeBytes: 0,
			coverArt: nil, starredAt: nil, lastPlayedAt: nil, isVideo: false,
			nativeSeek: false, width: nil, height: nil)
	}

	// MARK: - Which marker is playing

	@Test func nothingIsPlayingBeforeTheFirstMarker() {
		// Every list here starts at 0, so this is a list that does not.
		let late = [chapter(1, at: 30), chapter(2, at: 90)]
		#expect(late.currentIndex(at: 10) == nil)
		#expect(late.currentIndex(at: 30) == 0)
	}

	@Test func aMarkerCountsAsReachedSlightlyEarly() {
		// The whole reason the tolerance exists: seeking to 100 frequently
		// lands a few milliseconds short, and without it the list would
		// highlight the previous song for a moment after jumping to one.
		#expect(set.currentIndex(at: 99.9) == 1)
		#expect(set.currentIndex(at: 99.5) == 0)
	}

	@Test func theLastMarkerRunsToTheEnd() {
		#expect(set.currentIndex(at: 10_000) == 2)
	}

	@Test func nextStepsForwardAndRunsOut() {
		#expect(set.next(after: 0)?.index == 2)
		#expect(set.next(after: 150)?.index == 3)
		#expect(set.next(after: 300) == nil)
	}

	@Test func previousRestartsTheMarkerBeingPlayed() {
		// Deep into chapter 2, "previous" means "start this song again" — the
		// behaviour every physical transport has.
		#expect(set.previousTarget(from: 200) == 100)
	}

	@Test func previousTwiceReachesTheMarkerBefore() {
		// Just after chapter 2 began, it means the one before instead.
		#expect(set.previousTarget(from: 101) == 0)
	}

	@Test func previousIsNeverInert() {
		// Before the first marker there is no current one, and the control
		// still has to do something.
		#expect([chapter(1, at: 30)].previousTarget(from: 5) == 0)
	}

	// MARK: - Names

	@Test func anEmptyNameGetsThePlaceholderOnlyWhenDrawn() {
		let bare = chapter(3, at: 0)
		// The stored value is left exactly as the file holds it, or a client
		// saving back what it read would write the placeholder into a line
		// somebody deliberately left blank.
		#expect(bare.name.isEmpty)
		#expect(bare.displayName == "Chapter 3")
		#expect(chapter(3, at: 0, "Encore").displayName == "Encore")
	}

	// MARK: - The wire

	private func decode<Body: Decodable & Sendable>(
		_ json: String, expecting type: Body.Type
	) throws -> Body {
		try SubsonicClient.decode(Data(json.utf8), expecting: type, httpStatus: 200)
	}

	@Test func aFractionalStartSurvivesTheWire() throws {
		// The first fractional number in the API, and the one field here that
		// must not be rounded: `00:00.5` is half a second, and a client saving
		// back what it read would move every marker it did not touch.
		let body: ChaptersBody = try decode(
			#"""
			{"subsonic-response":{"status":"ok","chapters":{"id":"12","source":"sidecar",
			 "writable":true,"chapter":[{"index":1,"start":0,"duration":100,"name":"One"},
			 {"index":2,"start":100.5,"duration":90,"name":""}]}}}
			"""#, expecting: ChaptersBody.self)
		let list = try #require(body.chapters.map(LibraryMapper.chapterList))
		#expect(list.source == .sidecar)
		#expect(list.chapters.map(\.start) == [0, 100.5])
		// Empty on the wire is empty in the model. The placeholder is drawn,
		// never stored.
		#expect(list.chapters[1].name.isEmpty)
	}

	@Test func containerMarkersSayWhereTheyCameFrom() throws {
		// Worth carrying, because such a list is not in the scan's index — it
		// appears in the player and not in the album listing, which reads as a
		// bug unless the panel says so.
		let body: ChaptersBody = try decode(
			#"""
			{"subsonic-response":{"status":"ok","chapters":{"id":"12","source":"container",
			 "chapter":{"index":1,"start":0,"duration":10,"name":"Opening"}}}}
			"""#, expecting: ChaptersBody.self)
		let list = try #require(body.chapters.map(LibraryMapper.chapterList))
		// One marker collapsed to an object rather than an array, which every
		// listing endpoint here does.
		#expect(list.chapters.count == 1)
		#expect(list.source == .container)
	}

	@Test func aSearchHitCarriesItsOwnContext() throws {
		let body: Search3Body = try decode(
			#"""
			{"subsonic-response":{"status":"ok","searchResult3":{"chapter":[
			 {"songId":"7","parent":"3","index":4,"start":612.25,"name":"Echoes",
			  "track":"Live at Pompeii","album":"Pompeii","artist":"Pink Floyd"}]}}}
			"""#, expecting: Search3Body.self)
		let result = try #require(body.searchResult3)
		let selection = LibraryMapper.selection(result, server: server)
		#expect(selection.chapters.count == 1)
		// The recording and its album, which is what makes a hit actionable:
		// open the album, start the recording, seek here.
		#expect(selection.chapters[0].songRef.id == "7")
		#expect(selection.chapters[0].albumRef?.id == "3")
		#expect(selection.chapters[0].start == 612.25)
		// And never among the songs, which is the server's own rule.
		#expect(selection.songs.isEmpty)
	}

	@Test func aServerThatWasNotAskedSendsNoChapterArray() throws {
		// What every server answers when `chapterCount` was not sent, and what
		// one too old for the extension always answers.
		let body: Search3Body = try decode(
			#"""
			{"subsonic-response":{"status":"ok","searchResult3":{"song":[]}}}
			"""#, expecting: Search3Body.self)
		let result = try #require(body.searchResult3)
		#expect(LibraryMapper.selection(result, server: server).chapters.isEmpty)
	}

	// MARK: - The album listing

	@Test func anAlbumWithNoChaptersIsUnchanged() {
		let songs = [song("1", track: 1), song("2", track: 2)]
		let rows = albumListRows(songs: songs, chapters: [:])
		#expect(rows.count == 2)
		#expect(rows.allSatisfy { $0.recordingHeading == nil })
		#expect(rows.map(\.queueIndex) == [0, 1])
		if case .track(let number) = rows[0].kind { #expect(number == 1) } else { Issue.record() }
	}

	@Test func aChapteredRecordingIsReplacedByItsMarkers() {
		let concert = song("1", title: "At the Roundhouse")
		let rows = albumListRows(songs: [concert], chapters: [concert.ref: set])
		#expect(rows.count == 3)
		// Its own row is gone: listing it as one file names the file rather
		// than the music.
		#expect(rows.allSatisfy { if case .marker = $0.kind { return true } else { return false } })
		// Every marker queues the recording, which is the only thing here with
		// an id anything can stream.
		#expect(rows.allSatisfy { $0.queueIndex == 0 })
	}

	@Test func oneChapteredRecordingNeedsNoHeading() {
		let concert = song("1", title: "At the Roundhouse")
		let other = song("2", title: "Soundcheck")
		let rows = albumListRows(songs: [concert, other], chapters: [concert.ref: set])
		#expect(rows.allSatisfy { $0.recordingHeading == nil })
	}

	@Test func twoChapteredRecordingsEachGetOne() {
		let first = song("1", title: "Night one")
		let second = song("2", title: "Night two")
		let rows = albumListRows(
			songs: [first, second], chapters: [first.ref: set, second.ref: set])
		// On the first marker of each and nowhere else: a heading introduces
		// the rows below it.
		#expect(rows.map(\.recordingHeading) == ["Night one", nil, nil, "Night two", nil, nil])
	}

	@Test func markersAreIdentifiedApartFromTheirRecording() {
		let concert = song("1")
		let rows = albumListRows(songs: [concert], chapters: [concert.ref: set])
		#expect(Set(rows.map(\.id)).count == rows.count)
		// And never as the song itself, or a marker row would take the playing
		// highlight from the row that owns it.
		#expect(!rows.contains { $0.id == concert.ref.encoded })
	}

	@Test func untaggedAlbumsAreNumberedByPosition() {
		// Every track tagged 1, or none tagged at all, carries no usable
		// numbering — the same rule the web client and Android apply.
		let songs = [song("1", track: 1), song("2", track: 1), song("3")]
		let rows = albumListRows(songs: songs, chapters: [:])
		let numbers = rows.compactMap { row -> Int? in
			if case .track(let number) = row.kind { return number }
			return nil
		}
		#expect(numbers == [1, 2, 3])
	}

	@Test func discsSurviveTheFlattening() {
		let songs = [song("1", disc: 1), song("2", disc: 2)]
		let rows = albumListRows(songs: songs, chapters: [:])
		#expect(rows.map(\.disc) == [1, 2])
	}
}

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

/// Track links in both directions. The same cases as Android's `TrackLinkTest`
/// and `TrackShareTest`, since a link made by one client is opened by another.
struct TrackLinkTests {
	private func link(_ text: String) -> TrackLink? {
		URL(string: text).flatMap(TrackLink.init)
	}

	@Test func aLinkParses() throws {
		let parsed = try #require(link("gaindrive://music.example.org/gd/?track=42&t=90"))
		#expect(parsed.authority == "music.example.org")
		#expect(parsed.path == "/gd/")
		#expect(parsed.trackId == "42")
		#expect(parsed.position == 90)
	}

	@Test func aPortIsPartOfTheAuthority() throws {
		#expect(try #require(link("gaindrive://10.0.0.5:4040/?track=1")).authority == "10.0.0.5:4040")
	}

	/// Not for us: ignored rather than answered with an error.
	@Test func whatIsNotALinkIsNil() {
		#expect(link("https://music.example.org/?track=42") == nil)
		#expect(link("gaindrive://music.example.org/") == nil)
		#expect(link("gaindrive://music.example.org/?track=") == nil)
	}

	@Test func aMissingOrBadTimeIsZero() throws {
		#expect(try #require(link("gaindrive://h/?track=1")).position == 0)
		#expect(try #require(link("gaindrive://h/?track=1&t=abc")).position == 0)
		#expect(try #require(link("gaindrive://h/?track=1&t=-5")).position == 0)
	}

	@Test func theServerIsMatchedByAuthorityAndPath() throws {
		let parsed = try #require(link("gaindrive://Music.Example.org/gd/?track=1"))
		#expect(parsed.matches(configURL: "https://music.example.org/gd"))
		#expect(parsed.matches(configURL: "https://music.example.org"))
		#expect(!parsed.matches(configURL: "https://music.example.org/other"))
		#expect(!parsed.matches(configURL: "https://elsewhere.example.org/gd"))
	}

	/// Two instances under different subpaths of one host stay distinct.
	@Test func aSiblingSubpathIsNotAMatch() throws {
		let parsed = try #require(link("gaindrive://h/gd2/?track=1"))
		#expect(!parsed.matches(configURL: "https://h/gd"))
	}

	@Test func shareLinksAreTheWebClientsShape() {
		#expect(
			TrackLink.shareURL(serverURL: "https://h/gd", trackId: "42")?.absoluteString
				== "https://h/gd/?track=42")
		#expect(
			TrackLink.shareURL(serverURL: "https://h", trackId: "42", seconds: 90)?.absoluteString
				== "https://h/?track=42&t=90")
		#expect(
			TrackLink.shareURL(serverURL: "https://h", trackId: "42", seconds: 6491.238)?
				.absoluteString == "https://h/?track=42&t=6491.238")
	}

	/// The last marker at or before the position, and none for one at 0.
	@Test func theChapterUnderThePositionIsFound() {
		let chapters = [
			Chapter(index: 1, start: 0, duration: 0, name: "Intro"),
			Chapter(index: 2, start: 120.5, duration: 0, name: "Song"),
		]
		#expect(TrackLink.chapter(in: chapters, at: 200)?.index == 2)
		#expect(TrackLink.chapter(in: chapters, at: 30) == nil)
	}
}

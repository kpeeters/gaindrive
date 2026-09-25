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

/// Reading a WiiM and building commands for it. The bodies are the ones a WiiM
/// Amp actually sent, recorded in `android/WIIM.md`; Android's `WiiMEqTest`
/// holds the same cases.
struct WiiMEqTests {
	private func bandBody(_ values: [Int], stat: String = "On", name: String = "Rock") -> String {
		let entries = values.enumerated().map { index, value in
			#"{"index":\#(index),"param_name":"\#(WiiMEq.bands[index].name)","value":\#(value)}"#
		}
		return #"{"status":"OK","EQStat":"\#(stat)","Name":"\#(name)","EQBand":[\#(entries.joined(separator: ","))]}"#
	}

	@Test func aFullBandResponseGivesSwitchPresetAndFaders() {
		let values = Array(40..<50)
		let state = WiiMEq.parseBand(bandBody(values))
		#expect(state == WiiMEqState(enabled: true, preset: "Rock", bands: values))
	}

	@Test func offIsReadAsOff() {
		#expect(WiiMEq.parseBand(#"{"status":"OK","EQStat":"Off","Name":"Rock"}"#)?.enabled == false)
	}

	/// Matched by name, so a reordered array still reads.
	@Test func bandOrderComesFromTheNamesNotTheIndexes() {
		let entries = (0..<10).reversed().map { i in
			#"{"index":\#(i),"param_name":"\#(WiiMEq.bands[i].name)","value":\#(10 * i)}"#
		}
		let body = #"{"EQStat":"On","EQBand":[\#(entries.joined(separator: ","))]}"#
		#expect(WiiMEq.parseBand(body)?.bands == (0..<10).map { $0 * 10 })
	}

	/// All ten or nothing - but the switch and the preset still come through.
	@Test func aMissingBandWithholdsTheFadersOnly() {
		let body = #"{"EQStat":"On","Name":"Rock","EQBand":[{"param_name":"band31hz","value":50}]}"#
		#expect(WiiMEq.parseBand(body) == WiiMEqState(enabled: true, preset: "Rock", bands: nil))
	}

	@Test func valuesAreClampedToTheDeviceScale() {
		var values = Array(repeating: 50, count: 10)
		values[0] = -5
		values[9] = 140
		let bands = WiiMEq.parseBand(bandBody(values))?.bands
		#expect(bands?.first == WiiMEq.levelMin)
		#expect(bands?.last == WiiMEq.levelMax)
	}

	@Test func statGivesTheSwitchAlone() {
		#expect(WiiMEq.parseStat(#"{"EQStat":"On"}"#) == true)
		#expect(WiiMEq.parseStat(#"{"EQStat":"Off"}"#) == false)
		#expect(WiiMEq.parseStat(#"{"status":"Failed"}"#) == nil)
	}

	/// Bare, as the measured firmware sends it, or wrapped.
	@Test func presetListsAreReadBareOrWrapped() {
		#expect(WiiMEq.parsePresets(#"["Flat","R&B"]"#) == ["Flat", "R&B"])
		#expect(
			WiiMEq.parsePresets(#"{"status":"OK","EQList":["Flat","LotsOfHigh"]}"#)
				== ["Flat", "LotsOfHigh"])
		#expect(WiiMEq.parsePresets("[]") == nil)
		#expect(WiiMEq.parsePresets(#"["", "  "]"#) == nil)
	}

	/// The lesson Android paid for: a real device answers `{"status":"OK"}`.
	@Test func bothSuccessShapesAreOk() {
		#expect(WiiMEq.isOk("OK"))
		#expect(WiiMEq.isOk("OK\n"))
		#expect(WiiMEq.isOk(#"{"status":"OK"}"#))
		#expect(WiiMEq.isOk(#"{"status":"ok","EQStat":"On"}"#))
		#expect(!WiiMEq.isOk("Failed"))
		#expect(!WiiMEq.isOk(#"{"status":"Failed"}"#))
		#expect(!WiiMEq.isOk(#"{"EQStat":"On"}"#))
		#expect(!WiiMEq.isOk("<html>404</html>"))
	}

	@Test func nonsenseIsNilNotACrash() {
		for body in ["", "   ", "<html>404</html>", "{", "null", "42", #"{"EQStat":7}"#] {
			#expect(WiiMEq.parseBand(body) == nil)
			#expect(WiiMEq.parseStat(body) == nil)
			#expect(WiiMEq.parsePresets(body) == nil)
		}
	}

	@Test func setBandCarriesAllTenInTheDeviceShape() throws {
		let command = try #require(WiiMEq.setBand(Array(40..<50)))
		#expect(command.hasPrefix("EQSetBand:"))
		let json = try #require(castJSON(String(command.dropFirst("EQSetBand:".count))))
		let bands = try #require(json["EQBand"] as? [[String: Any]])
		#expect(bands.count == 10)
		for (index, band) in bands.enumerated() {
			#expect(band["index"] as? Int == index)
			#expect(band["param_name"] as? String == WiiMEq.bands[index].name)
			#expect(band["value"] as? Int == 40 + index)
		}
		#expect(WiiMEq.setBand([50]) == nil)
	}

	/// `R&B` unencoded would deliver the device `EQLoad:R`.
	@Test func anAmpersandInAPresetStaysInTheCommand() throws {
		let url = try #require(WiiMEq.url(address: "192.168.1.20", command: WiiMEq.load("R&B")))
		let parts = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
		#expect(parts.queryItems?.count == 1)
		#expect(parts.queryItems?.first?.value == "EQLoad:R&B")
		#expect(url.scheme == "https")
		#expect(url.port == nil)
	}

	@Test func theBandCommandSurvivesTheQueryWhole() throws {
		let command = try #require(WiiMEq.setBand(Array(repeating: 50, count: 10)))
		let url = try #require(WiiMEq.url(address: "192.168.1.20", command: command))
		let parts = try #require(URLComponents(url: url, resolvingAgainstBaseURL: false))
		#expect(parts.queryItems?.count == 1)
		#expect(parts.queryItems?.first?.value == command)
	}

	@Test func anIPv6AddressIsBracketed() throws {
		let url = try #require(WiiMEq.url(address: "fd00::20", command: WiiMEq.getBand))
		#expect(url.absoluteString.hasPrefix("https://[fd00::20]/httpapi.asp"))
	}

	// MARK: - Which preset is ticked

	@Test func aCurveThisAppWroteKeepsItsRecordedName() {
		let bands = Array(40..<50)
		let device = WiiMEqState(enabled: true, preset: "Rock", bands: bands)
		let shown = WiiMControlsModel.shownPreset(
			device: device, last: (bands, "Mine"), presets: ["Rock"], saved: ["Mine"])
		#expect(shown == "Mine")
		// Hand-shaped here: no name, whatever the device still says.
		let none = WiiMControlsModel.shownPreset(
			device: device, last: (bands, nil), presets: ["Rock"], saved: [])
		#expect(none == nil)
	}

	/// Changed elsewhere, so the device's own name is the best answer.
	@Test func aCurveSetElsewhereShowsTheDevicesName() {
		let device = WiiMEqState(enabled: true, preset: "Rock", bands: Array(40..<50))
		let shown = WiiMControlsModel.shownPreset(
			device: device, last: (Array(repeating: 50, count: 10), "Mine"),
			presets: ["Rock"], saved: ["Mine"])
		#expect(shown == "Rock")
	}
}

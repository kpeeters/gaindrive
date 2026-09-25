//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The equalizer state a WiiM reports.
///
/// `preset` is nil when the device would not say which one is loaded, and
/// `bands` when it gave no usable set of ten. Both are real states rather than
/// errors: the switch is still worth showing, and the preset list too.
struct WiiMEqState: Equatable, Sendable {
	var enabled: Bool
	var preset: String?
	/// Fader positions in `WiiMEq.bands` order.
	var bands: [Int]?
}

/// Everything about the WiiM HTTP API that can be decided without a socket:
/// building a command URL and reading a response. `WiiMClient` is the I/O.
///
/// A port of Android's `WiiMEq.kt`; `android/WIIM.md` records what the API
/// does, which of it is measured against a real WiiM Amp, and why each rule
/// below is the shape it is. Bodies are read with tolerant accessors rather
/// than `Codable`, the house rule for JSON from a device we do not control.
enum WiiMEq {
	static let getBand = "EQGetBand"
	static let getStat = "EQGetStat"
	static let getList = "EQGetList"
	static let on = "EQOn"
	static let off = "EQOff"

	static func load(_ preset: String) -> String { "EQLoad:\(preset)" }

	/// The graphic EQ's ten fixed bands: `param_name` and fader label, in
	/// device index order. Fixed rather than read, because `EQSetBand` must
	/// name them and a band this table does not know could not be written.
	static let bands: [(name: String, label: String)] = [
		("band31hz", "31"), ("band63hz", "63"), ("band125hz", "125"),
		("band250hz", "250"), ("band500hz", "500"), ("band1khz", "1k"),
		("band2khz", "2k"), ("band4khz", "4k"), ("band8khz", "8k"),
		("band16khz", "16k"),
	]

	/// The device's fader scale. How 0-99 maps to decibels is unmeasured (the
	/// WiiM app draws ±12 dB), so the sheet shows offsets from flat rather
	/// than dB figures it cannot vouch for.
	static let levelMin = 0
	static let levelMax = 99
	static let levelFlat = 50

	/// The presets WiiM's own PDF documents, used only when `EQGetList` fails
	/// so the sheet is never empty. Not the primary source: firmware adds
	/// presets, and the owner's own ones appear only in the fetched list.
	static let documentedPresets = [
		"Flat", "Acoustic", "Bass Booster", "Bass Reducer", "Classical", "Dance",
		"Deep", "Electronic", "Hip-Hop", "Jazz", "Latin", "Loudness", "Lounge",
		"Piano", "Pop", "R&B", "Rock", "Small Speakers", "Spoken Word",
		"Treble Booster", "Treble Reducer", "Vocal Booster",
	]

	/// `https://<address>/httpapi.asp?command=<command>`, on port 443 and not
	/// the Cast port.
	///
	/// **Assembled by `URLComponents`, never concatenated, and with `&`, `+`
	/// and `=` encoded by hand.** `R&B` is a documented preset: an unencoded
	/// ampersand ends the parameter and delivers the device `EQLoad:R`.
	/// `URLComponents` leaves those three alone in a query value because they
	/// are legal there, so they are encoded explicitly.
	static func url(address: String, command: String) -> URL? {
		var parts = URLComponents()
		parts.scheme = "https"
		// An IPv6 literal has to be bracketed to be a host at all, and the
		// percent-encoded setter is the one that takes the brackets as given.
		parts.percentEncodedHost = address.contains(":") ? "[\(address)]" : address
		parts.path = "/httpapi.asp"
		var allowed = CharacterSet.urlQueryAllowed
		allowed.remove(charactersIn: "&+=")
		guard let encoded = command.addingPercentEncoding(withAllowedCharacters: allowed) else {
			return nil
		}
		parts.percentEncodedQuery = "command=\(encoded)"
		return parts.url
	}

	/// An `EQGetBand` response, which carries the whole state at once:
	///
	///     {"status":"OK","EQStat":"On","Name":"Rock",
	///      "EQBand":[{"index":0,"param_name":"band31hz","value":71}, …]}
	///
	/// The state read, because only this one says *which* preset is loaded.
	static func parseBand(_ body: String) -> WiiMEqState? {
		guard let root = object(body), let stat = root.string("EQStat") else { return nil }
		return WiiMEqState(
			enabled: stat.caseInsensitiveCompare("on") == .orderedSame,
			preset: root.string("Name"),
			bands: parseBands(root))
	}

	/// Matched by `param_name` rather than `index`, so a reordered array still
	/// reads. All ten or nothing: a fader row with a hole has no honest
	/// rendering, and half a curve written back is one nobody shaped.
	private static func parseBands(_ root: [String: Any]) -> [Int]? {
		guard let entries = root.array("EQBand")?.compactMap({ $0 as? [String: Any] }) else {
			return nil
		}
		var byName: [String: Int] = [:]
		for entry in entries {
			if let name = entry.string("param_name"), let value = entry.int("value") {
				byName[name] = value
			}
		}
		var levels: [Int] = []
		for band in bands {
			guard let value = byName[band.name] else { return nil }
			levels.append(min(max(value, levelMin), levelMax))
		}
		return levels
	}

	/// `{"EQStat":"On"}`: the fallback, which costs the preset name.
	static func parseStat(_ body: String) -> Bool? {
		object(body)?.string("EQStat").map { $0.caseInsensitiveCompare("on") == .orderedSame }
	}

	/// A JSON array of names, bare or inside an object. Served as `text/html`,
	/// so nothing consults the content type. Empty counts as a miss.
	static func parsePresets(_ body: String) -> [String]? {
		guard let data = body.data(using: .utf8),
			let root = try? JSONSerialization.jsonObject(with: data)
		else { return nil }
		let array =
			root as? [Any]
			?? (root as? [String: Any])?.values.lazy.compactMap { $0 as? [Any] }.first
		let names = (array ?? []).compactMap { $0 as? String }
			.filter { !$0.trimmingCharacters(in: .whitespaces).isEmpty }
		return names.isEmpty ? nil : names
	}

	/// All ten fader positions in one command. Always the whole curve, so the
	/// outcome never depends on state last read rather than what is on screen.
	static func setBand(_ levels: [Int]) -> String? {
		guard levels.count == bands.count else { return nil }
		let entries: [[String: Any]] = levels.enumerated().map { index, value in
			[
				"index": index, "param_name": bands[index].name,
				"value": min(max(value, levelMin), levelMax),
			]
		}
		guard
			let data = try? JSONSerialization.data(
				withJSONObject: ["EQBand": entries], options: [.sortedKeys]),
			let json = String(data: data, encoding: .utf8)
		else { return nil }
		return "EQSetBand:\(json)"
	}

	/// **Both documented success shapes**, plain `OK` and `{"status":"OK"}`,
	/// because a real device sends the one the PDF does not describe; believing
	/// only the PDF reported every obeyed command as refused on Android.
	/// `Failed` in either shape stays a failure.
	static func isOk(_ body: String) -> Bool {
		let text = body.trimmingCharacters(in: .whitespacesAndNewlines)
		if text.caseInsensitiveCompare("OK") == .orderedSame { return true }
		return object(text)?.string("status")?.caseInsensitiveCompare("OK") == .orderedSame
	}

	private static func object(_ body: String) -> [String: Any]? {
		castJSON(body.trimmingCharacters(in: .whitespacesAndNewlines))
	}
}

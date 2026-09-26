//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// A `gaindrive://` track link, and the https link it is made from.
///
/// The link a person shares is the web client's, `<server>/?track=<id>[&t=<s>]`,
/// so one made here lands in the same chooser page, web player and app intake
/// as one copied from a browser. The server's chooser page (`web/link.html`)
/// turns it into `gaindrive://<host>[:port]<path>?track=<id>[&t=<s>]` for the
/// app, keeping the server's public spelling of itself, proxy subpath
/// included. A port of Android's `TrackLink.kt` and `TrackShare.kt`.
///
/// **A custom scheme, not a universal link**, and not by preference: servers
/// are self-hosted, so their domains cannot be listed in the app's Associated
/// Domains at build time.
struct TrackLink: Equatable, Sendable {
	/// `host[:port]`, exactly as the link spelled it.
	let authority: String
	/// The path before the query, where the server is mounted.
	let path: String
	let trackId: String
	/// Seconds in; 0 when the link named none.
	let position: Double

	/// Nil when this is not one: the wrong scheme, no authority, or no usable
	/// `track`. Nil means "not for us", so the URL is ignored rather than
	/// answered with an error.
	init?(_ url: URL) {
		guard url.scheme?.lowercased() == "gaindrive",
			let parts = URLComponents(url: url, resolvingAgainstBaseURL: false),
			let host = parts.host, !host.isEmpty
		else { return nil }
		authority = parts.port.map { "\(host):\($0)" } ?? host
		path = parts.path
		// `queryItems` percent-decodes, which hands back the server's own
		// spelling of the id.
		let items = parts.queryItems ?? []
		guard let id = items.last(where: { $0.name == "track" })?.value, !id.isEmpty else {
			return nil
		}
		trackId = id
		let seconds = items.last { $0.name == "t" }?.value.flatMap(Double.init) ?? 0
		position = seconds > 0 ? seconds : 0
	}

	/// Whether the server configured at `configURL` is the one this link
	/// names: the authority compared case-insensitively, and the link's path
	/// at or under the configured one, so two instances under different
	/// subpaths of one host stay distinct. The scheme is not part of it - the
	/// link travelled under the app's own and lost the original.
	func matches(configURL: String) -> Bool {
		guard let parts = URLComponents(string: configURL), let host = parts.host else {
			return false
		}
		let configAuthority = parts.port.map { "\(host):\($0)" } ?? host
		guard configAuthority.caseInsensitiveCompare(authority) == .orderedSame else {
			return false
		}
		let configPath = Self.trimmingSlashes(parts.path)
		let linkPath = Self.trimmingSlashes(path)
		return linkPath == configPath || linkPath.hasPrefix(configPath + "/")
	}

	private static func trimmingSlashes(_ path: String) -> String {
		var trimmed = path
		while trimmed.hasSuffix("/") { trimmed.removeLast() }
		return trimmed
	}

	/// The link to share for `trackId` on the server at `serverURL` (stored
	/// form, no trailing slash), starting `seconds` in. Zero or less is the
	/// plain link - `t=0` is rightly no parameter at all.
	static func shareURL(serverURL: String, trackId: String, seconds: Double = 0) -> URL? {
		var allowed = CharacterSet.urlQueryAllowed
		allowed.remove(charactersIn: "&+=")
		guard let id = trackId.addingPercentEncoding(withAllowedCharacters: allowed) else {
			return nil
		}
		var text = "\(serverURL)/?track=\(id)"
		if seconds > 0 { text += "&t=\(formatT(seconds))" }
		return URL(string: text)
	}

	/// `t` as the web emits it: whole seconds as an integer ("90", never
	/// "90.0"), a fraction verbatim, so a chapter's start keeps its millisecond
	/// precision through a round trip.
	static func formatT(_ seconds: Double) -> String {
		seconds == seconds.rounded(.down) ? String(Int(seconds)) : String(seconds)
	}

	/// The chapter under `position`: the last marker at or before it. Nil for
	/// none, and nil for one starting at 0, which would be the plain link.
	static func chapter(in chapters: [Chapter], at position: Double) -> Chapter? {
		guard let found = chapters.last(where: { $0.start <= position }), found.start > 0 else {
			return nil
		}
		return found
	}
}

/// Where a track link leads, or why it leads nowhere.
enum TrackLinkResult: Sendable {
	case album(ref: ItemRef, title: String, song: ItemRef, at: Double)
	case failure(String)
}

/// Finds the server a link names and the album its track is on.
@MainActor
struct TrackLinkResolver {
	let registry: ServerRegistry

	func resolve(_ link: TrackLink) async -> TrackLinkResult {
		// Every server, enabled or not, so a disabled one is reported as what
		// it is rather than as a stranger.
		guard let config = registry.servers.first(where: { link.matches(configURL: $0.urlString) })
		else {
			return .failure("This link is for \(link.authority), which is not one of your servers.")
		}
		guard config.isEnabled, let client = registry.client(for: config.id) else {
			return .failure("This link is for \(config.displayName), which is disabled in Settings.")
		}
		do {
			guard let song = try await client.song(id: link.trackId), let id = song.id else {
				return .failure("The server did not return that track.")
			}
			// The ID3 album wins over the folder parent; on a gaindrive server
			// they are the same folder row.
			guard let album = song.albumId ?? song.parent else {
				return .failure("That track belongs to no album the server names.")
			}
			return .album(
				ref: ItemRef(server: config.id, id: album), title: song.album ?? "",
				song: ItemRef(server: config.id, id: id), at: link.position)
		} catch {
			// A song id is a rowid, reassigned when the server's database is
			// rebuilt, so "not found" is a link that has outlived one - worth
			// saying, since from outside it reads as the feature being broken.
			return .failure(error.userMessage)
		}
	}
}

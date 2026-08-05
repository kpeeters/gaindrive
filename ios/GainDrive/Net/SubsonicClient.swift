//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The one `URLSession` the whole app shares, so the connection pool and the
/// URL cache are shared across every configured server and, later, with the
/// image loader. A session per server would multiply both for no benefit.
enum HTTP {
	static let shared: URLSession = {
		let config = URLSessionConfiguration.default
		config.urlCache = URLCache(memoryCapacity: 8 << 20, diskCapacity: 64 << 20)
		// A self-hosted server that is switched off should fail quickly and
		// say so. The default 60 s makes the connection test look hung, and
		// `waitsForConnectivity` would make it wait indefinitely for a LAN
		// that is reachable but has nothing listening.
		config.timeoutIntervalForRequest = 20
		config.waitsForConnectivity = false
		return URLSession(configuration: config)
	}()
}

/// Talks to exactly one configured server.
///
/// There is no ambient "current server": the caller names one, and the ids in
/// the response are only meaningful relative to it. Each client holds its own
/// `AuthParameters`, so credentials cannot cross between servers.
struct SubsonicClient: Sendable {
	let serverId: ServerId
	let baseURL: URL
	private let auth: AuthParameters
	private let session: URLSession

	init(
		serverId: ServerId,
		baseURL: URL,
		auth: AuthParameters,
		session: URLSession = HTTP.shared
	) {
		self.serverId = serverId
		self.baseURL = baseURL
		self.auth = auth
		self.session = session
	}

	/// Builds the URL for an endpoint. Every request goes through here,
	/// including the ones that are never fetched by this type — cover art and
	/// stream URLs handed to the image loader and to `AVPlayer` — which is why
	/// it is public rather than an implementation detail of `perform`.
	///
	/// The `suffix` exists for `hls.m3u8`, the one endpoint not spelled
	/// `<name>.view`.
	///
	/// Parameters are sorted by name so the same logical request always
	/// produces the same URL. A URL-keyed image cache depends on that, and a
	/// dictionary's iteration order does not survive a rehash.
	func url(
		_ endpoint: String,
		suffix: String = ".view",
		parameters: [String: String] = [:]
	) -> URL {
		let path = baseURL.appending(path: "rest/\(endpoint)\(suffix)")
		// The invariant that makes this non-optional: `ServerConfig` only
		// yields a `baseURL` that already parsed, and the registry only builds
		// a client from one.
		guard var components = URLComponents(url: path, resolvingAgainstBaseURL: false) else {
			return path
		}
		let items = parameters.sorted { $0.key < $1.key }
			.map { URLQueryItem(name: $0.key, value: $0.value) } + auth.queryItems
		components.percentEncodedQueryItems = items.map {
			URLQueryItem(
				name: Self.encode($0.name),
				value: $0.value.map(Self.encode)
			)
		}
		return components.url ?? path
	}

	/// `+` is decoded as a space by a form decoder, and `&`, `=` and `?` would
	/// split or end the value. `urlQueryAllowed` permits all of them because
	/// they are legal in a query *string*; they are not safe inside a single
	/// value, and `URLComponents` does not encode them for you. A username
	/// like `me+music@example.com` reaches the server as `me music@…`
	/// otherwise — the same class of silent corruption as the untrimmed
	/// username `AuthParameters` guards against.
	private static let queryValueAllowed: CharacterSet = {
		var set = CharacterSet.urlQueryAllowed
		set.remove(charactersIn: "+&=?;")
		return set
	}()

	private static func encode(_ value: String) -> String {
		value.addingPercentEncoding(withAllowedCharacters: queryValueAllowed) ?? value
	}

	/// Fetches an endpoint and unwraps its envelope.
	func perform<Body: Decodable & Sendable>(
		_ endpoint: String,
		parameters: [String: String] = [:],
		expecting: Body.Type
	) async throws -> Body {
		let (data, response) = try await session.data(from: url(endpoint, parameters: parameters))
		let decoded = try Self.decode(data, expecting: Body.self, httpStatus: (response as? HTTPURLResponse)?.statusCode)
		return decoded
	}

	/// Split out from `perform` so the envelope handling can be tested without
	/// a network round trip or a stubbed protocol. What is worth testing here
	/// is the parsing, and that is a pure function of the bytes.
	static func decode<Body: Decodable & Sendable>(
		_ data: Data,
		expecting: Body.Type,
		httpStatus: Int?
	) throws -> Body {
		do {
			return try JSONDecoder().decode(SubsonicResponse<Body>.self, from: data).unwrap()
		} catch is DecodingError {
			// A server that answers a non-2xx with no envelope has still told
			// us something; reporting the status beats reporting a parse
			// failure it caused.
			if let httpStatus, !(200..<300).contains(httpStatus) {
				throw SubsonicError.httpStatus(httpStatus)
			}
			throw SubsonicError.malformedResponse
		}
	}

	// MARK: - Phase 1 endpoints

	/// Connectivity and, on most servers, authentication.
	func ping() async throws {
		_ = try await perform("ping", expecting: EmptyBody.self)
	}

	/// Roles and `maxBitRate` for one account, defaulting to our own.
	func user(named username: String? = nil) async throws -> SubsonicUser {
		let name = username ?? auth.username
		return try await perform("getUser", parameters: ["username": name], expecting: UserBody.self).user
	}
}

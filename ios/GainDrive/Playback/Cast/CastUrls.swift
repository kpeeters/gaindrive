//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import OSLog

/// Marks a URL as one that will be read at playback speed, so the server
/// delivers it at roughly 1x instead of as fast as the socket will take it.
///
/// **The server cannot work this out for itself, which is why it has to be
/// said.** It paces a browser by its `Mozilla/` User-Agent and its *own* cast by
/// knowing it started it; a receiver on our route is neither, because this app
/// holds the control channel itself. From the server's side that is
/// indistinguishable from a third-party Subsonic client, which wants the
/// opposite treatment.
///
/// Not the same question `withCastToken(_:_:)` answers, and the two are applied
/// together without either subsuming the other: a grant says *who may fetch*,
/// `pace` says *how fast to send*.
///
/// **What happens without it is not a slow stream but a dead one.** A receiver
/// reads at 1x and stops reading once its buffer is full; unpaced, the server
/// writes the whole track into the socket within seconds and then blocks in one
/// `sink.write`, and from that moment the connection carries no bytes at all.
/// Things on the path are counting - the receiver's own ~60 s no-data timeout,
/// and a reverse proxy's `ProxyTimeout`, which defaults to 60 s - so the music
/// stops about ninety seconds in, half a minute after the cause. That failure
/// was measured against a WiiM; see `serve_direct()` in `src/streamer.cc`.
///
/// **Only ever on a URL a receiver fetches**, never on a playback, download or
/// pin URL: nothing plays off those in real time, and a paced pin takes as long
/// to fetch as the music takes to hear. That is why it is applied here, at the
/// cast route, rather than inside `StreamUrls`, whose builders are shared with
/// the player and the download queue.
func paced(_ url: URL) -> URL {
	guard var parts = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return url }
	parts.queryItems = (parts.queryItems ?? []) + [URLQueryItem(name: "pace", value: "true")]
	return parts.url ?? url
}

/// Swaps this account's credentials out of a URL for a grant that opens one
/// track and nothing else.
///
/// **What it replaces is the password.** `u`/`t`/`s` are the account's
/// credentials - `t` is md5(password + salt) and `s` is the salt - so a
/// television handed them reads the whole library as this person for as long as
/// the password stands, from its own logs and from anything on the path. The
/// grant is one song, twelve hours, and reaches nothing the account could not
/// already read. `getCaptions`' own comment in `src/gaindrive.cc` said as much
/// about the old arrangement before there was anything to do about it.
///
/// `v`, `c` and `f` stay: the server ignores them on a grant-authed request, and
/// `c` is what names this client in its log. Any existing `castToken` is dropped
/// first, so applying this twice cannot leave two for the server to pick between.
///
/// **A nil token returns the URL untouched**, which is what keeps the call sites
/// free of branches - a server too old to mint one is not an error, it is the
/// behaviour this app had before the endpoint existed.
///
/// Applied at the cast route rather than in `StreamUrls`, for the reason
/// `paced(_:)` gives: those builders are shared with playback, downloads and
/// pins, where the ordinary credentials are exactly right.
func withCastToken(_ url: URL, _ token: String?) -> URL {
	guard let token, !token.isEmpty else { return url }
	guard var parts = URLComponents(url: url, resolvingAgainstBaseURL: false) else { return url }
	let kept = (parts.queryItems ?? []).filter {
		!["u", "t", "s", "castToken"].contains($0.name)
	}
	parts.queryItems = kept + [URLQueryItem(name: "castToken", value: token)]
	return parts.url ?? url
}

/// Whether a Cast receiver will decode a file of this type as it stands.
///
/// The set is what the Default Media Receiver documents, intersected with the
/// types `src/codecs.hh` can report. **An allowlist rather than a one-entry
/// denylist**, and deliberately: a codec the server learns to report later would
/// otherwise be sent to a receiver that cannot play it, and demoting to a
/// transcode is the safe direction for a wrong guess. What it costs when wrong
/// is a needless transcode; what a denylist costs is a LOAD that fails, is
/// retried once, fails again, and stalls with nothing on screen to say why.
///
/// **ALAC is the gap it cannot close.** `songs.codec` is a file extension, so
/// the server reports `audio/mp4` for AAC and ALAC alike and nothing here can
/// tell them apart. An ALAC `.m4a` cast at original quality will still fail.
///
/// A nil type is refused for the same reason as an unknown one: not knowing is
/// not the same as knowing it is fine.
func castPlaysNatively(_ contentType: String?) -> Bool {
	guard let contentType else { return false }
	return castNativeTypes.contains(contentType)
}

private let castNativeTypes: Set<String> = [
	"audio/flac",
	"audio/mpeg",
	"audio/mp4",
	"audio/aac",
	"audio/ogg",
	"audio/wav",
]

/// Builds what a receiver is handed: the stream URL, its type, and the artwork.
///
/// **The receiver fetches these itself**, which is the one thing that makes them
/// different from every other URL this app builds. Three consequences, and each
/// has been a bug somewhere:
///
/// * **No stored file.** `LocalEngine` substitutes a `file:` URL when a copy is
///   on the device; a receiver cannot fetch one. Android could, through its
///   bridge, and that branch is gone with the bridge.
/// * **No rewritten scheme.** Local playback goes through
///   `CachingResourceLoader` under `gaindrive-cache://`, which exists precisely
///   because AVFoundation will not consult a delegate for a scheme it knows. A
///   receiver has no delegate and no idea what that scheme is.
/// * **Paced**, per `paced(_:)` above.
@MainActor
struct CastUrls {
	let targets: StreamTargets
	let registry: ServerRegistry

	// `nonisolated` as `CastDiscovery` declares its own: a static in a
	// `@MainActor` type is main-actor isolated otherwise, which is a constraint
	// a logger has no reason to carry.
	nonisolated private static let log = Logger(
		subsystem: "org.gaindrive.ios", category: "cast")

	/// The audio a receiver should fetch.
	///
	/// Returns the same `StreamTarget` shape the local engine deals in, so the
	/// track-info view and the prewarmer need no second vocabulary - the URL is
	/// simply one a receiver can use.
	func audio(for song: Song) async -> StreamTarget? {
		// **No `playable` here either, and by the same omission.** This reaches
		// the resolver `LocalEngine` reaches, one call apart, and the only
		// thing keeping them apart is that this one passes no declaration. With
		// one, a receiver asked for AAC 160 would be sent the MP3 the server
		// holds while the `LOAD` below announced `audio/mp4`, and refuse the
		// media outright - the audio shape of the QuickTime failure on
		// `video(for:)`. `PlayableAudioTests.castRouteDeclaresNothing` guards
		// the seam; there is nothing to assert at this line itself.
		guard var target = await targets.target(for: song.ref) else { return nil }
		// **The original is only sent when the receiver can decode it.** Asking
		// for `raw` is a setting about *this device's* ears, and a receiver that
		// meets a container it cannot type fails the LOAD, is retried once by
		// `LoadRetryWatcher`, fails again, and stalls silently. Falling back to
		// the default transcode costs quality on a route that is crossing the
		// network twice anyway.
		if target.quality.format == .original, !castPlaysNatively(song.contentType) {
			guard let transcoded = await targets.target(for: song.ref, wanted: .default) else {
				return nil
			}
			target = transcoded
		}
		return StreamTarget(
			url: paced(target.url), quality: target.quality, cacheKey: target.cacheKey,
			// For the original the server states the type and we do not know it
			// in advance; the song's own is the best answer, and a receiver
			// sniffs when told nothing.
			contentType: target.contentType ?? song.contentType)
	}

	/// A film, on the one tier a receiver can take: `nativeSeek` means the
	/// server serves it off disk or remuxes with `-c copy`, and either way it
	/// arrives as a real MP4 with a `Content-Length` that answers byte ranges.
	/// Anything else can only be re-encoded, which `CastEngine` refuses rather
	/// than sending.
	func video(for song: Song) -> StreamTarget? {
		// No `playable`, and that omission is the load-bearing one on
		// this route. A receiver demuxes none of them, and the `contentType`
		// below is the entry's `transcodedContentType` - `video/mp4` for
		// exactly the files a declaration would change. Adding the argument
		// here announces MP4 and sends QuickTime, which a receiver refuses
		// outright: the film never starts and nothing on the phone says why.
		// See `StreamUrls.video`.
		guard let target = targets.video(for: song) else { return nil }
		return StreamTarget(
			url: paced(target.url), quality: target.quality, cacheKey: target.cacheKey,
			contentType: target.contentType ?? song.contentType)
	}

	/// A film's **soundtrack**, for a receiver with no screen.
	///
	/// **The mechanism is one query parameter**, and `audio(for:)` already
	/// produces it: naming an audio `format` for a video *is* the server's
	/// request for its soundtrack. So
	/// this is a name rather than an implementation - the call site should read
	/// as the decision it is making, and "ask for the audio of a video" is not
	/// obviously that.
	func soundtrack(for song: Song) async -> StreamTarget? {
		await audio(for: song)
	}

	/// The sleeve the television shows. An ordinary cover URL - the receiver
	/// fetches it from the same server as the audio, so if it can reach one it
	/// can reach the other.
	///
	/// Which is also why it needs the same credential, and why the grant covers
	/// cover art rather than stopping at the stream: this URL travels to the
	/// receiver in the `LOAD`'s metadata and the receiver goes and gets it, so a
	/// token that covered only the audio would have left the password on the
	/// television regardless. `CastEngine.load` applies it to both.
	func artwork(for song: Song) -> URL? {
		registry.clientsSnapshot().coverUrls
			.source(song.coverArt, size: CoverSize.hero)?.url
	}

	/// A grant for one track, or nil to carry on with the ordinary credentials.
	///
	/// **Nil is a normal answer rather than a failure.** A server older than the
	/// endpoint answers a failed envelope, and what that leaves - a URL still
	/// carrying `u`/`t`/`s` - is exactly what this app did before the endpoint
	/// existed. There is nothing to tell the person holding the phone and
	/// nothing to abandon a cast over. It is logged, because "why is the password
	/// still going to the television" should be answerable from the log rather
	/// than by reading this.
	///
	/// **One per track, and not per seek.** This app seeks the receiver rather
	/// than re-issuing the LOAD, so the receiver goes on fetching the URL it
	/// already holds for as long as the track is open - which is why the server's
	/// grant lasts hours, and why it evicts its oldest rather than clearing its
	/// table when full.
	func castToken(for song: Song) async -> String? {
		guard let client = registry.clientsSnapshot().client(for: song.ref.server) else {
			return nil
		}
		do {
			return try await client.castToken(id: song.ref.id)
		} catch {
			Self.log.info(
				"no cast token (\(error.localizedDescription, privacy: .public)); sending the account's own credentials")
			return nil
		}
	}

}

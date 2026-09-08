//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// Marks a URL as one that will be read at playback speed, so the server
/// delivers it at roughly 1x instead of as fast as the socket will take it.
///
/// **The server cannot work this out for itself, which is why it has to be
/// said.** It paces a browser by its `Mozilla/` User-Agent and a server-driven
/// cast by its `castToken`; a receiver on our route has neither, because this
/// app holds the control channel itself and hands over a URL built with the
/// ordinary credentials. From the server's side that is indistinguishable from a
/// third-party Subsonic client, which wants the opposite treatment.
///
/// **What happens without it is not a slow stream but a dead one.** A receiver
/// reads at 1x and stops reading once its buffer is full; unpaced, the server
/// writes the whole track into the socket within seconds and then blocks in one
/// `sink.write`, and from that moment the connection carries no bytes at all.
/// Things on the path are counting — the receiver's own ~60 s no-data timeout,
/// and a reverse proxy's `ProxyTimeout`, which defaults to 60 s — so the music
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
///   bridge, and that branch is gone with the bridge — see `ios/CAST.md`.
/// * **No rewritten scheme.** Local playback goes through
///   `CachingResourceLoader` under `gaindrive-cache://`, which exists precisely
///   because AVFoundation will not consult a delegate for a scheme it knows. A
///   receiver has no delegate and no idea what that scheme is.
/// * **Paced**, per `paced(_:)` above.
@MainActor
struct CastUrls {
	let targets: StreamTargets
	let registry: ServerRegistry

	/// The audio a receiver should fetch.
	///
	/// Returns the same `StreamTarget` shape the local engine deals in, so the
	/// track-info view and the prewarmer need no second vocabulary — the URL is
	/// simply one a receiver can use.
	func audio(for song: Song) async -> StreamTarget? {
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
		guard let target = targets.video(for: song) else { return nil }
		return StreamTarget(
			url: paced(target.url), quality: target.quality, cacheKey: target.cacheKey,
			contentType: target.contentType ?? song.contentType)
	}

	/// The sleeve the television shows. An ordinary cover URL — the receiver
	/// fetches it from the same server as the audio, so if it can reach one it
	/// can reach the other.
	func artwork(for song: Song) -> URL? {
		registry.clientsSnapshot().coverUrls
			.source(song.coverArt, size: CoverSize.hero)?.url
	}

}

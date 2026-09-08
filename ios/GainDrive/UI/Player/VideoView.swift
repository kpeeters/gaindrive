//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import AVKit
import SwiftUI

/// The picture.
///
/// **A destination of its own, not part of Now Playing.** That sheet is
/// square-artwork-shaped and modal, which is the wrong host for a landscape
/// picture — and a surface that moved between two hosts would either be
/// re-parented, which restarts the stream on some devices, or exist twice.
///
/// **`AVPlayerViewController` rather than a custom surface**, which is the
/// opposite of what Android chose and for a symmetrical reason: it rejected
/// media3's `PlayerView` because the chrome was not Material 3, and here the
/// system chrome *is* what an iOS user expects. It also brings
/// Picture-in-Picture, the AirPlay route button, the system scrubber and
/// accessibility, none of which is worth rebuilding.
///
/// Leaving does not stop the film — a concert is listened to as often as it is
/// watched — and the mini player leads back in.
struct VideoView: View {
	let song: Song

	@Environment(PlayerConnection.self) private var player
	@Environment(ServerRegistry.self) private var registry
	@Environment(\.dismiss) private var dismiss

	@State private var tracks: [CaptionTrack] = []
	@State private var chosen: CaptionTrack?
	@State private var cues: [Cue] = []

	var body: some View {
		VideoSurface(player: player.videoPlayer, caption: caption)
			.ignoresSafeArea()
			.overlay(alignment: .topLeading) { close }
			.overlay(alignment: .topTrailing) { picker }
			// Nothing is touching the screen while a film plays, so the system
			// has no other reason to believe anybody is there.
			.onAppear { UIApplication.shared.isIdleTimerDisabled = true }
			.onDisappear { UIApplication.shared.isIdleTimerDisabled = false }
			.task(id: song.ref) {
				chosen = nil
				cues = []
				tracks = await CaptionTracks(registry: registry).list(for: song.ref)
			}
			// **Nothing is fetched until a track is chosen.** Listing costs one
			// request; a track costs another, and only then — the same bargain
			// the web client strikes by leaving a `<track>` disabled.
			.task(id: chosen) {
				guard let chosen else {
					cues = []
					return
				}
				cues = await CaptionTracks(registry: registry).cues(for: song.ref, track: chosen)
			}
	}

	/// Read from the same clock the seek bar is drawn from, so there is no
	/// second timer to keep in step. `position` is published twice a second,
	/// which is finer than any subtitle needs.
	private var caption: String? {
		WebVTT.showing(at: player.position, in: cues)
	}

	private var close: some View {
		Button {
			// The film keeps playing; only the picture goes.
			player.showingVideo = false
			dismiss()
		} label: {
			Image(systemName: "chevron.down")
				.font(.title3)
				.padding(12)
				.background(.thinMaterial, in: Circle())
		}
		.padding()
		.accessibilityLabel("Hide the picture")
	}

	/// Drawn only when there is something to choose. A film with no captions
	/// gets no control — there is no invented track to offer, unlike
	/// ExoPlayer's HLS extractor, which conjures one for a playlist declaring
	/// none.
	@ViewBuilder
	private var picker: some View {
		if !tracks.isEmpty {
			Menu {
				Button {
					chosen = nil
				} label: {
					Label("Off", systemImage: chosen == nil ? "checkmark" : "")
				}
				ForEach(tracks) { track in
					Button {
						chosen = track
					} label: {
						Label(track.name, systemImage: chosen == track ? "checkmark" : "")
					}
				}
			} label: {
				Image(systemName: chosen == nil ? "captions.bubble" : "captions.bubble.fill")
					.font(.title3)
					.padding(12)
					.background(.thinMaterial, in: Circle())
			}
			.padding()
			.accessibilityLabel("Subtitles")
		}
	}
}

/// `AVPlayerViewController`, with the cues drawn into its `contentOverlayView`.
///
/// That view exists for content-related overlays, which is what a subtitle is.
/// What it costs, stated plainly: the cues do not move when the transport
/// controls appear, and they do not follow the picture into Picture-in-Picture
/// or onto an AirPlay screen. Native HLS subtitles would do both — but they
/// would reach only the re-encode tier, so this is the right trade rather than
/// a good one. See `WebVTT`.
private struct VideoSurface: UIViewControllerRepresentable {
	let player: AVPlayer
	let caption: String?

	func makeUIViewController(context: Context) -> AVPlayerViewController {
		let controller = AVPlayerViewController()
		controller.player = player
		controller.allowsPictureInPicturePlayback = true
		controller.videoGravity = .resizeAspect

		let label = context.coordinator.label
		label.numberOfLines = 0
		label.textAlignment = .center
		label.textColor = .white
		label.font = .preferredFont(forTextStyle: .body)
		label.adjustsFontForContentSizeCategory = true
		// A shadow rather than a plate: a plate over a light shot is heavier
		// than the text needs, and cues sit over a moving image where a
		// constant outline reads better than a constant box.
		label.layer.shadowColor = UIColor.black.cgColor
		label.layer.shadowRadius = 3
		label.layer.shadowOpacity = 1
		label.layer.shadowOffset = .zero
		label.translatesAutoresizingMaskIntoConstraints = false

		if let overlay = controller.contentOverlayView {
			overlay.addSubview(label)
			NSLayoutConstraint.activate([
				label.leadingAnchor.constraint(
					greaterThanOrEqualTo: overlay.leadingAnchor, constant: 16),
				label.trailingAnchor.constraint(
					lessThanOrEqualTo: overlay.trailingAnchor, constant: -16),
				label.centerXAnchor.constraint(equalTo: overlay.centerXAnchor),
				// Above the transport controls' usual resting place, which is
				// the best that can be done without being told where they are.
				label.bottomAnchor.constraint(equalTo: overlay.bottomAnchor, constant: -64),
			])
		}
		return controller
	}

	func updateUIViewController(_ controller: AVPlayerViewController, context: Context) {
		if controller.player !== player { controller.player = player }
		context.coordinator.label.text = caption
		context.coordinator.label.isHidden = caption == nil
	}

	func makeCoordinator() -> Coordinator { Coordinator() }

	/// Holds the label so `updateUIViewController` can find it again without
	/// searching the view hierarchy for it.
	final class Coordinator {
		let label = UILabel()
	}
}

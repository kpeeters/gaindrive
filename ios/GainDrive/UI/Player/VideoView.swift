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
/// picture - and a surface that moved between two hosts would either be
/// re-parented, which restarts the stream on some devices, or exist twice.
///
/// **`AVPlayerViewController` rather than a custom surface**, which is the
/// opposite of what Android chose and for a symmetrical reason: it rejected
/// media3's `PlayerView` because the chrome was not Material 3, and here the
/// system chrome *is* what an iOS user expects. It also brings
/// Picture-in-Picture, the AirPlay route button, the system scrubber and
/// accessibility, none of which is worth rebuilding.
///
/// Leaving does not stop the film - a concert is listened to as often as it is
/// watched - and the mini player leads back in.
struct VideoView: View {
	let song: Song

	@Environment(PlayerConnection.self) private var player
	@Environment(ServerRegistry.self) private var registry
	@Environment(\.dismiss) private var dismiss

	@State private var tracks: [CaptionTrack] = []
	@State private var chosen: CaptionTrack?
	@State private var cues: [Cue] = []
	@State private var chapters = ChapterList()
	@State private var chaptersOpen = false

	var body: some View {
		VideoSurface(player: player.videoPlayer, caption: caption)
			.ignoresSafeArea()
			.overlay(alignment: .topLeading) { close }
			.overlay(alignment: .topTrailing) { controls }
			.overlay(alignment: .trailing) { chapterPanel }
			// Nothing is touching the screen while a film plays, so the system
			// has no other reason to believe anybody is there.
			.onAppear { UIApplication.shared.isIdleTimerDisabled = true }
			.onDisappear { UIApplication.shared.isIdleTimerDisabled = false }
			.task(id: song.ref) {
				chosen = nil
				cues = []
				tracks = await CaptionTracks(registry: registry).list(for: song.ref)
			}
			// **`getChapters`, not the album index.** A jump list has to be
			// right about a sidecar somebody edited a moment ago, and it is the
			// only endpoint that can see a video's own container chapters at
			// all - which the scan does not index, so they appear here and not
			// in the album listing.
			//
			// Cleared first, so one recording's markers are never on screen
			// beside another's title while the request is in flight. A queue
			// advance re-runs the task and cancels it, which is the whole of the
			// staleness story.
			.task(id: song.ref) {
				chapters = ChapterList()
				chapters = await ChapterTracks(registry: registry).chapters(for: song.ref)
				// A film with no markers must not be left showing an empty
				// panel somebody opened over the previous one.
				if chapters.isEmpty { chaptersOpen = false }
			}
			// **Nothing is fetched until a track is chosen.** Listing costs one
			// request; a track costs another, and only then - the same bargain
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
		.accessibilityHint("The film keeps playing; the bar at the bottom brings it back")
	}

	/// The markers being played, or nil before the first one.
	private var currentChapter: Int? {
		chapters.chapters.currentIndex(at: player.position)
	}

	private var controls: some View {
		HStack(spacing: 0) {
			chaptersButton
			picker
		}
	}

	/// Drawn only when the recording has markers, like the caption picker
	/// beside it: a control that opens an empty list is a control that lies.
	@ViewBuilder
	private var chaptersButton: some View {
		if !chapters.isEmpty {
			Button {
				chaptersOpen.toggle()
			} label: {
				Image(systemName: chaptersOpen ? "list.bullet.circle.fill" : "list.bullet.circle")
					.font(.title3)
					.padding(12)
					.background(.thinMaterial, in: Circle())
			}
			.padding()
			.accessibilityLabel("Chapters")
		}
	}

	/// The markers inside the recording being played, over the picture.
	///
	/// Over it rather than in a sheet, and opaque rather than tinted, for the
	/// two reasons the web client's panel is: jumping between the songs of a
	/// concert is something you do *while watching it*, so the picture has to
	/// stay visible; and text over a moving image is hard to read at any tint,
	/// so this is a panel of the application that happens to sit over a video
	/// rather than an overlay painted onto one.
	///
	/// It does **not** hide with the transport. Those controls get out of the
	/// way after a few seconds because they are in front of the film; this is a
	/// list being read and scrolled, and having it vanish mid-scroll would make
	/// it unusable. Its own close button and its toggle are what dismiss it.
	@ViewBuilder
	private var chapterPanel: some View {
		if chaptersOpen, !chapters.isEmpty {
			ChapterPanel(
				list: chapters,
				currentIndex: currentChapter,
				onSeek: { player.seek(to: $0) },
				onClose: { chaptersOpen = false })
			.frame(maxWidth: 360)
			.transition(.move(edge: .trailing))
		}
	}

	/// Drawn only when there is something to choose. A film with no captions
	/// gets no control - there is no invented track to offer, unlike
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
/// or onto an AirPlay screen. Native HLS subtitles would do both - but they
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
	///
	/// **`@MainActor` because `UILabel()` is.** A stored default value is
	/// initialised in the enclosing type's isolation, and a plain class has
	/// none - so the property would be constructing a main-actor type from
	/// nowhere. Both methods that touch it, `makeCoordinator` and
	/// `updateUIViewController`, are main-actor already, so nothing else moves.
	@MainActor
	final class Coordinator {
		let label = UILabel()
	}
}

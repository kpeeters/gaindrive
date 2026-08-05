//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The one view that draws a `CoverSource`, mirroring `ui/components/CoverArt.kt`.
///
/// The placeholder sits **behind** the image rather than instead of it. That is
/// the Android lesson and it is easy to lose: `getCoverArt` answers 404 for an
/// artist with no portrait, so a placeholder chosen on a nil *URL* leaves a
/// blank rectangle for every artist that has one but whose fetch failed.
/// Layering makes a failed load fall back for free.
struct CoverArt: View {
	let source: CoverSource?
	var symbol: String = "music.note"
	var cornerRadius: CGFloat = 4

	@State private var image: UIImage?

	var body: some View {
		ZStack {
			Rectangle()
				.fill(.quaternary)
				.overlay {
					Image(systemName: symbol)
						.font(.system(size: 20))
						.foregroundStyle(.secondary)
				}
			if let image {
				Image(uiImage: image)
					.resizable()
					.scaledToFill()
					.transition(.opacity)
			}
		}
		.clipShape(RoundedRectangle(cornerRadius: cornerRadius, style: .continuous))
		// Keyed on the cache key: a reused cell whose source changed must drop
		// the previous image, or a scrolling list shows the wrong artwork for a
		// frame — and with `List` reuse, sometimes for longer.
		.task(id: source?.cacheKey) {
			image = nil
			guard let source else { return }
			image = await ImageStore.shared.image(for: source)
		}
		.animation(.easeIn(duration: 0.15), value: image != nil)
	}
}

struct CoverThumb: View {
	let source: CoverSource?
	var size: CGFloat = 48

	var body: some View {
		CoverArt(source: source)
			.frame(width: size, height: size)
	}
}

/// Round, and with a person rather than a note behind it — an artist with no
/// portrait should not look like an album with no cover.
struct ArtistAvatar: View {
	let source: CoverSource?
	var size: CGFloat = 96

	var body: some View {
		CoverArt(source: source, symbol: "person.fill", cornerRadius: size / 2)
			.frame(width: size, height: size)
	}
}

struct CoverHero: View {
	let source: CoverSource?

	var body: some View {
		CoverArt(source: source, cornerRadius: 10)
			.aspectRatio(1, contentMode: .fit)
	}
}

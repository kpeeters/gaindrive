//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

struct ExternalLink: Identifiable, Hashable, Sendable {
	let label: String
	let url: URL

	var id: URL { url }

	init?(_ label: String, _ raw: String?) {
		guard let raw, let url = URL(string: raw) else { return nil }
		self.label = label
		self.url = url
	}
}

/// Prose plus its external links, mirroring `ui/components/Notes.kt`.
///
/// Shared between the artist biography and the album notes so the two cannot
/// drift - they are the same shape and came from the same upstream.
struct NotesSection: View {
	let text: String?
	let links: [ExternalLink]
	var collapsedLines = 4

	@State private var expanded = false
	/// The text's height in full and at `collapsedLines`, measured, so the
	/// toggle can animate a clip between them; see `body`.
	@State private var fullHeight: CGFloat?
	@State private var collapsedHeight: CGFloat?

	var body: some View {
		if text != nil || !links.isEmpty {
			VStack(alignment: .leading, spacing: 8) {
				if let text, !text.isEmpty {
					// **Only the height animates.** The text is always laid out
					// in full and a frame clips it to the collapsed height:
					// toggling `lineLimit` inside `withAnimation` re-wrapped the
					// text, slid its ellipsis about and animated every other
					// size in the row, the cover's included.
					Text(text)
						.font(.callout)
						.fixedSize(horizontal: false, vertical: true)
						.onGeometryChange(for: CGFloat.self) { $0.size.height } action: {
							fullHeight = $0
						}
						.background(alignment: .top) {
							Text(text)
								.font(.callout)
								.lineLimit(collapsedLines)
								.fixedSize(horizontal: false, vertical: true)
								.hidden()
								.onGeometryChange(for: CGFloat.self) { $0.size.height } action: {
									collapsedHeight = $0
								}
						}
						.frame(height: expanded ? fullHeight : collapsedHeight, alignment: .top)
						.clipped()
						.animation(.easeInOut(duration: 0.2), value: expanded)
					Button(expanded ? "Show less" : "Show more") {
						expanded.toggle()
					}
					.font(.footnote)
				}
				if !links.isEmpty {
					// Scrolls rather than wraps: a wrapped row of links changes
					// the height of the header as the text expands, which moves
					// the track list under the user's finger.
					ScrollView(.horizontal, showsIndicators: false) {
						HStack(spacing: 8) {
							ForEach(links) { link in
								Link(link.label, destination: link.url)
									.buttonStyle(.bordered)
									.controlSize(.small)
							}
						}
					}
					.scrollClipDisabled()
				}
			}
		}
	}
}

extension ArtistInfo {
	var externalLinks: [ExternalLink] {
		[
			ExternalLink("Wikipedia", wikiUrl),
			ExternalLink("AllMusic", allMusicUrl),
			ExternalLink("Last.fm", lastFmUrl),
			ExternalLink("Discogs", discogsUrl),
		].compactMap { $0 }
	}
}

extension AlbumNotes {
	var externalLinks: [ExternalLink] {
		[
			ExternalLink("Wikipedia", wikiUrl),
			ExternalLink("AllMusic", allMusicUrl),
		].compactMap { $0 }
	}
}

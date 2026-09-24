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

	var body: some View {
		if text != nil || !links.isEmpty {
			VStack(alignment: .leading, spacing: 8) {
				if let text, !text.isEmpty {
					Text(text)
						.font(.callout)
						.lineLimit(expanded ? nil : collapsedLines)
					Button(expanded ? "Show less" : "Show more") {
						withAnimation { expanded.toggle() }
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

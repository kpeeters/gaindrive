//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Which server a row came from.
///
/// Deliberately quiet - a tinted label rather than a chip. It appears on every
/// row in merged scope, so anything louder would compete with the content it is
/// annotating.
///
/// Callers pass `nil` in single-server scope and nothing renders, which is what
/// keeps the app looking like a single-server client until it isn't one.
struct ServerBadge: View {
	let name: String?

	var body: some View {
		if let name, !name.isEmpty {
			Text(name)
				.font(.caption2)
				.lineLimit(1)
				.padding(.horizontal, 6)
				.padding(.vertical, 2)
				.background(.quaternary, in: RoundedRectangle(cornerRadius: 4, style: .continuous))
				.foregroundStyle(.secondary)
				// A bare server name read out in the middle of a row says
				// nothing about what it is; the row is announced as one
				// utterance, so this has to carry its own preposition.
				.accessibilityLabel("on \(name)")
		}
	}
}

/// Plural, because a row whose duplicates were collapsed stands for every
/// server that has the album - and hiding that would make the missing second
/// row look like a bug.
struct ServerBadges: View {
	let names: [String]

	var body: some View {
		if !names.isEmpty {
			HStack(spacing: 4) {
				ForEach(names, id: \.self) { ServerBadge(name: $0) }
			}
		}
	}
}

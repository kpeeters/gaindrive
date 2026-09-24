//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Renders the three arms of `Load`, mirroring `ui/components/LoadStateBox.kt`.
///
/// Android's version takes a `Modifier` and its recorded bug was that the
/// modifier reached only two of the three arms, so every list scrolled up
/// underneath the app bar once content arrived. That cannot recur here: the
/// caller wraps this view rather than passing styling into it.
///
/// There is no `RefreshableLoadBox` counterpart. On iOS `.refreshable` has to
/// sit on the scroll view itself, so a wrapper that is not the `List` could not
/// host the indicator - and each screen writing `.refreshable` directly gets
/// the distinction that type existed to encode (content stays on screen during
/// a refresh, and only a scope change or a retry blanks it) by construction.
struct LoadStateBox<Value, Content: View>: View {
	let state: Load<Value>
	var onRetry: (() -> Void)? = nil
	@ViewBuilder let content: (Value) -> Content

	var body: some View {
		switch state {
		case .loading:
			ProgressView()
				.frame(maxWidth: .infinity, maxHeight: .infinity)
		case .failed(let message):
			ContentUnavailableView {
				Label("Could not load", systemImage: "exclamationmark.triangle")
			} description: {
				Text(message)
			} actions: {
				if let onRetry {
					Button("Try again", action: onRetry)
						.buttonStyle(.borderedProminent)
				}
			}
		case .ready(let value):
			content(value)
		}
	}
}

/// Kept as a named type rather than inlined so the wording stays consistent
/// across screens, and so phase 5 can make it say something different when the
/// app is offline in exactly one place.
struct EmptyMessage: View {
	let text: String
	var symbol: String = "music.note.list"

	var body: some View {
		ContentUnavailableView(text, systemImage: symbol)
	}
}

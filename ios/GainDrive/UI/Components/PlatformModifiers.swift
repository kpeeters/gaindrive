//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

//	The SwiftUI modifiers that exist on iOS only, behind names both builds
//	accept. This file is where the two platforms' differences in the screens
//	are kept, so a screen says what it wants once and does not branch.

extension ToolbarItemPlacement {
	/// The bar's leading edge: `.topBarLeading` on iOS, the navigation area
	/// of the window toolbar on the Mac.
	static var leadingBar: ToolbarItemPlacement {
		#if os(macOS)
			.navigation
		#else
			.topBarLeading
		#endif
	}

	/// The bar's trailing edge: `.topBarTrailing` on iOS, the primary actions
	/// of the window toolbar on the Mac.
	static var trailingBar: ToolbarItemPlacement {
		#if os(macOS)
			.primaryAction
		#else
			.topBarTrailing
		#endif
	}
}

extension View {
	/// An inline title on iOS; the Mac has no large titles to opt out of.
	@ViewBuilder
	func inlineTitle() -> some View {
		#if os(macOS)
			self
		#else
			navigationBarTitleDisplayMode(.inline)
		#endif
	}

	/// A field that takes a URL: the URL keyboard, no autocapitalisation, and
	/// the content type that offers what was typed before. The Mac's keyboard
	/// needs none of it.
	@ViewBuilder
	func urlEntry() -> some View {
		#if os(macOS)
			self
		#else
			keyboardType(.URL)
				.textContentType(.URL)
				.textInputAutocapitalization(.never)
		#endif
	}

	/// No automatic capitals, for names that are typed exactly - usernames,
	/// addresses. The Mac does not capitalise to begin with.
	@ViewBuilder
	func noAutocapitalization() -> some View {
		#if os(macOS)
			self
		#else
			textInputAutocapitalization(.never)
		#endif
	}

	/// Capitalised words, for a display name.
	@ViewBuilder
	func wordsAutocapitalization() -> some View {
		#if os(macOS)
			self
		#else
			textInputAutocapitalization(.words)
		#endif
	}
}

extension Color {
	/// The window's own background, for a pinned header that must be opaque
	/// over the rows scrolling beneath it.
	static var platformBackground: Color {
		#if os(macOS)
			Color(nsColor: .windowBackgroundColor)
		#else
			Color(.systemBackground)
		#endif
	}
}

/// The list edit button, iOS only: a Mac list reorders and deletes without an
/// edit mode.
struct PlatformEditButton: View {
	var body: some View {
		#if os(iOS)
			EditButton()
		#else
			EmptyView()
		#endif
	}
}

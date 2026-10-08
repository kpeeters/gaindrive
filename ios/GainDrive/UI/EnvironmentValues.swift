//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

extension EnvironmentValues {
	/// The repository, carried as a **keyed value** rather than through
	/// `.environment(object)`.
	///
	/// That overload requires `Observable`, and `LibraryRepository`
	/// deliberately is not: it is stateless and `Sendable` precisely so the
	/// fan-out can run off the main actor, and `@Observable` would give it
	/// observation storage and take both properties away. Nothing observes it
	/// anyway - it answers questions, it does not publish changes; the view
	/// models are what the views observe.
	///
	/// Optional only because an `EnvironmentValues` default has to exist
	/// without a registry to build one from. `GainDriveApp` always supplies it,
	/// so a `nil` here means a preview that did not.
	@Entry var library: LibraryRepository? = nil

	/// Opens an album from outside any pane, under its artist in the
	/// Library, as the web's player cover does. Set by both roots; the player
	/// bar's cover uses it, passing the playing track to scroll to.
	@Entry var revealAlbum: @MainActor (ItemRef, ItemRef?) -> Void = { _, _ in }
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The merged library listing - or, with `uploads` set, the account's own
/// uploads - mirroring `ui/browse/ArtistsViewModel.kt`.
///
/// **State is a plain held value**, not something derived from a subscription.
/// Android reversed away from `stateIn(WhileSubscribed)` here because leaving
/// for an album dropped the last subscriber, the share stopped, and returning
/// restarted the upstream - a full re-read of the library every time the screen
/// came back into view. SwiftUI has the identical trap wearing a different hat:
/// `.task` runs again on every re-appearance. `loadedFor` is what closes it.
///
/// **The initialiser stores references and does nothing else.** `RootView.init`
/// re-runs whenever `GainDriveApp.body` re-evaluates - which
/// `preferredColorScheme` guarantees on every theme change - and `@State` keeps
/// the first instance and discards the rest. Any work started in an initialiser
/// would happen once per discarded copy.
@MainActor
@Observable
final class ArtistsViewModel {
	private(set) var state: Load<LibraryListing> = .loading
	private(set) var failures: [ServerFailure] = []
	private(set) var isRefreshing = false
	private(set) var badgeNames: [ServerId: String] = [:]

	/// Whether the upload icon is worth drawing. Never true on the uploads
	/// instance - the icon is how you get there. A failed check leaves the
	/// last answer standing: the icon is navigation, and taking it away
	/// because one request timed out would strand the user out of their own
	/// uploads.
	private(set) var canUpload = false

	/// Whether this instance is the uploads listing rather than the library.
	/// The listing it loads is the whole difference - the pushed uploads
	/// screen and the Library tab share every other line of this class.
	let uploads: Bool

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	/// What is currently on screen. Leaving for an album and coming back is not
	/// a reason to re-read the library: the list has not changed while the user
	/// was two screens deep.
	@ObservationIgnored private var loadedFor: BrowseScope?
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, selection: ServerSelection, uploads: Bool = false) {
		self.library = library
		self.selection = selection
		self.uploads = uploads
	}

	/// Called from `.task(id:)`, which fires on appearance *and* on a scope
	/// change. The guard is what distinguishes them.
	func appear() {
		let scope = selection.scope
		guard loadedFor != scope else { return }
		start(clearFirst: loadedFor != nil)
	}

	func retry() {
		start(clearFirst: true)
	}

	/// `.refreshable` awaits this, or the pull indicator snaps back before the
	/// request has even left. A refresh keeps the list on screen - blanking it
	/// would hide the very thing the user pulled to update.
	func refresh() async {
		isRefreshing = true
		start(clearFirst: false)
		await task?.value
		isRefreshing = false
	}

	private func start(clearFirst: Bool) {
		let scope = selection.scope
		loadedFor = scope
		if clearFirst { state = .loading }
		badgeNames = selection.badgeNames

		// Fire-and-forget beside the load, never awaited before it: the icon
		// arriving a beat after the list costs nothing, while serialising the
		// two would put an account lookup in front of every listing.
		if !uploads {
			Task { [library] in
				self.canUpload = await library.canUpload(scope: scope)
			}
		}

		task?.cancel()
		// The counterpart of `viewModelScope.launch`: the *view model* owns the
		// task, so navigating away does not cancel the load and strand the
		// screen on `.loading` forever. The view's `.task` closure only kicks
		// this off and returns.
		task = Task { [library, uploads] in
			let merged =
				uploads
				// The personal listing has no categories by construction, so
				// it is an artists-only listing of the same shape.
				? await library.uploadIndexes(scope: scope)
					.map { LibraryListing(categories: [], artists: $0) }
				: await library.libraryListing(scope: scope)
			guard !Task.isCancelled else { return }
			self.state = merged.load
			self.failures = merged.failures
		}
	}

	func dismissFailures() {
		failures = []
	}

	func artistUi(_ artist: Artist) -> ArtistUi {
		ArtistUi(artist: artist, badges: artist.sources.compactMap { badgeNames[$0] })
	}
}

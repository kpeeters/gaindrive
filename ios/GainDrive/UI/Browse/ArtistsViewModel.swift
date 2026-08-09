//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// The artist list, mirroring `ui/browse/ArtistsViewModel.kt`.
///
/// **State is a plain held value**, not something derived from a subscription.
/// Android reversed away from `stateIn(WhileSubscribed)` here because leaving
/// for an album dropped the last subscriber, the share stopped, and returning
/// restarted the upstream — a full re-read of the library every time the screen
/// came back into view. SwiftUI has the identical trap wearing a different hat:
/// `.task` runs again on every re-appearance. `loadedFor` is what closes it.
///
/// **The initialiser stores references and does nothing else.** `RootView.init`
/// re-runs whenever `GainDriveApp.body` re-evaluates — which
/// `preferredColorScheme` guarantees on every theme change — and `@State` keeps
/// the first instance and discards the rest. Any work started in an initialiser
/// would happen once per discarded copy.
@MainActor
@Observable
final class ArtistsViewModel {
	private(set) var state: Load<[ArtistIndex]> = .loading
	private(set) var failures: [ServerFailure] = []
	private(set) var isRefreshing = false
	private(set) var badgeNames: [ServerId: String] = [:]

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let selection: ServerSelection
	/// What is currently on screen. Leaving for an album and coming back is not
	/// a reason to re-read the library: the list has not changed while the user
	/// was two screens deep.
	@ObservationIgnored private var loadedFor: BrowseScope?
	@ObservationIgnored private var task: Task<Void, Never>?

	init(library: LibraryRepository, selection: ServerSelection) {
		self.library = library
		self.selection = selection
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
	/// request has even left. A refresh keeps the list on screen — blanking it
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

		task?.cancel()
		// The counterpart of `viewModelScope.launch`: the *view model* owns the
		// task, so navigating away does not cancel the load and strand the
		// screen on `.loading` forever. The view's `.task` closure only kicks
		// this off and returns.
		task = Task { [library] in
			let merged = await library.artistIndexes(scope: scope)
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

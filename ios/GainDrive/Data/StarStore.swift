//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What the user has starred, over what the last fetch said.
///
/// **A deviation from Android, and the server's semantics are the reason.**
/// gaindrive stores stars by *path* rather than by row id - deliberately, since
/// folder and song rowids are not stable across a rescan - and an id that does
/// not resolve to a path is **silently ignored**. There is no error 70 and no
/// other signal, so `star` returning `ok` is not evidence that anything
/// changed. Android's model, trusting `starredAt` from the last fetch, cannot
/// be right against that.
///
/// So the client keeps its own record, shows it immediately, and confirms it
/// against `getStarred2` - the only thing that can actually answer the
/// question.
@MainActor
@Observable
final class StarStore {
	/// Only what the user has touched this session. Everything else falls back
	/// to the `starredAt` its model arrived with, so this stays small and no
	/// screen has to wait for it to be populated.
	private var overrides: [ItemRef: Bool] = [:]
	/// Toggles in flight, so a double tap cannot issue two writes whose replies
	/// arrive out of order and leave the star showing the earlier one.
	private var inFlight: Set<ItemRef> = []

	@ObservationIgnored private let library: LibraryRepository

	init(library: LibraryRepository) {
		self.library = library
	}

	func isStarred(_ ref: ItemRef, fallback: Bool) -> Bool {
		overrides[ref] ?? fallback
	}

	func isBusy(_ ref: ItemRef) -> Bool {
		inFlight.contains(ref)
	}

	/// Flips the star, shows it at once, and then asks the server what actually
	/// happened.
	///
	/// The reconciliation is not belt and braces: it is the only way to find out
	/// whether the write took, given that the endpoint cannot say. A star the
	/// server did not record silently reverts a moment later, which is honest -
	/// the alternative is a star that lies for the rest of the session.
	func toggle(_ ref: ItemRef, kind: StarKind, currently starred: Bool) async {
		guard !inFlight.contains(ref) else { return }
		let wanted = !isStarred(ref, fallback: starred)
		overrides[ref] = wanted
		inFlight.insert(ref)
		defer { inFlight.remove(ref) }

		do {
			try await library.setStarred(ref, kind: kind, starred: wanted)
		} catch {
			// A refusal we *can* see, unlike the silent kind: put it back.
			overrides[ref] = !wanted
			return
		}
		await reconcile(server: ref.server)
	}

	/// Folds a `getStarred2` answer into the overrides for that server.
	func reconcile(server: ServerId) async {
		guard let starred = try? await library.starred(server: server) else { return }
		let truth = Set(
			starred.artists.map(\.ref) + starred.albums.map(\.ref) + starred.songs.map(\.ref))
		overrides = Self.reconciled(overrides, server: server, truth: truth)
	}

	/// Every override on `server` is set to what the server says; other
	/// servers' are untouched.
	///
	/// **An override the server agrees with is kept, not dropped.** Dropping it
	/// fell back to the `starredAt` the screen was loaded with, which is the
	/// value from *before* the toggle - so a star that took flipped back until
	/// that screen reloaded. Android's `StarStore` keeps it for the same reason.
	/// Pure and static so the rule is tested without a server.
	static func reconciled(
		_ overrides: [ItemRef: Bool], server: ServerId, truth: Set<ItemRef>
	) -> [ItemRef: Bool] {
		var result = overrides
		for ref in overrides.keys where ref.server == server {
			result[ref] = truth.contains(ref)
		}
		return result
	}

	/// Removing a server must not leave its stars behind to be applied to
	/// whatever id happens to match next.
	func forget(server: ServerId) {
		overrides = overrides.filter { $0.key.server != server }
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What can be done to an album in your uploads: move it into the shared
/// library (an admin only) or delete it (its owner). Android's album screen
/// overflow and its two dialogs.
@MainActor
@Observable
final class UploadActionsModel {
	private(set) var canMove = false
	private(set) var roots: [MusicRoot] = []
	var root: MusicRoot? {
		didSet { rootChanged() }
	}
	var folder = ""
	private(set) var suggestions: [String] = []
	private(set) var working = false
	/// Moved or deleted: the screen has nothing left to show.
	private(set) var done = false
	var error: String?

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let album: ItemRef
	@ObservationIgnored var artistName: String?

	init(library: LibraryRepository, album: ItemRef) {
		self.library = library
		self.album = album
	}

	/// Only asked on an album reached through uploads, so an ordinary one
	/// costs no request. The roots only once the answer is yes.
	func appear() async {
		canMove = await library.isAdmin(on: album.server)
		guard canMove else { return }
		roots = (try? await library.destinationRoots(on: album.server)) ?? []
		// Pre-selected, so the common single-root case is just a confirmation.
		root = roots.first
	}

	/// An artists root defaults to what the album is filed under now. A
	/// categories root deliberately does not: the level there is a category,
	/// and the batch's artist is whatever the source called it - for a fetched
	/// video, the channel, which is never the answer.
	private func rootChanged() {
		folder = root?.contentType == "categories" ? "" : (artistName ?? "")
		suggestions = []
		guard let root else { return }
		Task {
			suggestions = await library.folders(on: album.server, in: root.id).sorted {
				$0.localizedCaseInsensitiveCompare($1) == .orderedAscending
			}
		}
	}

	var isCategories: Bool { root?.contentType == "categories" }

	/// **Both halves required**, so the button says so rather than letting a
	/// refusal arrive afterwards: each was briefly optional, with a default
	/// that filed things wrongly.
	var canConfirmMove: Bool {
		!working && root != nil && !folder.trimmingCharacters(in: .whitespaces).isEmpty
	}

	var matches: [String] {
		let typed = folder.trimmingCharacters(in: .whitespaces)
		let list =
			typed.isEmpty
			? suggestions
			: suggestions.filter { $0.localizedCaseInsensitiveContains(typed) && $0 != typed }
		return Array(list.prefix(8))
	}

	func move() async {
		guard canConfirmMove, let root else { return }
		working = true
		do {
			try await library.moveUpload(album, to: root.id, folder: folder)
			done = true
		} catch {
			// The server's own words: "already in that folder" and "outside the
			// library" need different fixes.
			self.error = "Could not move it: \(error.userMessage)"
		}
		working = false
	}

	func delete() async {
		working = true
		do {
			try await library.deleteUpload(album)
			done = true
		} catch {
			self.error = "Could not delete it: \(error.userMessage)"
		}
		working = false
	}
}

/// The toolbar menu, its confirmation and the move sheet, for an album in
/// uploads. Leaves the screen when the album is gone.
struct UploadActionsMenu: View {
	let album: ItemRef
	let albumTitle: String
	let artistName: String?

	@Environment(\.library) private var library
	@Environment(\.dismiss) private var dismiss
	@State private var model: UploadActionsModel?
	@State private var confirmingDelete = false
	@State private var moving = false

	var body: some View {
		Menu {
			if model?.canMove == true {
				Button {
					moving = true
				} label: {
					Label("Move to the library…", systemImage: "tray.and.arrow.down")
				}
			}
			Button(role: .destructive) {
				confirmingDelete = true
			} label: {
				Label("Delete from uploads…", systemImage: "trash")
			}
		} label: {
			Label("More", systemImage: "ellipsis.circle")
		}
		.disabled(model?.working == true)
		.task {
			if model == nil, let library {
				let made = UploadActionsModel(library: library, album: album)
				made.artistName = artistName
				model = made
				await made.appear()
			}
		}
		.onChange(of: artistName) { model?.artistName = artistName }
		.onChange(of: model?.done) { if model?.done == true { dismiss() } }
		// Says where the files go, because that is the difference between this
		// and everything else on the screen: no trash behind it, and nothing in
		// the app can put them back.
		.alert("Delete from uploads?", isPresented: $confirmingDelete) {
			Button("Delete", role: .destructive) {
				Task { await model?.delete() }
			}
			Button("Cancel", role: .cancel) {}
		} message: {
			Text("“\(albumTitle)” and its files are removed from the server. This cannot be undone.")
		}
		.alert(
			"Nothing changed",
			isPresented: Binding(
				get: { model?.error != nil }, set: { if !$0 { model?.error = nil } })
		) {
			Button("OK") { model?.error = nil }
		} message: {
			Text(model?.error ?? "")
		}
		.sheet(isPresented: $moving) {
			if let model {
				MoveUploadView(model: model, albumTitle: albumTitle)
			}
		}
	}
}

/// Where to file an upload in the shared library.
///
/// A root, then a name at the level under it - an artist or a category -
/// typed freely with suggestions rather than picked from a browser: typing a
/// name that is not there is how a new one is made, and the server creates it.
/// The album being moved is itself the level below that, so there is nothing
/// deeper to choose.
struct MoveUploadView: View {
	@Bindable var model: UploadActionsModel
	let albumTitle: String

	@Environment(\.dismiss) private var dismiss

	var body: some View {
		NavigationStack {
			Form {
				Section {
					Text(
						"""
						“\(albumTitle)” leaves your uploads and joins the shared \
						library, where everyone with an account can see it. The \
						files move on the server; this cannot be undone from here.
						""")
					.font(.footnote)
					.foregroundStyle(.secondary)
				}
				if model.roots.count > 1 {
					Picker("Library", selection: $model.root) {
						ForEach(model.roots, id: \.id) { root in
							// The kind alongside the name: which one holds films
							// is not guessable from a name somebody chose.
							Text(root.contentType.map { "\(root.name) (\($0))" } ?? root.name)
								.tag(Optional(root))
						}
					}
				}
				Section {
					TextField(model.isCategories ? "Category" : "Artist", text: $model.folder)
						.autocorrectionDisabled()
					ForEach(model.matches, id: \.self) { name in
						Button(name) { model.folder = name }
					}
				} footer: {
					Text(
						model.isCategories
							? "A category that does not exist yet is created."
							: "An artist that does not exist yet is created.")
				}
			}
			.navigationTitle("Move to the library")
			.inlineTitle()
			.toolbar {
				ToolbarItem(placement: .cancellationAction) {
					Button("Cancel") { dismiss() }
				}
				ToolbarItem(placement: .confirmationAction) {
					Button("Move") {
						Task {
							await model.move()
							if model.done { dismiss() }
						}
					}
					.disabled(!model.canConfirmMove)
				}
			}
		}
	}
}

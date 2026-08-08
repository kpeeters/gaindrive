//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Add one track to a playlist, or to a new one.
///
/// **Only the track's own server's playlists are offered**, and that is not a
/// simplification: a playlist lives on one server and can only hold that
/// server's songs, so listing another server's playlists would offer the user a
/// choice that cannot work. In merged scope that means this sheet shows a
/// subset of what the Playlists tab does, which is correct and worth the
/// heading saying so when there is more than one server.
struct AddToPlaylistView: View {
	let song: Song

	@Environment(\.library) private var library
	@Environment(ServerRegistry.self) private var registry
	@Environment(\.dismiss) private var dismiss

	@State private var state: Load<[Playlist]> = .loading
	@State private var newName = ""
	@State private var isSaving = false
	@State private var error: String?

	var body: some View {
		NavigationStack {
			Form {
				Section("New playlist") {
					HStack {
						TextField("Name", text: $newName)
						Button("Create") {
							Task { await create() }
						}
						.disabled(newName.trimmingCharacters(in: .whitespaces).isEmpty || isSaving)
					}
				}

				Section {
					LoadStateBox(state: state) { playlists in
						if playlists.isEmpty {
							Text("No playlists on this server yet.")
								.foregroundStyle(.secondary)
						} else {
							ForEach(playlists) { playlist in
								Button {
									Task { await add(to: playlist) }
								} label: {
									PlaylistRow(playlist: playlist)
								}
								.buttonStyle(.plain)
								.disabled(isSaving)
							}
						}
					}
				} header: {
					Text(serverName.map { "Playlists on \($0)" } ?? "Playlists")
				}
			}
			.navigationTitle("Add to Playlist")
			.navigationBarTitleDisplayMode(.inline)
			.toolbar {
				ToolbarItem(placement: .cancellationAction) {
					Button("Cancel") { dismiss() }
				}
			}
			.task { await load() }
			.alert(
				"Could not add the track",
				isPresented: Binding(get: { error != nil }, set: { if !$0 { error = nil } })
			) {
				Button("OK") { error = nil }
			} message: {
				Text(error ?? "")
			}
		}
	}

	/// Named only when there is more than one server, so a single-server install
	/// is not told which of its one server this is.
	private var serverName: String? {
		guard registry.enabled.count > 1 else { return nil }
		return registry.config(for: song.ref.server)?.displayName
	}

	private func load() async {
		guard let library else { return }
		do {
			state = .ready(try await library.playlistsOf(server: song.ref.server))
		} catch {
			guard !error.isCancellation else { return }
			state = .failed(error.userMessage)
		}
	}

	private func add(to playlist: Playlist) async {
		guard let library else { return }
		isSaving = true
		defer { isSaving = false }
		do {
			try await library.addToPlaylist(playlist.ref, song: song.ref)
			dismiss()
		} catch {
			self.error = error.userMessage
		}
	}

	private func create() async {
		guard let library else { return }
		isSaving = true
		defer { isSaving = false }
		do {
			try await library.createPlaylist(
				server: song.ref.server,
				name: newName.trimmingCharacters(in: .whitespacesAndNewlines),
				songs: [song.ref])
			dismiss()
		} catch {
			self.error = error.userMessage
		}
	}
}

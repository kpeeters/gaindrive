//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What is stored on the device, and how much of it there may be.
///
/// **There is deliberately no browse-every-stored-file screen.** The complaint
/// automatic eviction always earns is that it deletes the thing you were about
/// to want, and pinning answers it from the other end: instead of policing what
/// gets removed, you name what may never be removed. So this shows how much is
/// used, lists the pins you placed, and has one button that empties the lot —
/// `android/CACHING.md` reaches the same three.
struct StorageSettingsView: View {
	@Environment(SettingsStore.self) private var settings
	@Environment(PinRepository.self) private var pins

	@State private var removing: Pin?
	@State private var removingEverything = false

	var body: some View {
		@Bindable var settings = settings
		Form {
			Section {
				LabeledContent("Used") {
					Text(PinRepository.readable(pins.usageBytes))
				}
				Picker("Limit", selection: $settings.cacheCapBytes) {
					ForEach(SettingsStore.cacheCapChoices, id: \.self) { choice in
						Text(PinRepository.readable(choice)).tag(choice)
					}
				}
				// The store enforces the cap, so lowering it has to reach it —
				// otherwise the new limit takes effect only after something
				// else happens to push the limits down.
				.onChange(of: settings.cacheCapBytes) {
					Task { await pins.capChanged() }
				}
			} footer: {
				// The cap is a refusal rather than an evictor, and saying so is
				// what stops it reading as a promise to tidy up on its own.
				Text(
					"""
					Downloads are never removed to make room, so a download \
					that would not fit is refused instead.
					"""
				)
			}

			Section("Downloads") {
				if pins.pins.isEmpty {
					Text("Nothing downloaded")
						.foregroundStyle(.secondary)
				} else {
					ForEach(pins.pins) { pin in
						row(pin)
					}
				}
			}

			Section {
				Button("Remove all downloads", role: .destructive) {
					removingEverything = true
				}
				.disabled(pins.pins.isEmpty)
			}
		}
		.navigationTitle("Storage")
		.navigationBarTitleDisplayMode(.inline)
		// Re-reads what each pin covers, which is what makes a pinned playlist
		// pick up a track added since it was pinned.
		.task { await pins.refresh() }
		// **This screen confirms and the album screen's toggle does not.** It
		// is the managing surface, where a row says only a name and a mis-tap
		// is much less obviously reversible than a control with its state on
		// its face.
		.alert(
			"Remove this download?",
			isPresented: Binding(get: { removing != nil }, set: { if !$0 { removing = nil } }),
			presenting: removing
		) { pin in
			Button("Remove", role: .destructive) {
				Task { await pins.remove(pin) }
			}
			Button("Cancel", role: .cancel) {}
		} message: { pin in
			Text("“\(pin.name)” is deleted from this device.")
		}
		.alert("Remove all downloads?", isPresented: $removingEverything) {
			Button("Remove", role: .destructive) {
				Task { await pins.removeEverything() }
			}
			Button("Cancel", role: .cancel) {}
		} message: {
			Text("Everything downloaded is deleted from this device.")
		}
	}

	private func row(_ pin: Pin) -> some View {
		HStack(spacing: 12) {
			DownloadStateIcon(state: pins.state(of: pin))
			VStack(alignment: .leading, spacing: 2) {
				Text(pin.name).lineLimit(1)
				Text(pin.kind.label)
					.font(.footnote)
					.foregroundStyle(.secondary)
			}
			Spacer(minLength: 0)
		}
		.swipeActions(edge: .trailing) {
			Button(role: .destructive) {
				removing = pin
			} label: {
				Label("Remove", systemImage: "trash")
			}
		}
	}
}

extension PinKind {
	var label: String {
		switch self {
		case .song: "Track"
		case .album: "Album"
		case .playlist: "Playlist"
		}
	}
}

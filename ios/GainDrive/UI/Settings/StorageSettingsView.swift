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
/// used, lists the pins you placed, and offers to empty what was merely kept or
/// everything - `android/CACHING.md` reaches the same shape.
struct StorageSettingsView: View {
	@Environment(SettingsStore.self) private var settings
	@Environment(PinRepository.self) private var pins

	@State private var removing: Pin?
	@State private var removingEverything = false
	/// Measured when the screen appears and after a clear; the cover cache
	/// is not observable, and a figure that is a moment stale is harmless.
	@State private var coverBytes: Int?

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
				// The store enforces the cap, so lowering it has to reach it -
				// otherwise the new limit takes effect only after something
				// else happens to push the limits down.
				.onChange(of: settings.cacheCapBytes) {
					Task { await pins.capChanged() }
				}
			} footer: {
				// Both halves said, so the cap reads neither as a promise to keep
				// everything nor as a threat to the downloads.
				Text(
					"""
					Music kept from playing is removed oldest first to make room. \
					Downloads are never removed, so a download that would not fit \
					is refused instead.
					"""
				)
			}

			Section {
				Toggle("Store music as it plays", isOn: $settings.cacheOnPlay)
				Toggle("Download on Wi-Fi only", isOn: $settings.downloadUnmeteredOnly)
					// A download carries its network policy from when it was
					// created, so the ones in flight have to be restarted.
					.onChange(of: settings.downloadUnmeteredOnly) {
						Task { await pins.downloadPolicyChanged() }
					}
			} footer: {
				Text(
					"""
					Wi-Fi only applies to downloads. Storing what is already \
					playing costs no extra data.
					"""
				)
			}

			Section {
				// No confirmation for either: what goes is re-fetched on the next
				// play or the next scroll, unlike a download.
				Button("Free \(PinRepository.readable(pins.evictableBytes))") {
					Task { await pins.freeEvictable() }
				}
				.disabled(pins.evictableBytes == 0)
				Button(coverLabel) {
					Task {
						await ImageStore.shared.clear()
						coverBytes = await ImageStore.shared.diskUsage()
					}
				}
				.disabled(coverBytes == 0)
			} footer: {
				Text(
					"""
					Free removes music kept from playing. Downloads stay, and so \
					do their covers when cover art is cleared.
					""")
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
		.task {
			coverBytes = await ImageStore.shared.diskUsage()
			await pins.refresh()
		}
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

	private var coverLabel: String {
		guard let coverBytes else { return "Clear cover art" }
		return "Clear cover art (\(PinRepository.readable(Int64(coverBytes))))"
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

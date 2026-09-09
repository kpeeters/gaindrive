//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Where to play: this device, or a Cast receiver on the network.
///
/// **A plain sheet rather than a system picker**, which is the one real loss in
/// not taking the Google Cast SDK — `GCKUICastButton` would have brought the
/// standard chooser with it, and `ios/LICENSE` clause 2 rules the SDK out. See
/// `CAST.md`. Android is in the same position and reached the same shape.
///
/// It is deliberately *not* an AirPlay route picker with Cast devices added:
/// `AVRoutePickerView` is UIKit's own and cannot be extended, and the two
/// mechanisms are unrelated — AirPlay moves this device's audio output, casting
/// hands a URL to something that fetches for itself. They sit beside each other
/// in Now Playing and stay distinct.
struct CastDeviceSheet: View {
	@Environment(PlayerConnection.self) private var player
	@Environment(CastDeviceStore.self) private var store
	@Environment(\.dismiss) private var dismiss

	/// Owned here, so **browsing runs only while somebody is looking**.
	/// Multicast is not free, and this sheet is the only consumer.
	@State private var discovery = CastDiscovery()
	@Environment(\.scenePhase) private var scenePhase

	var body: some View {
		NavigationStack {
			List {
				thisDevice
				discovered
				configured
			}
			.navigationTitle("Play on")
			.navigationBarTitleDisplayMode(.inline)
			.toolbar {
				ToolbarItem(placement: .confirmationAction) {
					Button("Done") { dismiss() }
				}
			}
		}
		.presentationDetents([.medium, .large])
		.onAppear { discovery.start() }
		.onDisappear { discovery.stop() }
		// iOS tears the browser down when the app is suspended and Mac Catalyst
		// never suspends; `onAppear` cannot cover the difference, because it
		// does not fire again for a view that never left the screen.
		.onChange(of: scenePhase) { _, phase in
			guard phase == .active else { return }
			discovery.restart()
		}
	}

	// MARK: - Rows

	/// **First, and always present.** Coming back is the action somebody is
	/// most likely to want from this sheet — a device list with no way off it
	/// makes leaving feel like a setting rather than a tap.
	private var thisDevice: some View {
		Section {
			row(
				name: "This device", detail: nil, systemImage: "iphone",
				selected: player.castDevice == nil
			) {
				player.stopCasting()
				dismiss()
			}
		}
	}

	@ViewBuilder
	private var discovered: some View {
		Section {
			switch discovery.state {
			case .failed(let message):
				Label(message, systemImage: "exclamationmark.triangle")
					.font(.footnote)
					.foregroundStyle(.secondary)
			case .idle, .browsing:
				if discovery.devices.isEmpty {
					looking
				} else {
					ForEach(discovery.devices) { device in
						deviceRow(device)
					}
				}
			}
		} header: {
			Text("On this network")
		}
	}

	/// The configured devices a discovered one does not already stand for.
	/// Their own section, as on Android: an entry somebody typed is a different
	/// kind of thing from one that announced itself, and merging the two lists
	/// would hide which is which.
	@ViewBuilder
	private var configured: some View {
		let manual = store.notDiscovered(among: discovery.devices)
		if !manual.isEmpty {
			Section("Added by hand") {
				ForEach(manual) { deviceRow($0.device) }
			}
		}
	}

	@ViewBuilder
	private var looking: some View {
		HStack(spacing: 8) {
			ProgressView().controlSize(.small)
			Text("Looking for devices…").foregroundStyle(.secondary)
		}
		// **What a refused Local Network permission actually looks like.**
		// There is no API to ask whether it was granted, and denied, the browser
		// reports zero results rather than failing — so a silence that has gone
		// on too long is the only signal there is. It claims nothing: a network
		// with no receivers on it looks exactly the same.
		if discovery.quiet {
			Text(
				"""
				Nothing has answered. If there is a receiver here, check that \
				GainDrive may find devices on your local network in Settings, \
				or add one by its address in Settings → Casting.
				"""
			)
			.font(.footnote)
			.foregroundStyle(.secondary)
		}
	}

	private func deviceRow(_ device: CastDevice) -> some View {
		row(
			name: device.name,
			// The model ahead of the address, as Android's picker does:
			// `WiiM Pro` says far more about which box this is than a number,
			// and the address is only known once something has connected.
			detail: [device.model, device.address].compactMap { $0 }.first,
			systemImage: device.kind == .wiim ? "hifispeaker" : "tv",
			selected: player.castDevice == device
		) {
			player.startCasting(to: device)
			dismiss()
		}
	}

	private func row(
		name: String, detail: String?, systemImage: String, selected: Bool,
		action: @escaping () -> Void
	) -> some View {
		Button(action: action) {
			HStack(spacing: 12) {
				Image(systemName: systemImage)
					.frame(width: 24)
					.foregroundStyle(selected ? Color.accentColor : .secondary)
				VStack(alignment: .leading, spacing: 2) {
					Text(name).foregroundStyle(.primary)
					if let detail {
						Text(detail).font(.footnote).foregroundStyle(.secondary)
					}
				}
				Spacer(minLength: 8)
				if selected {
					Image(systemName: "checkmark").foregroundStyle(Color.accentColor)
				}
			}
			.contentShape(.rect)
		}
		.buttonStyle(.plain)
	}
}

/// The control that opens it.
///
/// **Drawn as filled while casting**, which is the whole of the "casting to X"
/// state on a surface that has no room to say more — the same glyph the system
/// uses for an active AirPlay route, and the same convention every Cast client
/// follows.
struct CastButton: View {
	@Environment(PlayerConnection.self) private var player
	@Binding var showing: Bool

	var body: some View {
		Button {
			showing = true
		} label: {
			Image(systemName: player.isCasting ? "tv.fill" : "tv")
				.foregroundStyle(player.isCasting ? Color.accentColor : .secondary)
		}
		.accessibilityLabel(
			player.castDevice.map { "Casting to \($0.name)" } ?? "Play on another device")
	}
}

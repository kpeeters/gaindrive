//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Cast receivers: what is announcing itself, and what has been named by hand.
///
/// **Managing devices, not choosing one.** Where to play is the player's
/// question and is answered by `CastDeviceSheet` behind the cast button; this
/// screen exists for the devices themselves - seeing what the network offers,
/// naming one that will not announce itself, and asking whether it answers.
///
/// The Test button is what proves the two assumptions everything downstream
/// rests on, and it is why this pane was built a stage before anything used it:
/// that `NWBrowser` finds a receiver without a restricted multicast
/// entitlement, and that Network.framework will complete a TLS handshake with
/// one. Neither is settled by a unit test.
struct CastSettingsView: View {
	@Environment(CastDeviceStore.self) private var store

	/// Owned here rather than injected, because **browsing runs only while
	/// something is looking**. Multicast is not free and the picker is the only
	/// consumer; a discovery object living in the environment would browse for
	/// the life of the app.
	@State private var discovery = CastDiscovery()
	@State private var editing: ManualCastDevice?
	@State private var probing: Set<String> = []
	@State private var results: [String: CastProbeResult] = [:]
	/// iOS tears the browser down when the app is suspended; Mac Catalyst never
	/// suspends. `onAppear` cannot cover the difference, because it does not
	/// fire again for a view that never left the screen.
	@Environment(\.scenePhase) private var scenePhase

	var body: some View {
		Form {
			discoveredSection
			manualSection
		}
		.navigationTitle("Casting")
		.navigationBarTitleDisplayMode(.inline)
		.toolbar {
			ToolbarItem(placement: .topBarTrailing) {
				Button {
					editing = ManualCastDevice(address: "", name: "")
				} label: {
					Label("Add a device", systemImage: "plus")
				}
			}
		}
		.sheet(item: $editing) { device in
			CastDeviceEditor(device: device) { store.save($0) }
		}
		.onAppear { discovery.start() }
		.onDisappear { discovery.stop() }
		.onChange(of: scenePhase) { _, phase in
			guard phase == .active else { return }
			discovery.restart()
		}
	}

	// MARK: - Discovered

	@ViewBuilder
	private var discoveredSection: some View {
		Section {
			switch discovery.state {
			case .failed(let message):
				// **Denial is a state, not a timeout.** A refused Local Network
				// permission and a network with no receivers on it both produce
				// an empty list, and only one of them is worth telling somebody
				// about - so the failure says what to do rather than leaving a
				// spinner running over nothing.
				Label(message, systemImage: "exclamationmark.triangle")
					.foregroundStyle(.secondary)
					.font(.footnote)
			case .idle, .browsing:
				if discovery.devices.isEmpty {
					HStack(spacing: 8) {
						ProgressView().controlSize(.small)
						Text("Looking for devices…").foregroundStyle(.secondary)
					}
					// **What a refused Local Network permission actually looks
					// like.** There is no API to ask whether it was granted, and
					// denied, the browser reports zero results rather than
					// failing - so a silence that has gone on too long is the
					// only signal there is. It claims nothing: a network with no
					// receivers on it looks exactly the same, which is why this
					// says what to check rather than what is wrong.
					if discovery.quiet {
						Text(
							"""
							Nothing has answered. If there is a receiver on this \
							network, check that GainDrive is allowed to find \
							devices on your local network in Settings → Privacy \
							& Security → Local Network.
							"""
						)
						.font(.footnote)
						.foregroundStyle(.secondary)
					}
				} else {
					ForEach(discovery.devices) { device in
						row(device, id: device.id)
					}
				}
			}
		} header: {
			Text("On this network")
		} footer: {
			Text(
				"""
				A receiver has to be on the same network as this device. \
				Some guest and hotel networks block devices from seeing each \
				other, and casting is then unavailable.
				"""
			)
		}
	}

	// MARK: - Added by hand

	@ViewBuilder
	private var manualSection: some View {
		let manual = store.notDiscovered(among: discovery.devices)
		Section {
			if manual.isEmpty {
				Text("None").foregroundStyle(.secondary)
			} else {
				ForEach(manual) { entry in
					row(entry.device, id: entry.device.id)
						.swipeActions {
							Button("Delete", role: .destructive) { store.remove(id: entry.id) }
							Button("Edit") { editing = entry }
						}
				}
			}
		} header: {
			Text("Added by hand")
		} footer: {
			Text(
				"""
				For a receiver that does not announce itself. Give its IP \
				address - a name will not do. A device added here stops \
				appearing twice once you have tested the discovered one.
				"""
			)
		}
	}

	// MARK: - A row, and the test

	private func row(_ device: CastDevice, id: String) -> some View {
		VStack(alignment: .leading, spacing: 4) {
			HStack {
				VStack(alignment: .leading, spacing: 2) {
					Text(device.name)
					if let subtitle = subtitle(device) {
						Text(subtitle)
							.font(.footnote)
							.foregroundStyle(.secondary)
					}
				}
				Spacer(minLength: 12)
				if probing.contains(id) {
					ProgressView().controlSize(.small)
				} else {
					// **Test, and only test.** Choosing where to play is the
					// player's business and lives behind the cast button in Now
					// Playing; this screen is for the devices themselves. The
					// two are different enough acts that one gesture meaning
					// either would sometimes mean the wrong one - which is the
					// same reason `CastProbe` is a separate type from the
					// session in the first place.
					Button("Test") { test(device) }
						.buttonStyle(.bordered)
						.controlSize(.small)
				}
			}
			if let result = results[id] {
				Text(Self.describe(result))
					.font(.footnote)
					.foregroundStyle(result == .unreachable ? .red : .secondary)
			}
		}
	}

	/// The model ahead of the address, as Android's picker does - `WiiM Pro`
	/// says far more about which box this is than a number does, and the address
	/// is only known once something has connected.
	private func subtitle(_ device: CastDevice) -> String? {
		let parts = [device.model, device.address].compactMap { $0 }
		return parts.isEmpty ? nil : parts.joined(separator: " · ")
	}

	private func test(_ device: CastDevice) {
		probing.insert(device.id)
		Task {
			let result = await CastProbe().probe(device)
			probing.remove(device.id)
			results[device.id] = result
			guard case .answered(_, let address) = result, let address else { return }
			// The address the connection reached, written back to the discovered
			// entry. `NWBrowser` never reports one, so this is the only way the
			// list learns which box a name refers to - and it is what lets a
			// device somebody also added by hand stop appearing twice.
			discovery.note(address: address, for: device.id)
		}
	}

	private static func describe(_ result: CastProbeResult) -> String {
		switch result {
		case .answered(let runningApp, let address):
			// The address it actually reached, which for a discovered device is
			// the only place that fact ever appears: `NWBrowser` reports a
			// service, not a host.
			let at = address.map { " at \($0)" } ?? ""
			guard let runningApp else { return "Answered\(at)" }
			return "Answered\(at) - showing \(runningApp)"
		case .silent:
			// Different advice from unreachable, which is the whole reason the
			// two are separate outcomes: the address is live, and something
			// other than a Cast receiver is on it.
			return "Something answered on that address, but it does not speak Cast."
		case .unreachable:
			return "No answer. Check the address, and that the device is on."
		}
	}
}

/// Adding or editing a device by address.
private struct CastDeviceEditor: View {
	@State var device: ManualCastDevice
	let onSave: (ManualCastDevice) -> Void

	@Environment(\.dismiss) private var dismiss

	var body: some View {
		NavigationStack {
			Form {
				Section {
					TextField("Address", text: $device.address)
						.textInputAutocapitalization(.never)
						.autocorrectionDisabled()
					TextField("Name", text: $device.name)
					TextField("Port", value: $device.port, format: .number)
				} footer: {
					Text("An IP address, such as 192.168.1.50. The usual port is 8009.")
				}
			}
			.navigationTitle("Device")
			.navigationBarTitleDisplayMode(.inline)
			.toolbar {
				ToolbarItem(placement: .cancellationAction) {
					Button("Cancel") { dismiss() }
				}
				ToolbarItem(placement: .confirmationAction) {
					Button("Save") {
						onSave(device)
						dismiss()
					}
					// A device with no address is not a device. The name is
					// allowed to be empty and falls back to the address, which
					// is the one label that is certainly true.
					.disabled(device.address.trimmingCharacters(in: .whitespaces).isEmpty)
				}
			}
		}
	}
}

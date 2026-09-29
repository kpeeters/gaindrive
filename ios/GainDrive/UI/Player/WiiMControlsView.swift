//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// A WiiM's equalizer: the switch, the ten graphic bands, the device's presets
/// and the ones saved in this app. Android's `WiiMControlsSheet`, pushed from
/// the cast device sheet rather than opened from its own button, which keeps
/// the Now Playing row as it is and puts it beside the volume it belongs with.
///
/// Horizontal sliders rather than Android's vertical faders: ten rotated
/// sliders side by side are cramped on a phone, and a labelled list is how an
/// iOS settings screen shows ten values.
struct WiiMControlsView: View {
	@State private var model: WiiMControlsModel
	@State private var naming = false
	@State private var newName = ""

	init(deviceId: String, address: String) {
		_model = State(initialValue: WiiMControlsModel(deviceId: deviceId, address: address))
	}

	var body: some View {
		Group {
			switch model.state {
			case .loading:
				ProgressView()
			case .failed(let message):
				ContentUnavailableView {
					Label("Equalizer unavailable", systemImage: "slider.vertical.3")
				} description: {
					Text(message)
				} actions: {
					Button("Try again") { Task { await model.refresh() } }
				}
			case .ready(let ui):
				form(ui)
			}
		}
		.navigationTitle("Equalizer")
		.inlineTitle()
		.task { await model.refresh() }
		.alert(
			"Something went wrong",
			isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })
		) {
			Button("OK") { model.error = nil }
		} message: {
			Text(model.error ?? "")
		}
		.alert("Save preset", isPresented: $naming) {
			TextField("Name", text: $newName)
			Button("Save") { _ = model.save(newName) }
			Button("Cancel", role: .cancel) {}
		} message: {
			Text("Kept in this app, not on the speaker.")
		}
	}

	private func form(_ ui: WiiMEqUi) -> some View {
		Form {
			Section {
				Toggle(
					"Equalizer",
					isOn: Binding(get: { ui.enabled }, set: { model.setEnabled($0) }))
			}
			.disabled(model.busy)

			if let bands = ui.bands {
				Section {
					ForEach(Array(WiiMEq.bands.enumerated()), id: \.offset) { index, band in
						bandRow(band.label, value: bands[index], index: index)
					}
					Button("Save as preset…") {
						newName = ""
						naming = true
					}
				} header: {
					Text("Bands")
				} footer: {
					// Offsets from flat, not decibels: how the device's 0-99
					// maps to dB has not been measured.
					Text("Steps from flat on the device's own scale.")
				}
				// Faders are inert while the equalizer is off, and while a write
				// is in flight, whose answer would move them anyway.
				.disabled(!ui.enabled || model.busy)
			}

			if !ui.saved.isEmpty {
				Section("Saved in this app") {
					ForEach(ui.saved, id: \.self) { name in
						presetRow(name, ticked: ui.preset == name) { model.applySaved(name) }
							.swipeActions {
								Button("Delete", role: .destructive) { model.deleteSaved(name) }
							}
					}
				}
				.disabled(model.busy)
			}

			Section("On the speaker") {
				ForEach(ui.presets, id: \.self) { name in
					presetRow(name, ticked: ui.preset == name) { model.selectPreset(name) }
				}
			}
			.disabled(model.busy)
		}
	}

	private func bandRow(_ label: String, value: Int, index: Int) -> some View {
		HStack(spacing: 12) {
			Text(label)
				.font(.footnote.monospacedDigit())
				.frame(width: 36, alignment: .trailing)
			Slider(
				value: Binding(
					get: { Double(value) },
					set: { model.setBand(index, to: Int($0.rounded())) }),
				in: Double(WiiMEq.levelMin)...Double(WiiMEq.levelMax),
				step: 1,
				// Written on release, as the whole curve: one command per drag
				// rather than one per step.
				onEditingChanged: { editing in if !editing { model.commitBands() } }
			)
			.accessibilityLabel("\(label) hertz")
			Text(Self.offset(value))
				.font(.footnote.monospacedDigit())
				.foregroundStyle(.secondary)
				.frame(width: 32, alignment: .trailing)
		}
	}

	private func presetRow(_ name: String, ticked: Bool, action: @escaping () -> Void) -> some View {
		Button(action: action) {
			HStack {
				Text(name).foregroundStyle(.primary)
				Spacer()
				if ticked {
					Image(systemName: "checkmark").foregroundStyle(Color.accentColor)
				}
			}
		}
	}

	private static func offset(_ value: Int) -> String {
		let delta = value - WiiMEq.levelFlat
		return delta > 0 ? "+\(delta)" : "\(delta)"
	}
}

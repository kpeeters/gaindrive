//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// A WiiM's equalizer: the switch, the ten graphic bands, the device's presets
/// and the ones saved in this app. Android's `WiiMControlsSheet`, shown in
/// the tune panel (`CastTunePanel`) under the receiver's volume.
///
/// The bands are ten vertical faders side by side, as the web and Android
/// draw them: the curve reads as a shape, which ten labelled rows do not.
struct WiiMControlsView<Header: View>: View {
	@State private var model: WiiMControlsModel
	@State private var naming = false
	@State private var newName = ""
	/// The receiver's volume, which the tune panel shows above the equalizer.
	/// Passed in rather than drawn beside this view because on iOS both have
	/// to be sections of the one `Form`.
	private let header: Header

	init(deviceId: String, address: String, @ViewBuilder header: () -> Header) {
		_model = State(initialValue: WiiMControlsModel(deviceId: deviceId, address: address))
		self.header = header()
	}

	var body: some View {
		container {
			switch model.state {
			case .loading:
				ProgressView()
					.frame(maxWidth: .infinity)
			case .failed(let message):
				ContentUnavailableView {
					Label("Equalizer unavailable", systemImage: "slider.vertical.3")
				} description: {
					Text(message)
				} actions: {
					Button("Try again") { Task { await model.refresh() } }
				}
			case .ready(let ui):
				#if os(macOS)
					panel(ui)
				#else
					form(ui)
				#endif
			}
		}
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

	@ViewBuilder
	private func container(@ViewBuilder _ content: () -> some View) -> some View {
		#if os(macOS)
			VStack(alignment: .leading, spacing: 14) {
				header
				Divider()
				content()
			}
		#else
			Form {
				header
				content()
			}
		#endif
	}

	#if os(macOS)
		/// A Mac popover body: **stacks have an ideal height**, so the popover
		/// fits the content, where a `Form` has none. The width is the
		/// presenter's. The presets are a pop-up rather than rows: a WiiM ships
		/// some twenty, and twenty rows make a popover taller than the window.
		private func panel(_ ui: WiiMEqUi) -> some View {
			VStack(alignment: .leading, spacing: 14) {
				HStack {
					Text("Equalizer").font(.headline)
					Spacer()
					Toggle(
						"Equalizer",
						isOn: Binding(get: { ui.enabled }, set: { model.setEnabled($0) })
					)
					.labelsHidden()
					.toggleStyle(.switch)
					.disabled(model.busy)
				}
				if let bands = ui.bands {
					faders(bands)
					// Inert while off, and while a write is in flight, whose
					// answer would move the faders anyway.
					.disabled(!ui.enabled || model.busy)
					Text("Steps from flat on the device's own scale.")
						.font(.footnote)
						.foregroundStyle(.secondary)
				}
				// Icons beside the pop-up, as the web's row has: labelled
				// buttons pushed the row wider than the popover.
				HStack(spacing: 8) {
					presetPicker(ui)
					if ui.bands != nil {
						Button {
							newName = ""
							naming = true
						} label: {
							Image(systemName: "square.and.arrow.down")
						}
						.help("Save as preset")
						.disabled(!ui.enabled || model.busy)
					}
					// No swipe on a Mac, so deleting a saved preset is a menu.
					if !ui.saved.isEmpty {
						Menu {
							ForEach(ui.saved, id: \.self) { name in
								Button(name, role: .destructive) { model.deleteSaved(name) }
							}
						} label: {
							Image(systemName: "trash")
						}
						.menuStyle(.borderlessButton)
						.menuIndicator(.hidden)
						.fixedSize()
						.help("Delete a saved preset")
					}
				}
				.buttonStyle(.borderless)
				.disabled(model.busy)
			}
		}

		/// The speaker's presets and this app's in one pop-up, tagged by where
		/// they live, since the two lists may share a name.
		private func presetPicker(_ ui: WiiMEqUi) -> some View {
			Picker(
				"Preset",
				selection: Binding<String?>(
					get: {
						ui.preset.map { ui.saved.contains($0) ? "saved:" + $0 : "speaker:" + $0 }
					},
					set: { tag in
						guard let tag else { return }
						if tag.hasPrefix("saved:") {
							model.applySaved(String(tag.dropFirst("saved:".count)))
						} else {
							model.selectPreset(String(tag.dropFirst("speaker:".count)))
						}
					})
			) {
				if ui.preset == nil {
					Text("Custom").tag(String?.none)
				}
				Section("On the speaker") {
					ForEach(ui.presets, id: \.self) { Text($0).tag(Optional("speaker:" + $0)) }
				}
				if !ui.saved.isEmpty {
					Section("Saved in this app") {
						ForEach(ui.saved, id: \.self) { Text($0).tag(Optional("saved:" + $0)) }
					}
				}
			}
			.labelsHidden()
			.frame(maxWidth: .infinity)
		}
	#endif

	@ViewBuilder
	private func form(_ ui: WiiMEqUi) -> some View {
		Section {
			Toggle(
				"Equalizer",
				isOn: Binding(get: { ui.enabled }, set: { model.setEnabled($0) }))
		}
		.disabled(model.busy)

		if let bands = ui.bands {
			Section {
				faders(bands)
					.padding(.vertical, 4)
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

	/// One column per band: the offset above, the fader, the frequency below.
	/// Each column takes an equal share of the width, so the ten narrow evenly
	/// rather than pushing the panel wider.
	private func faders(_ bands: [Int]) -> some View {
		HStack(spacing: 2) {
			ForEach(Array(WiiMEq.bands.enumerated()), id: \.offset) { index, band in
				VStack(spacing: 4) {
					Text(Self.offset(bands[index]))
						.foregroundStyle(.secondary)
					VerticalFader(
						value: bands[index],
						range: WiiMEq.levelMin...WiiMEq.levelMax,
						onChange: { model.setBand(index, to: $0) },
						// Written on release, as the whole curve: one command
						// per drag rather than one per step.
						onCommit: { model.commitBands() }
					)
					.frame(height: 112)
					.accessibilityLabel("\(band.label) hertz")
					.accessibilityValue(Self.offset(bands[index]))
					Text(band.label)
						.foregroundStyle(.secondary)
				}
				.font(.caption2.monospacedDigit())
				.lineLimit(1)
				.frame(maxWidth: .infinity)
			}
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

/// A vertical fader drawn as the web's range input is: a thin track filled
/// from the bottom up to a round knob. Drawn rather than a rotated `Slider`,
/// whose layout keeps its unrotated frame and so overlaps its neighbours.
private struct VerticalFader: View {
	let value: Int
	let range: ClosedRange<Int>
	let onChange: (Int) -> Void
	let onCommit: () -> Void
	@Environment(\.isEnabled) private var isEnabled

	private let knob: CGFloat = 14
	private let track: CGFloat = 4

	var body: some View {
		GeometryReader { geo in
			let travel = max(geo.size.height - knob, 1)
			let fraction = CGFloat(value - range.lowerBound)
				/ CGFloat(range.upperBound - range.lowerBound)
			let knobTop = travel * (1 - fraction)
			let tint = isEnabled ? Color.accentColor : Color.secondary
			ZStack(alignment: .top) {
				Capsule()
					.fill(.quaternary)
					.frame(width: track)
				Capsule()
					.fill(tint)
					.frame(width: track, height: geo.size.height - knobTop - knob / 2)
					.offset(y: knobTop + knob / 2)
				Circle()
					.fill(tint)
					.frame(width: knob, height: knob)
					.offset(y: knobTop)
			}
			.frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
			.contentShape(.rect)
			// Distance zero, so a click on the track jumps the knob there,
			// as a range input does.
			.gesture(
				DragGesture(minimumDistance: 0)
					.onChanged { drag in
						guard isEnabled else { return }
						let f = 1 - (drag.location.y - knob / 2) / travel
						set(range.lowerBound
							+ Int((CGFloat(range.count - 1) * min(max(f, 0), 1)).rounded()))
					}
					.onEnded { _ in
						if isEnabled { onCommit() }
					})
		}
		.accessibilityElement()
		.accessibilityAdjustableAction { direction in
			switch direction {
			case .increment: set(value + 1)
			case .decrement: set(value - 1)
			default: return
			}
			onCommit()
		}
	}

	private func set(_ next: Int) {
		let clamped = min(max(next, range.lowerBound), range.upperBound)
		if clamped != value { onChange(clamped) }
	}
}

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
/// not taking the Google Cast SDK - `GCKUICastButton` would have brought the
/// standard chooser with it, and `ios/LICENSE` clause 2 rules the SDK out.
/// Android is in the same position and reached the same shape.
///
/// It is deliberately *not* an AirPlay route picker with Cast devices added:
/// `AVRoutePickerView` is UIKit's own and cannot be extended, and the two
/// mechanisms are unrelated - AirPlay moves this device's audio output, casting
/// hands a URL to something that fetches for itself. They sit beside each other
/// in Now Playing and stay distinct.
struct CastDeviceSheet: View {
	@Environment(PlayerConnection.self) private var player
	@Environment(\.dismiss) private var dismiss

	/// Owned here, so **browsing runs only while somebody is looking**.
	/// Multicast is not free, and this sheet is the only consumer.
	@State private var discovery = CastDiscovery()
	@Environment(\.scenePhase) private var scenePhase

	var body: some View {
		Group {
			#if os(macOS)
				dialog
			#else
				listSheet
			#endif
		}
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

	// MARK: - iOS

	#if !os(macOS)
		private var listSheet: some View {
			NavigationStack {
				List {
					thisDevice
					discovered
				}
				.navigationTitle("Play on")
				.inlineTitle()
				.toolbar {
					ToolbarItem(placement: .confirmationAction) {
						Button("Done") { dismiss() }
					}
				}
			}
			.presentationDetents([.medium, .large])
		}
	#endif

	// MARK: - macOS

	#if os(macOS)
		/// A Mac dialog: **stacks have an ideal height**, so the sheet takes its
		/// height from the rows and grows as receivers answer; a `List` scrolls,
		/// has none, and collapsed the sheet to its title.
		///
		/// **The width is set**, as an alert's is. Left to the content, the
		/// wrapped hint's height depended on a width that depended on it, and
		/// AppKit looped on the window's constraints until it crashed.
		private var dialog: some View {
			VStack(alignment: .leading, spacing: 12) {
				Text("Play on").font(.headline)
				thisDeviceRow
				Text("On this network")
					.font(.subheadline)
					.foregroundStyle(.secondary)
					.padding(.top, 4)
				switch discovery.state {
				case .failed(let message):
					Label(message, systemImage: "exclamationmark.triangle")
						.font(.footnote)
						.foregroundStyle(.secondary)
				case .idle, .browsing:
					if discovery.devices.isEmpty {
						looking
					} else {
						ForEach(discovery.devices) { deviceRow($0) }
					}
				}
				HStack {
					Spacer()
					Button("Done") { dismiss() }
						.keyboardShortcut(.defaultAction)
				}
				.padding(.top, 4)
			}
			.padding(20)
			.frame(width: 380)
		}
	#endif

	// MARK: - Rows

	/// **First, and always present.** Coming back is the action somebody is
	/// most likely to want from this sheet - a device list with no way off it
	/// makes leaving feel like a setting rather than a tap.
	private var thisDevice: some View {
		Section { thisDeviceRow }
	}

	#if os(macOS)
		private static let thisDeviceSymbol = "desktopcomputer"
	#else
		private static let thisDeviceSymbol = "iphone"
	#endif

	private var thisDeviceRow: some View {
		row(
			name: "This device", detail: nil, systemImage: Self.thisDeviceSymbol,
			selected: player.castDevice == nil
		) {
			player.stopCasting()
			dismiss()
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

	@ViewBuilder
	private var looking: some View {
		HStack(spacing: 8) {
			ProgressView().controlSize(.small)
			Text("Looking for devices…").foregroundStyle(.secondary)
		}
		// **What a refused Local Network permission actually looks like.**
		// There is no API to ask whether it was granted, and denied, the browser
		// reports zero results rather than failing - so a silence that has gone
		// on too long is the only signal there is. It claims nothing: a network
		// with no receivers on it looks exactly the same.
		if discovery.quiet {
			Text(
				"""
				Nothing has answered. If there is a receiver here, check that \
				GainDrive may find devices on your local network in Settings.
				"""
			)
			.font(.footnote)
			.foregroundStyle(.secondary)
		}
	}

	private func deviceRow(_ device: CastDevice) -> some View {
		row(
			name: device.name,
			// The model, as Android's picker shows: `WiiM Pro` says which box
			// this is, and a discovered device has no address until connected.
			detail: device.model,
			// **The device, not the action.** A row says what kind of box this
			// is; the cast glyph belongs on the control that starts a session
			// and would say nothing here, where every row is a receiver.
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

/// The Chromecast glyph.
///
/// **Not an SF Symbol, because there is none.** Cast is Google's mark and Apple
/// ships no third-party trademarks in the symbol set; `AVRoutePickerView` draws
/// the AirPlay triangle and knows nothing about Cast either. So the artwork is
/// Material's own, extracted from the very font the web client renders from -
/// see `Resources/Assets.xcassets/README.md`. This is the one icon in the app
/// where the shape *is* the meaning: every other platform has trained people to
/// look for that rectangle with the three waves, and an approximation out of
/// the symbol set is not recognisable as it.
///
/// **The filled screen is what says a session is live**, and it has to be the
/// glyph rather than only the colour. `web/app.js` records why, having tried
/// the other way first: a tinted `cast` reads as *an available device* on every
/// other platform, which is the opposite of what it would mean here.
///
/// An SF Symbol takes its size from the font and an image cannot, so the size
/// is a `@ScaledMetric` and grows with Dynamic Type as a symbol would.
struct CastGlyph: View {
	var connected: Bool
	@ScaledMetric(relativeTo: .body) var size: CGFloat = 20

	var body: some View {
		Image(connected ? "CastConnectedIcon" : "CastIcon")
			.renderingMode(.template)
			.resizable()
			.scaledToFit()
			.frame(width: size, height: size)
	}
}

/// The control that opens the picker.
struct CastButton: View {
	@Environment(PlayerConnection.self) private var player
	@Binding var showing: Bool
	var size: CGFloat = 20

	var body: some View {
		Button {
			showing = true
		} label: {
			CastGlyph(connected: player.isCasting, size: size)
				.foregroundStyle(player.isCasting ? Color.accentColor : .secondary)
		}
		.accessibilityLabel(
			player.castDevice.map { "Casting to \($0.name)" } ?? "Play on another device")
	}
}

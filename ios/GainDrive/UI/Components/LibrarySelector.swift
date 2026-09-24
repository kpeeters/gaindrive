//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The browse-scope picker, mirroring `ui/components/LibrarySelector.kt`.
///
/// Android passes six parameters; this takes none, because the selection lives
/// in the environment. That is the one place SwiftUI genuinely simplifies the
/// port rather than merely differing.
///
/// The *server* half renders nothing below two servers: the common case should
/// not pay for the general one, and a picker with a single entry is a control
/// that does nothing. The menu itself is always there, because the offline
/// switch has to be.
///
/// **Offline lives here rather than in Settings**, as Android's does and for
/// its reason: it answers the same question the scope does - which library am
/// I looking at - and it is flipped before a flight rather than configured
/// once.
struct LibrarySelector: View {
	@Environment(ServerSelection.self) private var selection
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		Menu {
			// **Offline first, and always present.** The menu used to render
			// nothing below two servers, which is the common case - so a mode
			// that stops every request would have been unreachable on exactly
			// the install most likely to want it. The *server* half keeps that
			// rule; the menu itself no longer does.
			Toggle("Offline", isOn: offline)
			if selection.showsSelector {
				Divider()
				Button {
					selection.selectAllServers()
				} label: {
					Label("All servers", systemImage: isAll ? "checkmark" : "")
				}
				ForEach(selection.available) { config in
					Button {
						selection.select(config.id)
					} label: {
						Label(
							config.displayName,
							systemImage: isSelected(config.id) ? "checkmark" : "")
					}
				}
			}
		} label: {
			// The label carries the state, or a mode that silently stops all
			// network traffic is invisible until somebody wonders why the
			// library shrank.
			Label(label, systemImage: settings.offlineMode ? "wifi.slash" : "server.rack")
				.labelStyle(.titleAndIcon)
				.font(.subheadline)
		}
	}

	private var offline: Binding<Bool> {
		Binding(
			get: { settings.offlineMode },
			set: { settings.offlineMode = $0 })
	}

	/// Offline wins the label: which servers are in scope matters much less
	/// than the fact that none of them is being asked.
	private var label: String {
		settings.offlineMode ? "Offline" : selection.currentName
	}

	private var isAll: Bool {
		if case .allServers = selection.scope { return true }
		return false
	}

	private func isSelected(_ id: ServerId) -> Bool {
		if case .oneServer(let selected) = selection.scope { return selected == id }
		return false
	}
}

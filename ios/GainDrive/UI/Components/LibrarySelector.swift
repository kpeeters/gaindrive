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
/// Renders nothing below two servers: the common case should not pay for the
/// general one, and a picker with a single entry is a control that does
/// nothing.
///
/// Offline mode is **not** here. Android's component hosts it because offline
/// and scope answer the same question, and it belongs here too — but it arrives
/// with the cache in phase 5, so this is where to put it.
struct LibrarySelector: View {
	@Environment(ServerSelection.self) private var selection

	var body: some View {
		if selection.showsSelector {
			Menu {
				Button {
					selection.selectAllServers()
				} label: {
					Label("All servers", systemImage: isAll ? "checkmark" : "")
				}
				Divider()
				ForEach(selection.available) { config in
					Button {
						selection.select(config.id)
					} label: {
						Label(
							config.displayName,
							systemImage: isSelected(config.id) ? "checkmark" : "")
					}
				}
			} label: {
				Label(selection.currentName, systemImage: "server.rack")
					.labelStyle(.titleAndIcon)
					.font(.subheadline)
			}
		}
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

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What a Cast receiver is sent. Choosing a receiver is the player's business,
/// behind the cast button.
struct CastSettingsView: View {
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		SettingsForm {
			Section {
				@Bindable var settings = settings
				Toggle("Cast at original quality", isOn: $settings.castOriginal)
			} footer: {
				// The ways it will not apply, said here because this is the
				// only place that answers it beforehand.
				Text(
					"""
					Send the file as it is stored rather than the playback \
					quality. Your account's bitrate limit still applies, and a \
					file in a format the receiver cannot decode is converted \
					anyway. Track info in Now Playing shows what is actually \
					being sent.
					""")
			}
		}
		.paneHeader("Casting")
		.inlineTitle()
	}
}

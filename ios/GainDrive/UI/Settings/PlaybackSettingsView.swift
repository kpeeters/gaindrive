//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What the server is asked to send.
///
/// One global setting rather than one per server: a queue may span servers and
/// would otherwise change quality at every boundary. The account's own
/// `maxBitRate` still applies on top, per track, from that track's own
/// server - which is why the effective figure is shown in the track-info view
/// and not here, where it could only be a guess.
struct PlaybackSettingsView: View {
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		Form {
			Section {
				Picker("Format", selection: format) {
					// **`AudioFormat` has no Opus, Vorbis or Ogg entry**, and
					// this picker is exactly why that absence is in the enum
					// rather than in the list: Apple ships no Ogg demuxer, so
					// offering one would let the app be configured into
					// silence. Android's default is Opus 160; ours cannot be.
					ForEach(AudioFormat.allCases, id: \.self) { option in
						Text(option.label).tag(option)
					}
				}
				if settings.audioQuality.format != .original {
					Picker("Bitrate", selection: bitRate) {
						ForEach(AudioQuality.bitRates, id: \.self) { rate in
							Text("\(rate) kbps").tag(rate)
						}
					}
				}
			} footer: {
				Text(footerText)
			}
		}
		.navigationTitle("Playback")
		.navigationBarTitleDisplayMode(.inline)
	}

	/// Written as whole `AudioQuality` values rather than as bindings into its
	/// fields, which are `let`s. That is deliberate in the model: a format and
	/// a bitrate that could be set independently are two settings pretending to
	/// be one, and the stored form is a single tag for the same reason.
	private var format: Binding<AudioFormat> {
		Binding(
			get: { settings.audioQuality.format },
			set: {
				settings.audioQuality = AudioQuality(
					format: $0, bitRate: settings.audioQuality.bitRate)
			})
	}

	private var bitRate: Binding<Int> {
		Binding(
			get: { settings.audioQuality.bitRate },
			set: {
				settings.audioQuality = AudioQuality(
					format: settings.audioQuality.format, bitRate: $0)
			})
	}

	private var footerText: String {
		settings.audioQuality.format == .original
			? """
			The file is sent as it is stored, with no conversion. \
			On an account with a bitrate limit the server converts anyway.
			"""
			: """
			Applies to every server. A track already playing keeps \
			the quality it started with.
			"""
	}
}

#Preview {
	NavigationStack {
		PlaybackSettingsView()
			.environment(SettingsStore())
	}
}

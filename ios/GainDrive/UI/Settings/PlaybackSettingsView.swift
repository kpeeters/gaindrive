//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// What the server is asked to send: the original, or a rough ceiling plus the
/// format to encode to when the ceiling makes that unavoidable. A file this
/// device plays and that fits is sent as stored either way.
///
/// One global setting rather than one per server: a queue may span servers and
/// would otherwise change quality at every boundary.
struct PlaybackSettingsView: View {
	@Environment(SettingsStore.self) private var settings

	var body: some View {
		SettingsForm {
			Section {
				Picker("Quality", selection: limit) {
					Text("Original").tag(0)
					ForEach(AudioQuality.bitRates, id: \.self) { rate in
						Text("Up to \(rate) kbps").tag(rate)
					}
				}
				if settings.audioQuality.format != .original {
					Picker("Convert to", selection: format) {
						// **`AudioFormat` has no Opus, Vorbis or Ogg entry**, and
						// this picker is exactly why that absence is in the enum
						// rather than in the list: Apple ships no Ogg demuxer, so
						// offering one would let the app be configured into
						// silence. Android's default is Opus 160; ours cannot be.
						ForEach(AudioFormat.allCases.filter { $0 != .original }, id: \.self) {
							option in
							Text(option.label).tag(option)
						}
					}
				}
			} footer: {
				Text(footerText)
			}

			// Here rather than under Storage, where Android has it: the
			// question is what a video *plays as*, and the storage half of
			// it follows from that answer.
			Section {
				@Bindable var settings = settings
				Toggle("Play videos as audio only", isOn: $settings.videoAudioOnly)
			} footer: {
				Text(
					"""
					Videos play their soundtrack in the chosen format, with no \
					picture, and can be downloaded like any track. Takes effect \
					from the next track.
					""")
			}
		}
		.paneHeader("Playback")
		.inlineTitle()
	}

	/// Written as whole `AudioQuality` values rather than as bindings into its
	/// fields, which are `let`s: the stored form is a single tag. 0 is the
	/// original.
	private var limit: Binding<Int> {
		Binding(
			get: { settings.audioQuality.format == .original ? 0 : settings.audioQuality.bitRate },
			set: {
				settings.audioQuality =
					$0 == 0
					? .original : AudioQuality(format: settings.transcodeFormat, bitRate: $0)
			})
	}

	private var format: Binding<AudioFormat> {
		Binding(
			get: { settings.transcodeFormat },
			set: {
				settings.transcodeFormat = $0
				settings.audioQuality = AudioQuality(
					format: $0, bitRate: settings.audioQuality.bitRate)
			})
	}

	private var footerText: String {
		settings.audioQuality.format == .original
			? """
			Files this device can play are sent as stored. Anything else is \
			converted to FLAC, so nothing is lost.
			"""
			: """
			Files this device can play are sent as stored when they fit under \
			the limit; anything else is converted at the limit. Applies to \
			every server. A track already playing keeps the quality it \
			started with.
			"""
	}
}

#Preview {
	NavigationStack {
		PlaybackSettingsView()
			.environment(SettingsStore())
	}
}

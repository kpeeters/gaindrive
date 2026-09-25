//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// A fetch in progress, above the mini player in every tab. Android's
/// `FetchStrip`: a fetch takes minutes, and the form it was started from is
/// long closed by the time it lands. Tapping it reopens the form, where the
/// job can be followed or cancelled.
struct FetchStrip: View {
	@Environment(Fetches.self) private var fetches
	@Environment(SettingsStore.self) private var settings
	@Environment(\.library) private var library
	@State private var open = false

	var body: some View {
		if let job = fetches.moving ?? fetches.live.first {
			Button {
				open = true
			} label: {
				HStack(spacing: 10) {
					Image(systemName: job.audio ? "arrow.down.circle" : "film")
						.foregroundStyle(Color.accentColor)
					VStack(alignment: .leading, spacing: 2) {
						Text(job.title).lineLimit(1)
						Text(summary(job))
							.font(.caption)
							.foregroundStyle(.secondary)
					}
					Spacer(minLength: 0)
					if job.state == .running {
						ProgressView(value: Double(job.percent), total: 100)
							.frame(width: 60)
					}
				}
				.padding(.horizontal, 12)
				.padding(.vertical, 6)
				.contentShape(.rect)
			}
			.buttonStyle(.plain)
			.background(.bar)
			.overlay(alignment: .top) { Divider() }
			.accessibilityLabel("Fetching \(job.title), \(job.state.label)")
			// Explicit, for the reason every sheet raised from the tab content
			// is: the inherited environment is not reliable there.
			.sheet(isPresented: $open) {
				FetchUrlView()
					.environment(fetches)
					.environment(settings)
					.environment(\.library, library)
			}
		}
	}

	/// The state, plus how many more are waiting behind this one.
	private func summary(_ job: FetchJob) -> String {
		let others = fetches.live.count - 1
		return others > 0 ? "\(job.state.label) · \(others) more" : job.state.label
	}
}

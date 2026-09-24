//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Names the servers that did not answer, above what did load.
///
/// Above the list rather than over it: what arrived is still worth using, and
/// **a screen must never be blank because the least important of three servers
/// is down**.
///
/// One line per server, named. "Two servers failed" tells the user nothing
/// about which of their libraries is missing - and with several servers, which
/// one it was is the whole question.
struct PartialFailureNote: View {
	let failures: [ServerFailure]
	let onRetry: () -> Void
	let onDismiss: () -> Void

	var body: some View {
		VStack(alignment: .leading, spacing: 6) {
			ForEach(failures) { failure in
				Label {
					VStack(alignment: .leading, spacing: 1) {
						Text(failure.serverName).font(.footnote.weight(.medium))
						Text(failure.message)
							.font(.caption)
							.foregroundStyle(.secondary)
					}
				} icon: {
					Image(systemName: "exclamationmark.triangle.fill")
						.foregroundStyle(.orange)
				}
			}
			HStack {
				Button("Retry", action: onRetry)
				Spacer()
				Button("Dismiss", action: onDismiss)
			}
			.font(.footnote)
			.buttonStyle(.borderless)
		}
		.padding(10)
		.frame(maxWidth: .infinity, alignment: .leading)
		.background(.orange.opacity(0.12), in: RoundedRectangle(cornerRadius: 8, style: .continuous))
		.listRowInsets(EdgeInsets(top: 4, leading: 12, bottom: 4, trailing: 12))
	}
}

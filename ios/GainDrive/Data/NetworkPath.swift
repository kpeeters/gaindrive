//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation
import Network

/// Whether the current network is one the user pays for by the byte.
///
/// Only for the marks: it lets a download held back by "Wi-Fi only" say so,
/// rather than showing a ring stuck at zero. The holding back itself is the
/// system's, through `URLRequest.allowsExpensiveNetworkAccess`, so this
/// being a moment late costs a wrong icon and nothing else.
@MainActor
@Observable
final class NetworkPath {
	/// Cellular or a personal hotspot. False until the first answer, which
	/// is the harmless direction: a clock drawn a moment late.
	private(set) var isExpensive = false

	@ObservationIgnored private let monitor = NWPathMonitor()

	init() {
		monitor.pathUpdateHandler = { [weak self] path in
			let expensive = path.isExpensive
			Task { @MainActor in self?.isExpensive = expensive }
		}
		monitor.start(queue: DispatchQueue(label: "org.gaindrive.network-path"))
	}
}

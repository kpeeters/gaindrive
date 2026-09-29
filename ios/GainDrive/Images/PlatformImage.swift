//	GainDrive for iOS and macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

//	The one image type the covers, the cache and Now Playing pass around:
//	`UIImage` on iOS, `NSImage` on the Mac. Both decode from `Data` with the
//	same initialiser, so the cache's read and write paths do not branch; only
//	the handful of places that differ are spelled out here.

#if os(macOS)
	import AppKit
	typealias PlatformImage = NSImage

	/// `UIImage` is `Sendable` and `NSImage` is not, while the covers cross
	/// actors - out of `ImageStore`, into Now Playing's artwork closure. They
	/// are decoded once and never mutated afterwards, which is the condition
	/// under which sharing one between threads is safe; stated here once
	/// rather than wrapped at every use.
	extension NSImage: @retroactive @unchecked Sendable {}
#else
	import UIKit
	typealias PlatformImage = UIImage
#endif

extension PlatformImage {
	/// Decoded pixels, for the memory cache's cost. `NSImage` has no `scale`;
	/// its size is in points and its representations carry the pixels.
	var pixelCount: Int {
		#if os(macOS)
			let rep = representations.max { $0.pixelsWide * $0.pixelsHigh < $1.pixelsWide * $1.pixelsHigh }
			if let rep { return rep.pixelsWide * rep.pixelsHigh }
			return Int(size.width * size.height)
		#else
			return Int(size.width * size.height * scale * scale)
		#endif
	}
}

extension Image {
	init(platformImage image: PlatformImage) {
		#if os(macOS)
			self.init(nsImage: image)
		#else
			self.init(uiImage: image)
		#endif
	}
}

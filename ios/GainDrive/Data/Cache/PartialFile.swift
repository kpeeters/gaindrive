//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What of a read can be answered from what has arrived.
///
/// **The whole of the cache's bookkeeping is one integer**, and that is the
/// point of filling sequentially rather than by range. What is present is
/// always a prefix, so "have I got this?" is a comparison rather than a search
/// through a set of intervals — which is the part of a general byte cache that
/// is hard to get right and impossible to eyeball.
///
/// Pure, so the arithmetic the design rests on is tested by calling it.
enum PartialRead {
	/// The bytes to hand over now. Possibly **shorter than asked for**: a read
	/// straddling the boundary is served up to it and the rest waited for,
	/// which is what keeps playback moving while the file is still arriving.
	/// `AVAssetResourceLoadingDataRequest` is built for exactly this — it
	/// advances `currentOffset` and the request stays open.
	static func chunk(currentOffset: Int, end: Int, received: Int) -> Range<Int>? {
		guard currentOffset < end else { return nil }
		guard currentOffset < received else { return nil }
		return currentOffset..<min(end, received)
	}

	/// Whether a read has been answered in full and its request may be closed.
	static func isSatisfied(currentOffset: Int, end: Int) -> Bool {
		currentOffset >= end
	}

	/// Where a read ends.
	///
	/// `requestsAllDataToEndOfResource` means "to the end", and the requested
	/// length is not to be trusted in that case — AVFoundation puts a nominal
	/// figure there. Falling for it truncates the last read of every track,
	/// which plays fine and then stops early.
	static func end(
		requestedOffset: Int, requestedLength: Int, toEnd: Bool, total: Int?
	) -> Int {
		guard toEnd, let total else { return requestedOffset + requestedLength }
		return total
	}
}

/// The bytes of one resource as they arrive.
///
/// Written strictly forward and read at any offset below what has been
/// written. **Not thread-safe on purpose**: it is confined to
/// `CachingResourceLoader`'s serial queue, which is also the queue both
/// AVFoundation and `URLSession` call back on, so there is nothing to
/// synchronise.
final class PartialFile {
	let url: URL
	private(set) var received = 0

	private let writer: FileHandle
	private let reader: FileHandle

	init?(url: URL) {
		self.url = url
		let manager = FileManager.default
		try? manager.createDirectory(
			at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
		// Truncated rather than resumed. A part file left by a previous run
		// cannot be trusted: nothing recorded how much of it was valid, and
		// half a track that claims to be whole is worse than no track.
		try? manager.removeItem(at: url)
		guard manager.createFile(atPath: url.path(percentEncoded: false), contents: nil),
			let writer = try? FileHandle(forWritingTo: url),
			let reader = try? FileHandle(forReadingFrom: url)
		else { return nil }
		self.writer = writer
		self.reader = reader
	}

	func append(_ data: Data) {
		guard !data.isEmpty else { return }
		try? writer.write(contentsOf: data)
		// Flushed so the read handle sees it. Without this a read can come back
		// short of what `received` promises, and the request stalls waiting for
		// bytes that are sitting in a buffer.
		try? writer.synchronize()
		received += data.count
	}

	func read(_ range: Range<Int>) -> Data? {
		guard !range.isEmpty, range.upperBound <= received else { return nil }
		try? reader.seek(toOffset: UInt64(range.lowerBound))
		return try? reader.read(upToCount: range.count)
	}

	func close() {
		try? writer.close()
		try? reader.close()
	}

	/// Nothing incomplete is ever visible, which is what keeps
	/// `AudioStore.storedFile` honest.
	func discard() {
		close()
		try? FileManager.default.removeItem(at: url)
	}
}

//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// What the user asked to keep, what that expands to, and how much of it is
/// here.
///
/// The pins themselves are persisted as a **Codable file, not a database**:
/// this is tens of rows of intent, and the thing that genuinely needs one is
/// the metadata mirror, which is a later stage. Adding a dependency for a list
/// this small would be a dependency taken for the wrong reason.
@MainActor
@Observable
final class PinRepository {
	private(set) var pins: [Pin] = []
	private(set) var usageBytes: Int64 = 0
	/// A refusal worth showing - today only "this would not fit". Cleared when
	/// the screen that showed it says so.
	private(set) var message: String?

	private var membership: [Pin.ID: [ItemRef]] = [:]
	/// Pinned **and** here. Everything else on disk is `cached`, which the
	/// store answers for directly - there is no list of it, because the list
	/// would be out of date the moment eviction ran.
	private var stored: Set<ItemRef> = []
	/// Everything the store holds, pinned or not - a track kept by playing it
	/// belongs to no pin, so it is in nothing else this class computes.
	///
	/// **Read from the store, never asked for per row.** A row cannot await the
	/// disk while it is being drawn, and asking during a view update would
	/// mutate observable state mid-render.
	private var held: Set<ItemRef> = []
	private var progress: [ItemRef: Double] = [:]
	private var failed: Set<ItemRef> = []

	@ObservationIgnored private let library: LibraryRepository
	@ObservationIgnored private let store: AudioStore
	@ObservationIgnored private let queue: DownloadQueue
	@ObservationIgnored private let targets: StreamTargets
	@ObservationIgnored private let settings: SettingsStore
	@ObservationIgnored private let file: URL

	init(
		library: LibraryRepository, store: AudioStore, queue: DownloadQueue,
		targets: StreamTargets, settings: SettingsStore
	) {
		self.library = library
		self.store = store
		self.queue = queue
		self.targets = targets
		self.settings = settings
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
		self.file = base.appending(path: "pins.json")
		load()

		queue.onProgress = { [weak self] ref, fraction in
			self?.progress[ref] = fraction
			self?.failed.remove(ref)
		}
		queue.onFinished = { [weak self] ref in
			guard let self else { return }
			self.progress[ref] = nil
			self.failed.remove(ref)
			self.stored.insert(ref)
			Task { await self.reloadHeld() }
		}
		queue.onFailed = { [weak self] ref in
			self?.progress[ref] = nil
			self?.failed.insert(ref)
		}
	}

	// MARK: - Reading

	func isPinned(_ ref: ItemRef, kind: PinKind) -> Bool {
		pins.contains { $0.ref == ref && $0.kind == kind }
	}

	/// One track's state.
	///
	/// **`stored` and `cached` are not the same claim.** A pinned track is safe
	/// from eviction; one kept because it was played can go tonight, and
	/// collapsing them would promise a permanence the second does not have.
	func state(for song: ItemRef) -> DownloadState {
		if stored.contains(song) { return .stored }
		if let fraction = progress[song] { return .running(fraction: fraction) }
		if failed.contains(song) { return .failed }
		if held.contains(song) { return .cached }
		return .absent
	}

	/// The store telling us what it holds has changed - a track finished
	/// arriving, or eviction took one. Wired in the composition root.
	func storeChanged() {
		Task { await reloadHeld() }
	}

	private func reloadHeld() async {
		held = await store.heldRefs()
		usageBytes = await store.totalBytes()
	}

	/// A whole pin's state, which is what the album screen's control shows.
	///
	/// **It reports what is happening, not what was asked for** - the reason
	/// `android/SCREENS.md` gives for the same control. A pin that is half here
	/// is running, not done, and one whose last track failed is failed however
	/// many succeeded.
	func state(of pin: Pin) -> DownloadState {
		let songs = Pins.expand([pin], membership: membership)
		guard !songs.isEmpty else {
			// Pinned, but never resolved - a pin whose album has not been read
			// yet protects nothing, and saying "here" would be a lie.
			return .running(fraction: 0)
		}
		if songs.contains(where: { failed.contains($0) }) { return .failed }
		let here = songs.filter { stored.contains($0) }.count
		if here == songs.count { return .stored }
		return .running(fraction: Double(here) / Double(songs.count))
	}

	func clearMessage() {
		message = nil
	}

	// MARK: - Writing

	func toggle(_ pin: Pin) async {
		if isPinned(pin.ref, kind: pin.kind) {
			await remove(pin)
		} else {
			await add(pin)
		}
	}

	private func add(_ pin: Pin) async {
		// A song pin is its own membership and needs no read, which is also
		// why it is the one kind that cannot fail to resolve.
		let songs = pin.kind == .song ? [] : await resolve(pin)
		guard pin.kind == .song || !songs.isEmpty else {
			message = "Could not read what that holds. Try again when the server answers."
			return
		}
		let refs = pin.kind == .song ? [pin.ref] : songs.map(\.ref)

		// **Refused rather than allowed to overrun.** Nothing here is evicted,
		// so pinning past the cap would quietly turn the cap into a lie -
		// `android/CACHING.md` reaches the same rule from the other direction,
		// where eviction exists but cannot reclaim pinned bytes.
		//
		// A song pin contributes nothing to the estimate, since its size was
		// never read. That errs towards letting one track through rather than
		// refusing it for want of a number, which is the right direction for a
		// figure that is an estimate either way.
		let needed = songs.filter { !stored.contains($0.ref) }.reduce(Int64(0)) {
			$0 + Pins.estimatedBytes(of: $1, quality: settings.audioQuality)
		}
		// **Against the pinned bytes, not the used bytes.** Everything else on
		// disk was kept because it was played and gives way to eviction, so
		// counting it would refuse a pin that fits perfectly well.
		let room = settings.cacheCapBytes - (await store.protectedBytes())
		guard needed <= room else {
			message = """
				Not enough room. That needs about \(Self.readable(needed)) \
				and \(Self.readable(max(room, 0))) is left. Raise the limit in \
				Storage, or remove something.
				"""
			return
		}

		pins.append(pin)
		if pin.kind != .song { membership[pin.id] = refs }
		persist()
		await pushLimits()
		await startMissing(refs)
	}

	func remove(_ pin: Pin) async {
		let before = Pins.expand(pins, membership: membership)
		pins.removeAll { $0.id == pin.id }
		membership = Pins.pruned(membership, to: pins)
		persist()

		// **Unpinning removes the bytes**, which is what "remove download" is
		// taken to mean and what stops orphaned files nothing lists. Only the
		// songs no *other* pin still covers: an album and a playlist may hold
		// the same track.
		let after = Pins.expand(pins, membership: membership)
		await pushLimits()
		for ref in before.subtracting(after) {
			queue.cancel(ref)
			progress[ref] = nil
			failed.remove(ref)
			stored.remove(ref)
			await store.remove(ref)
		}
		await reloadHeld()
	}

	/// Everything, including the pins - there is nothing on disk that is not
	/// pinned, so a flush that kept them would re-download immediately.
	func removeEverything() async {
		queue.cancelAll()
		pins = []
		membership = [:]
		progress = [:]
		failed = []
		stored = []
		persist()
		await pushLimits()
		await store.removeAll()
		await reloadHeld()
	}

	// MARK: - Refresh

	/// The cap changed in Settings; the store enforces it, so it has to hear.
	func capChanged() async {
		await pushLimits()
	}

	/// Re-reads what each pin covers and fetches anything missing.
	///
	/// This is what makes a pinned playlist cover a track added since - the
	/// reason a pin records intent rather than a song list. Run at launch and
	/// whenever the Storage screen appears.
	func refresh() async {
		await syncStored()
		progress = await queue.inFlight()
		queue.resume()

		var resolved: [Pin.ID: [ItemRef]] = [:]
		for pin in pins where pin.kind != .song {
			resolved[pin.id] = await resolve(pin).map(\.ref)
		}
		membership = Pins.pruned(
			Pins.merging(membership, resolved: resolved), to: pins)
		persist()
		await startMissing(Array(Pins.expand(pins, membership: membership)))
	}

	// MARK: - Machinery

	/// Empty for a song pin, which is its own membership - `Pins.expand` puts
	/// the ref straight in, so there is nothing to read and nothing to store.
	private func resolve(_ pin: Pin) async -> [Song] {
		switch pin.kind {
		case .song:
			return []
		// **Videos are never downloaded**, whatever they are pinned inside. One
		// film evicts the whole stored library, and a re-encoded one arrives
		// with no `Content-Length` so completeness could never be established.
		// Dropped here rather than at the download, so a mostly-audio album
		// pins cleanly and its film is simply not part of what the pin covers.
		case .album:
			return ((try? await library.albumDetail(pin.ref))?.songs ?? [])
				.filter { !$0.isVideo }
		case .playlist:
			return ((try? await library.playlist(pin.ref))?.songs ?? [])
				.filter { !$0.isVideo }
		}
	}

	private func startMissing(_ refs: [ItemRef]) async {
		for ref in refs where !stored.contains(ref) && progress[ref] == nil {
			// The same declaration playback makes, and it must be the same:
			// `StreamTargets` is shared precisely so a download and a play
			// cannot ask for different bytes, and they derive one cache key.
			guard
				let target = await targets.target(
					for: ref, playable: avfoundationPlayable(for:))
			else { continue }
			progress[ref] = 0
			queue.start(ref, quality: target.quality, url: target.url)
		}
	}

	private func syncStored() async {
		let wanted = Pins.expand(pins, membership: membership)
		stored = await store.storedRefs(among: wanted)
		await pushLimits()
		await reloadHeld()
	}

	/// Tells the store what it may not evict, and how much room it has.
	///
	/// Pushed rather than pulled: the store knows nothing about pins and should
	/// not learn, and this class already computes the set for its own answers.
	private func pushLimits() async {
		await store.setLimits(
			cap: settings.cacheCapBytes,
			protecting: Pins.expand(pins, membership: membership))
	}

	// MARK: - Persistence

	private struct Record: Codable {
		var pins: [Pin] = []
		var membership: [String: [ItemRef]] = [:]
	}

	private func load() {
		guard let data = try? Data(contentsOf: file),
			let record = try? JSONDecoder().decode(Record.self, from: data)
		else { return }
		pins = record.pins
		membership = record.membership
	}

	private func persist() {
		let record = Record(pins: pins, membership: membership)
		guard let data = try? JSONEncoder().encode(record) else { return }
		try? data.write(to: file, options: .atomic)
	}

	static func readable(_ bytes: Int64) -> String {
		ByteCountFormatter.string(fromByteCount: bytes, countStyle: .file)
	}
}

extension Pins {
	/// What a track is likely to take at this quality.
	///
	/// **Not `sizeBytes`**, which is the size of the file as stored. Asking for
	/// AAC 160 of a FLAC album would then overestimate by five or six times and
	/// refuse a pin that fits comfortably. The rate is the one actually being
	/// requested, so the arithmetic is the same one the server will do.
	static func estimatedBytes(of song: Song, quality: AudioQuality) -> Int64 {
		guard quality.format != .original else { return Int64(song.sizeBytes) }
		guard song.duration > 0 else { return Int64(song.sizeBytes) }
		return Int64(song.duration) * Int64(quality.bitRate) * 1000 / 8
	}
}

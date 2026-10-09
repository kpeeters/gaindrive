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
	/// What "Free" would give back: everything kept from playing.
	private(set) var evictableBytes: Int64 = 0
	/// A refusal worth showing - today only "this would not fit". Cleared when
	/// the screen that showed it says so.
	private(set) var message: String?
	/// A pin that does not fit, and the smallest limit that would hold it.
	/// Asked about rather than refused, since raising the limit is usually
	/// what the user wants.
	private(set) var roomOffer: RoomOffer?

	struct RoomOffer {
		let pin: Pin
		let song: Song?
		let needed: Int64
		let left: Int64
		let newCap: Int64
	}

	private var membership: [Pin.ID: [ItemRef]] = [:]
	/// Pinned refs that are videos, kept for their soundtrack under "Play
	/// videos as audio only". A ref alone cannot say, and a video resolved
	/// through the ordinary audio path with "Original" set would download the
	/// film.
	private var videos: Set<ItemRef> = []
	/// Pinned refs that are films, with the title the system's download
	/// list shows. Downloaded whole, as HLS, into `FilmStore`.
	private var films: [ItemRef: String] = [:]
	/// The covers each pin's songs are drawn with, so they can be kept out of
	/// the image cache's eviction - a downloaded album offline should not lose
	/// its sleeve to a scroll through the library. Per pin, so removing one
	/// releases exactly its covers and not another's.
	private var coversOf: [Pin.ID: [ItemRef]] = [:]
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
	@ObservationIgnored private let filmQueue: FilmQueue
	@ObservationIgnored private let filmStore: FilmStore
	@ObservationIgnored private let captions: CaptionTracks
	@ObservationIgnored private let targets: StreamTargets
	@ObservationIgnored private let settings: SettingsStore
	@ObservationIgnored private let network: NetworkPath
	@ObservationIgnored private let file: URL

	init(
		library: LibraryRepository, store: AudioStore, queue: DownloadQueue,
		filmQueue: FilmQueue, filmStore: FilmStore, captions: CaptionTracks,
		targets: StreamTargets, settings: SettingsStore, network: NetworkPath
	) {
		self.library = library
		self.store = store
		self.queue = queue
		self.filmQueue = filmQueue
		self.filmStore = filmStore
		self.captions = captions
		self.targets = targets
		self.settings = settings
		self.network = network
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
		self.file = base.appending(path: "pins.json")
		load()

		let onProgress: @MainActor (ItemRef, Double) -> Void = { [weak self] ref, fraction in
			self?.progress[ref] = fraction
			self?.failed.remove(ref)
		}
		let onFinished: @MainActor (ItemRef) -> Void = { [weak self] ref in
			guard let self else { return }
			self.progress[ref] = nil
			self.failed.remove(ref)
			self.stored.insert(ref)
			Task { await self.reloadHeld() }
		}
		let onFailed: @MainActor (ItemRef) -> Void = { [weak self] ref in
			self?.progress[ref] = nil
			self?.failed.insert(ref)
		}
		queue.onProgress = onProgress
		queue.onFinished = onFinished
		queue.onFailed = onFailed
		filmQueue.onProgress = onProgress
		filmQueue.onFinished = { [weak self] ref in
			guard let self else { return }
			// Unpinned while the session was finishing: nothing would ever
			// list the package again.
			guard self.films[ref] != nil else {
				Task { await self.filmStore.remove(ref) }
				return
			}
			onFinished(ref)
			Task { await self.storeCaptions(of: ref) }
		}
		filmQueue.onFailed = onFailed
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
		if let fraction = progress[song] {
			return isHeldBack ? .waiting : .running(fraction: fraction)
		}
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
		held = (await store.heldRefs()).union(await filmStore.heldRefs())
		let audio = await store.totalBytes()
		usageBytes = audio + (await filmStore.totalBytes())
		evictableBytes = max(audio - (await store.protectedBytes()), 0)
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
		if isHeldBack { return .waiting }
		return .running(fraction: Double(here) / Double(songs.count))
	}

	/// "Wi-Fi only" is on and the network is metered, so nothing in flight
	/// is moving. Read from the observed path, so the marks follow it.
	private var isHeldBack: Bool {
		settings.downloadUnmeteredOnly && network.isExpensive
	}

	/// A pinned album's or playlist's state for its row in a listing, or nil
	/// when it is not pinned.
	func containerState(_ ref: ItemRef, kind: PinKind) -> DownloadState? {
		pins.first { $0.ref == ref && $0.kind == kind }.map { state(of: $0) }
	}

	func clearMessage() {
		message = nil
	}

	/// Raises the limit to what the offer named and pins again. The offer
	/// is cleared before returning, so the alert showing it cannot reappear.
	func acceptRoomOffer() {
		guard let offer = roomOffer else { return }
		roomOffer = nil
		settings.cacheCapBytes = offer.newCap
		Task {
			await capChanged()
			await add(offer.pin, song: offer.song)
		}
	}

	func declineRoomOffer() {
		roomOffer = nil
	}

	// MARK: - Writing

	/// `song` matters only for a song pin, whose ref is all it carries; an
	/// album or playlist learns what it holds from its songs.
	func toggle(_ pin: Pin, song: Song? = nil) async {
		if isPinned(pin.ref, kind: pin.kind) {
			await remove(pin)
		} else {
			await add(pin, song: song)
		}
	}

	private func add(_ pin: Pin, song: Song?) async {
		// A song pin is its own membership and needs no read, which is also
		// why it is the one kind that cannot fail to resolve.
		let songs = pin.kind == .song ? [] : await resolve(pin)
		guard pin.kind == .song || !songs.isEmpty else {
			message = "Could not read what that holds. Try again when the server answers."
			return
		}
		let refs = pin.kind == .song ? [pin.ref] : songs.map(\.ref)
		// A song pin is sized only when it is a film. Its size was otherwise
		// never read, and erring towards letting one track through is the
		// right direction for a figure that is an estimate either way.
		let sized = pin.kind == .song ? [song].compactMap { $0 }.filter(settings.showsPicture) : songs

		// **Never allowed to overrun.** Nothing here is evicted, so pinning
		// past the cap would quietly turn the cap into a lie -
		// `android/CACHING.md` reaches the same rule from the other direction,
		// where eviction exists but cannot reclaim pinned bytes. What does not
		// fit is offered a larger limit, or refused when none would do.
		//
		// A film is its downloaded size, which for a playlist the server
		// re-encodes is not known in advance. The file's own size is the best
		// figure without fetching the playlist, and is near exact when the
		// picture is copied.
		let needed = sized.filter { !stored.contains($0.ref) }.reduce(Int64(0)) {
			$0 + (settings.showsPicture($1)
				? Int64($1.sizeBytes) : Pins.estimatedBytes(of: $1, quality: settings.audioQuality))
		}
		// **Against the pinned bytes, not the used bytes.** Everything else on
		// disk was kept because it was played and gives way to eviction, so
		// counting it would refuse a pin that fits perfectly well. Films are
		// all pinned.
		let pinned = (await store.protectedBytes()) + (await filmStore.totalBytes())
		let room = settings.cacheCapBytes - pinned
		guard needed <= room else {
			let newCap = SettingsStore.cacheCapChoices.first { $0 >= pinned + needed }
			let free = Self.freeSpace()
			if let newCap, needed <= free {
				roomOffer = RoomOffer(
					pin: pin, song: song, needed: needed, left: max(room, 0), newCap: newCap)
			} else if newCap == nil {
				message = """
					Not enough room. That needs about \(Self.readable(needed)), \
					more than the largest storage limit allows.
					"""
			} else {
				message = """
					Not enough room. That needs about \(Self.readable(needed)) \
					and this device has \(Self.readable(free)) free.
					"""
			}
			return
		}

		pins.append(pin)
		if pin.kind != .song { membership[pin.id] = refs }
		if pin.kind == .song, let song { classify(song) }
		songs.forEach(classify)
		coversOf[pin.id] =
			pin.kind == .song ? [song?.coverArt].compactMap { $0 } : Self.covers(of: songs)
		persist()
		await pushLimits()
		await startMissing(refs)
	}

	func remove(_ pin: Pin) async {
		let before = Pins.expand(pins, membership: membership)
		pins.removeAll { $0.id == pin.id }
		membership = Pins.pruned(membership, to: pins)
		coversOf[pin.id] = nil
		persist()

		// **Unpinning removes the bytes**, which is what "remove download" is
		// taken to mean and what stops orphaned files nothing lists. Only the
		// songs no *other* pin still covers: an album and a playlist may hold
		// the same track.
		let after = Pins.expand(pins, membership: membership)
		await pushLimits()
		for ref in before.subtracting(after) {
			queue.cancel(ref)
			filmQueue.cancel(ref)
			progress[ref] = nil
			failed.remove(ref)
			stored.remove(ref)
			await store.remove(ref)
			await filmStore.remove(ref)
		}
		await reloadHeld()
	}

	/// Empties what was kept from playing and leaves every download. The
	/// limits are pushed first, so the store's idea of what is protected is
	/// this class's current one and not whatever it last heard.
	func freeEvictable() async {
		await pushLimits()
		await store.removeUnprotected()
		await reloadHeld()
	}

	/// Everything, including the pins - a flush that kept them would
	/// re-download them immediately.
	func removeEverything() async {
		queue.cancelAll()
		filmQueue.cancelAll()
		pins = []
		membership = [:]
		coversOf = [:]
		progress = [:]
		failed = []
		stored = []
		persist()
		await pushLimits()
		await store.removeAll()
		await filmStore.removeAll()
		await reloadHeld()
	}

	/// "Wi-Fi only" was switched. A task carries its network policy from the
	/// moment it was created, so the ones in flight are restarted under the
	/// new one; what had arrived of them is fetched again.
	func downloadPolicyChanged() async {
		await queue.cancelAllNow()
		await filmQueue.cancelAllNow()
		progress = [:]
		failed = []
		await startMissing(Array(Pins.expand(pins, membership: membership)))
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
		progress = (await queue.inFlight()).merging(await filmQueue.inFlight()) { $1 }
		queue.resume()
		filmQueue.resume()

		var resolved: [Pin.ID: [ItemRef]] = [:]
		for pin in pins where pin.kind != .song {
			let songs = await resolve(pin)
			resolved[pin.id] = songs.map(\.ref)
			songs.forEach(classify)
			// Only a pin that resolved: one that could not be read keeps the
			// covers it had, as its membership does.
			if !songs.isEmpty { coversOf[pin.id] = Self.covers(of: songs) }
		}
		membership = Pins.pruned(
			Pins.merging(membership, resolved: resolved), to: pins)
		persist()
		await startMissing(Array(Pins.expand(pins, membership: membership)))
		// Captions are fetched once a film lands; one that landed while the
		// server could not be reached has none yet.
		for ref in films.keys where stored.contains(ref) {
			if await filmStore.captions(for: ref) == nil { await storeCaptions(of: ref) }
		}
	}

	// MARK: - Machinery

	/// Which store a video goes to follows "Play videos as audio only" at the
	/// time it is resolved: a film, or a soundtrack.
	private func classify(_ song: Song) {
		guard song.isVideo else { return }
		if settings.showsPicture(song) {
			films[song.ref] = song.title
			videos.remove(song.ref)
		} else {
			videos.insert(song.ref)
			films[song.ref] = nil
		}
	}

	/// All tracks, so the offline player offers what the online one does.
	private func storeCaptions(of ref: ItemRef) async {
		guard let tracks = await captions.download(for: ref) else { return }
		await filmStore.saveCaptions(tracks, for: ref)
	}

	/// What the volume holding Application Support could still take.
	private static func freeSpace() -> Int64 {
		let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
		let values = try? base.resourceValues(forKeys: [.volumeAvailableCapacityForImportantUsageKey])
		return values?.volumeAvailableCapacityForImportantUsage ?? 0
	}

	/// Empty for a song pin, which is its own membership - `Pins.expand` puts
	/// the ref straight in, so there is nothing to read and nothing to store.
	private func resolve(_ pin: Pin) async -> [Song] {
		switch pin.kind {
		case .song:
			return []
		// An album keeps its films: a film album is pinned for them. They
		// download as HLS into `FilmStore`; see `FilmQueue`.
		case .album:
			return (try? await library.albumDetail(pin.ref))?.songs ?? []
		// **A playlist drops its films.** One is a few gigabytes, which a
		// playlist of music pinned for the car should not quietly take.
		//
		// A video played for its soundtrack is not a film here: that is an
		// ordinary audio transcode, and Android downloads it too. Turning the
		// setting off drops it from the next resolve, which leaves its bytes
		// unprotected and so evictable.
		case .playlist:
			return ((try? await library.playlist(pin.ref))?.songs ?? [])
				.filter { !settings.showsPicture($0) }
		}
	}

	private func startMissing(_ refs: [ItemRef]) async {
		for ref in refs where !stored.contains(ref) && progress[ref] == nil {
			// A film is its HLS playlist, fetched whole by the system's asset
			// downloader. See `FilmQueue`.
			if let title = films[ref] {
				guard let url = targets.hls(for: ref) else { continue }
				progress[ref] = 0
				filmQueue.start(
					ref, title: title, url: url, allowsExpensive: !settings.downloadUnmeteredOnly)
				continue
			}
			// The same declaration playback makes, and it must be the same:
			// `StreamTargets` is shared precisely so a download and a play
			// cannot ask for different bytes, and they derive one cache key.
			// A pinned video is its soundtrack, resolved exactly as playback
			// resolves it so the two derive one cache key.
			let resolved: StreamTarget?
			if videos.contains(ref) {
				resolved = targets.soundtrack(for: ref)
			} else {
				resolved = targets.target(for: ref, playable: avfoundationAudio)
			}
			guard let target = resolved else { continue }
			progress[ref] = 0
			queue.start(
				ref, quality: target.quality, url: target.url,
				allowsExpensive: !settings.downloadUnmeteredOnly)
		}
	}

	private func syncStored() async {
		let wanted = Pins.expand(pins, membership: membership)
		let filmRefs = Set(films.keys)
		stored = (await store.storedRefs(among: wanted.subtracting(filmRefs)))
			.union((await filmStore.heldRefs()).intersection(wanted))
		await pushLimits()
		await reloadHeld()
	}

	/// Tells the store what it may not evict, and how much room it has.
	///
	/// Pushed rather than pulled: the store knows nothing about pins and should
	/// not learn, and this class already computes the set for its own answers.
	private func pushLimits() async {
		// Films share the cap, so what they take is what the audio may not.
		await store.setLimits(
			cap: settings.cacheCapBytes - (await filmStore.totalBytes()),
			protecting: Pins.expand(pins, membership: membership))
	}

	// MARK: - Persistence

	private struct Record: Codable {
		var pins: [Pin] = []
		var membership: [String: [ItemRef]] = [:]
		/// Optional so a file written before it existed still decodes: the
		/// synthesised decoder skips a missing key only for an optional.
		var videos: [ItemRef]?
		/// Optional for the same reason.
		var covers: [String: [ItemRef]]?
		var films: [ItemRef: String]?
	}

	private func load() {
		guard let data = try? Data(contentsOf: file),
			let record = try? JSONDecoder().decode(Record.self, from: data)
		else { return }
		pins = record.pins
		membership = record.membership
		videos = Set(record.videos ?? [])
		coversOf = record.covers ?? [:]
		films = record.films ?? [:]
		pushCovers()
	}

	private func persist() {
		// Only what a pin still covers, so the set cannot grow without bound.
		let covered = Pins.expand(pins, membership: membership)
		videos = videos.intersection(covered)
		films = films.filter { covered.contains($0.key) }
		coversOf = coversOf.filter { id, _ in pins.contains { $0.id == id } }
		pushCovers()
		let record = Record(
			pins: pins, membership: membership, videos: Array(videos), covers: coversOf,
			films: films)
		guard let data = try? JSONEncoder().encode(record) else { return }
		try? data.write(to: file, options: .atomic)
	}

	/// Tells the image store which covers to keep out of eviction's reach.
	private func pushCovers() {
		let covers = Set(coversOf.values.joined().map(\.encoded))
		Task { await ImageStore.shared.setPinned(covers) }
	}

	private static func covers(of songs: [Song]) -> [ItemRef] {
		Array(Set(songs.compactMap(\.coverArt)))
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
	///
	/// A video is its soundtrack, never the film's own size - which with
	/// "Original" set would refuse a concert for the size of its picture.
	static func estimatedBytes(of song: Song, quality wanted: AudioQuality) -> Int64 {
		let quality = song.isVideo ? wanted.forVideoAudio() : wanted
		guard quality.format != .original else { return Int64(song.sizeBytes) }
		guard song.duration > 0 else { return Int64(song.sizeBytes) }
		return Int64(song.duration) * Int64(quality.bitRate) * 1000 / 8
	}
}

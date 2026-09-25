//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import Foundation

/// One fetch job, on the server that runs it.
struct FetchJob: Identifiable, Equatable, Sendable {
	let server: ServerId
	let serverName: String
	let jobId: String
	let state: FetchState
	let audio: Bool
	let url: String
	let artist: String
	let album: String
	let percent: Int
	let detail: String
	let error: String

	var id: String { "\(server)/\(jobId)" }

	/// What to call it before the server has named anything: the album if one
	/// was typed or parsed, else the URL.
	var title: String { album.isEmpty ? url : album }

	init(server: ServerId, serverName: String, dto: FetchJobDto) {
		self.server = server
		self.serverName = serverName
		jobId = dto.id ?? ""
		state = FetchState(dto.state)
		audio = dto.mode != "video"
		url = dto.url ?? ""
		artist = dto.artist ?? ""
		album = dto.album ?? ""
		percent = dto.percent ?? 0
		detail = dto.detail ?? ""
		error = dto.error ?? ""
	}
}

/// A server that can fetch, and what it can fetch.
struct FetchTarget: Identifiable, Equatable, Sendable {
	let id: ServerId
	let name: String
	let canAudio: Bool
	let canVideo: Bool
}

/// URL fetches across every server: what may be offered, what is running, and
/// telling the library when one lands. Android's `UrlFetchRepository` and
/// `FetchMonitor` in one object, since here both are main-actor state.
///
/// **Polled only while the app is in the foreground.** iOS suspends a
/// backgrounded app, so there is nothing to poll from then anyway; `RootView`
/// starts and stops this on the scene phase, and the first sweep on coming back
/// is what notices a fetch that finished meanwhile - or one begun in another
/// client. Two seconds while something is live, a minute otherwise, the
/// cadence Android and the web client use.
@MainActor
@Observable
final class Fetches {
	private(set) var jobs: [FetchJob] = []
	/// Servers whose job list has stopped answering. A failed poll never
	/// clears what is known - it is usually the server busy indexing what it
	/// just fetched, the moment a fetch most wants reporting on - so this is
	/// what says the figures on screen may be stale.
	private(set) var contactLost: Set<ServerId> = []

	var live: [FetchJob] { jobs.filter(\.state.isLive) }

	/// The one the strip shows: running or being indexed, not merely queued.
	var moving: FetchJob? { jobs.first { $0.state == .running || $0.state == .scanning } }

	@ObservationIgnored private let registry: ServerRegistry
	@ObservationIgnored private let accounts: Accounts
	@ObservationIgnored private let events: LibraryEvents
	@ObservationIgnored private let settings: SettingsStore
	@ObservationIgnored private var handlers: [ServerId: [UrlHandlerDto]] = [:]
	/// The last state seen per job, so a completion is noticed once.
	@ObservationIgnored private var lastState: [String: FetchState] = [:]
	@ObservationIgnored private var failures: [ServerId: Int] = [:]
	@ObservationIgnored private var loop: Task<Void, Never>?
	@ObservationIgnored private var nudged = false

	private static let liveInterval = Duration.seconds(2)
	private static let idleInterval = Duration.seconds(60)
	/// Thirty seconds of silence at the live cadence before saying so.
	private static let maxFailures = 15

	init(
		registry: ServerRegistry, accounts: Accounts, events: LibraryEvents,
		settings: SettingsStore
	) {
		self.registry = registry
		self.accounts = accounts
		self.events = events
		self.settings = settings
	}

	// MARK: - Offering

	/// The servers this account may fetch on, with what each can produce.
	/// A server that says no - error 50 without the upload role, or one too
	/// old to know the endpoint - is simply absent; neither is a fault anyone
	/// can fix from here. Remembered for the session once answered.
	func targets() async -> [FetchTarget] {
		guard !settings.offlineMode else { return [] }
		let clients = registry.clientsSnapshot()
		var found: [FetchTarget] = []
		for config in clients.servers {
			let list: [UrlHandlerDto]
			if let known = handlers[config.id] {
				list = known
			} else {
				guard let client = clients.client(for: config.id) else { continue }
				do {
					list = try await client.urlHandlers()
				} catch is SubsonicError {
					// Answered, and said no: that is an answer worth keeping.
					list = []
				} catch {
					// Did not answer. Not remembered, so it is asked again.
					continue
				}
				handlers[config.id] = list
			}
			guard !list.isEmpty else { continue }
			found.append(
				FetchTarget(
					id: config.id, name: config.name,
					canAudio: list.contains { $0.audio == true },
					canVideo: list.contains { $0.video == true }))
		}
		return found
	}

	/// A server was edited or removed: its verdict and its jobs may belong
	/// to a different account now.
	func forget(_ server: ServerId) {
		handlers[server] = nil
		jobs.removeAll { $0.server == server }
		contactLost.remove(server)
	}

	// MARK: - Acting

	struct Offline: LocalizedError {
		var errorDescription: String? { "You are offline, so nothing can be fetched." }
	}

	/// Queues a fetch and shows it at once, rather than on the next sweep.
	func submit(
		on server: ServerId, url: String, audio: Bool, artist: String, album: String
	) async throws {
		guard !settings.offlineMode else { throw Offline() }
		let clients = registry.clientsSnapshot()
		guard let client = clients.client(for: server) else { return }
		let artist = artist.trimmingCharacters(in: .whitespaces)
		let album = album.trimmingCharacters(in: .whitespaces)
		let dto = try await client.fetchUrl(
			url: url.trimmingCharacters(in: .whitespacesAndNewlines), audio: audio,
			artist: artist.isEmpty ? nil : artist, album: album.isEmpty ? nil : album)
		if let dto {
			let job = FetchJob(
				server: server, serverName: clients.config(for: server)?.name ?? "", dto: dto)
			// Seeded, so its first sighting in a sweep is not a transition.
			lastState[job.id] = job.state
			jobs = [job] + jobs.filter { $0.id != job.id }
		}
		nudge()
	}

	func cancel(_ job: FetchJob) async {
		guard let client = registry.clientsSnapshot().client(for: job.server) else { return }
		try? await client.cancelFetch(id: job.jobId)
		nudge()
	}

	// MARK: - Polling

	func start() {
		guard loop == nil else { return }
		loop = Task { [weak self] in
			while !Task.isCancelled {
				guard let self else { return }
				await self.sweep()
				let interval = self.live.isEmpty ? Self.idleInterval : Self.liveInterval
				await self.sleep(for: interval)
			}
		}
	}

	func stop() {
		loop?.cancel()
		loop = nil
	}

	/// Cuts the current wait short, so a job just submitted or cancelled is
	/// read within a moment rather than at the end of an idle minute.
	func nudge() {
		nudged = true
	}

	private func sleep(for interval: Duration) async {
		let deadline = ContinuousClock.now.advanced(by: interval)
		while ContinuousClock.now < deadline, !Task.isCancelled {
			if nudged {
				nudged = false
				return
			}
			try? await Task.sleep(for: .milliseconds(250))
		}
	}

	private func sweep() async {
		guard !settings.offlineMode else { return }
		let clients = registry.clientsSnapshot()
		var landed = false
		// Only servers holding something live get the fast cadence; with
		// nothing live every uploading server is swept, which is what notices
		// a fetch someone began elsewhere.
		let busy = Set(live.map(\.server))
		var candidates = clients.servers.filter { busy.contains($0.id) }
		if candidates.isEmpty { candidates = clients.servers }
		for config in candidates {
			// The endpoint's own precondition, so a server without it would
			// cost one guaranteed error per tick.
			guard await accounts.facts(for: config.id, using: clients).canUpload,
				let client = clients.client(for: config.id)
			else { continue }
			guard let dtos = try? await client.fetchJobs() else {
				let count = (failures[config.id] ?? 0) + 1
				failures[config.id] = count
				if count >= Self.maxFailures { contactLost.insert(config.id) }
				continue
			}
			failures[config.id] = 0
			contactLost.remove(config.id)
			let fresh = dtos.map { FetchJob(server: config.id, serverName: config.name, dto: $0) }
			if Self.completed(since: &lastState, in: fresh) { landed = true }
			jobs = fresh + jobs.filter { $0.server != config.id }
		}
		// Once per sweep, not per job: two fetches landing together are one
		// re-read. The server has already indexed before it says done - that
		// is what `scanning` is - so the library is correct by now.
		if landed { events.libraryChanged() }
	}

	/// Whether any job moved to `done` since it was last seen. A job seen for
	/// the first time is not a transition, or every finished job the server
	/// still lists would count on launch.
	nonisolated static func completed(since previous: inout [String: FetchState], in jobs: [FetchJob]) -> Bool {
		var any = false
		for job in jobs {
			let was = previous.updateValue(job.state, forKey: job.id)
			if let was, was != job.state, job.state == .done { any = true }
		}
		return any
	}
}

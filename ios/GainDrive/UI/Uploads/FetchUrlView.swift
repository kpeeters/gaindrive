//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Have the server fetch a URL into your uploads, and watch it happen.
///
/// Android's fetch panel as a sheet from the uploads screen and from the fetch
/// strip. The URL is pasted: there is no Share Extension, which would need a
/// second target sharing the server list and credentials.
struct FetchUrlView: View {
	@Environment(Fetches.self) private var fetches
	@Environment(SettingsStore.self) private var settings
	@Environment(\.library) private var library
	@Environment(\.dismiss) private var dismiss
	@State private var model: FetchUrlModel?

	var body: some View {
		NavigationStack {
			Group {
				if let model {
					form(model)
				} else {
					ProgressView()
				}
			}
			.navigationTitle("Fetch from a URL")
			.inlineTitle()
			.toolbar {
				ToolbarItem(placement: .confirmationAction) {
					Button("Done") { dismiss() }
				}
			}
		}
		.task {
			if model == nil, let library {
				model = FetchUrlModel(fetches: fetches, library: library, settings: settings)
			}
			await model?.appear()
		}
	}

	@ViewBuilder
	private func form(_ model: FetchUrlModel) -> some View {
		@Bindable var model = model
		Form {
			if let targets = model.targets, targets.isEmpty {
				Section {
					Text(
						"""
						No server here can fetch for this account. It needs \
						the upload permission, and the server needs its URL \
						handlers configured.
						""")
					.foregroundStyle(.secondary)
				}
			} else {
				Section {
					TextField("https://…", text: $model.url)
						.urlEntry()
						.autocorrectionDisabled()
					PasteButton(payloadType: String.self) { strings in
						if let first = strings.first { model.url = Self.firstLink(in: first) }
					}
					// Drawn only when there is a choice to make.
					if let targets = model.targets, targets.count > 1 {
						Picker("Server", selection: $model.server) {
							ForEach(targets) { Text($0.name).tag(Optional($0.id)) }
						}
					}
					if model.showsModes {
						Picker("Keep", selection: $model.audio) {
							Text("Audio").tag(true)
							Text("Video").tag(false)
						}
						.pickerStyle(.segmented)
					}
				}

				Section {
					TextField("Artist", text: $model.artist)
					TextField("Album", text: $model.album)
					if !model.artist.isEmpty || !model.album.isEmpty {
						Button("Clear names") { model.clearNames() }
					}
				} footer: {
					VStack(alignment: .leading, spacing: 6) {
						Text("Leave empty to use what the page calls it. Kept for the next fetch.")
						if let existing = model.existing {
							Label(existing, systemImage: "info.circle")
						}
					}
				}

				Section {
					Button {
						model.submit()
					} label: {
						if model.submitting {
							ProgressView()
						} else {
							Text("Fetch")
						}
					}
					.disabled(!model.canSubmit)
				} footer: {
					if let duplicate = model.duplicate {
						Text(
							duplicate.state.isLive
								? "That URL is already being fetched."
								: "That URL was fetched already: \(duplicate.state.label.lowercased()).")
					}
				}

				if !model.jobs.isEmpty {
					Section {
						ForEach(model.jobs) { job in
							FetchJobRow(job: job)
								.swipeActions {
									if job.state.isCancellable {
										Button("Cancel", role: .destructive) { model.cancel(job) }
									}
								}
						}
					} header: {
						Text("Recent")
					} footer: {
						if model.contactLost {
							Text("The server has stopped answering; this may be out of date.")
						}
					}
				}
			}
		}
		.alert(
			"Nothing was fetched",
			isPresented: Binding(get: { model.error != nil }, set: { if !$0 { model.error = nil } })
		) {
			Button("OK") { model.error = nil }
		} message: {
			Text(model.error ?? "")
		}
	}

	/// The first http(s) link in pasted text, or the text itself. A shared
	/// YouTube item is a title and then the link on the next line, which is
	/// why Android's share intake searches rather than requiring one.
	nonisolated static func firstLink(in text: String) -> String {
		let detector = try? NSDataDetector(types: NSTextCheckingResult.CheckingType.link.rawValue)
		let range = NSRange(text.startIndex..., in: text)
		let links = detector?.matches(in: text, range: range).compactMap(\.url) ?? []
		let web = links.first { $0.scheme == "http" || $0.scheme == "https" }
		return web?.absoluteString ?? text.trimmingCharacters(in: .whitespacesAndNewlines)
	}
}

/// One job: what it is, where it has got to, and why it stopped if it did.
struct FetchJobRow: View {
	let job: FetchJob

	var body: some View {
		VStack(alignment: .leading, spacing: 4) {
			Text(job.title).lineLimit(1)
			HStack(spacing: 8) {
				Text(job.state.label)
					.foregroundStyle(job.state == .error ? Color.red : Color.secondary)
				if !job.audio { Image(systemName: "film").foregroundStyle(.secondary) }
			}
			.font(.footnote)
			if job.state == .running {
				// Determinate once the tool reports anything; the server keeps
				// the last figure rather than resetting, so this never snaps
				// back.
				ProgressView(value: Double(job.percent), total: 100)
			} else if job.state.isLive {
				ProgressView().progressViewStyle(.linear)
			}
			let note = job.state == .error ? job.error : job.detail
			if !note.isEmpty {
				Text(note)
					.font(.caption)
					.foregroundStyle(.secondary)
					.lineLimit(2)
			}
		}
	}
}

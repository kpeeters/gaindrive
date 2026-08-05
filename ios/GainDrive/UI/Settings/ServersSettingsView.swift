//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// The configured servers, and the first-run flow.
///
/// Adding a server *is* the login flow: there is no separate login screen,
/// because with several servers there is no single "logged in" state to gate
/// the app on, and a login screen that is really "add your first server" would
/// have to be a different screen from the one used to add the second.
struct ServersSettingsView: View {
	/// True only on a fresh install, and consumed once: the add form opens
	/// straight away rather than making the user find a button on an empty
	/// screen.
	let startAdding: Bool

	@Environment(ServerRegistry.self) private var registry
	@State private var editing: ServerEditView.Target?
	@State private var confirmingRemoval: ServerConfig?
	@State private var statuses: [ServerId: ConnectionTest] = [:]

	init(startAdding: Bool = false) {
		self.startAdding = startAdding
		_editing = State(initialValue: startAdding ? .new : nil)
	}

	var body: some View {
		List {
			ForEach(registry.servers) { config in
				Button {
					editing = .existing(config)
				} label: {
					ServerRow(config: config, status: statuses[config.id])
				}
				.buttonStyle(.plain)
				.swipeActions(edge: .trailing) {
					Button(role: .destructive) {
						confirmingRemoval = config
					} label: {
						Label("Remove", systemImage: "trash")
					}
				}
				.swipeActions(edge: .leading) {
					Button {
						registry.setEnabled(!config.isEnabled, for: config.id)
					} label: {
						Label(
							config.isEnabled ? "Disable" : "Enable",
							systemImage: config.isEnabled ? "pause.circle" : "play.circle")
					}
					.tint(config.isEnabled ? .orange : .green)
				}
			}
			.onMove { registry.move(fromOffsets: $0, toOffset: $1) }
			.onDelete { offsets in
				// Deliberately no confirmation on this path: an edit-mode
				// delete is already a two-step gesture, and the swipe action
				// above is the one that needs the guard.
				registry.remove(atOffsets: offsets)
			}

			if registry.servers.count > 1 {
				Section {
				} footer: {
					Text("Order decides which server wins when the same album is on more than one.")
				}
			}
		}
		.navigationTitle("Servers")
		.toolbar {
			ToolbarItem(placement: .topBarTrailing) {
				Button {
					editing = .new
				} label: {
					Label("Add server", systemImage: "plus")
				}
			}
			ToolbarItem(placement: .topBarLeading) {
				if registry.servers.count > 1 { EditButton() }
			}
		}
		.overlay {
			if registry.servers.isEmpty {
				ContentUnavailableView {
					Image("Logo")
						.resizable()
						.scaledToFit()
						.frame(width: 88, height: 88)
						.clipShape(RoundedRectangle(cornerRadius: 20, style: .continuous))
					Text("No servers yet")
				} description: {
					Text("Add the address of your GainDrive server to get started.")
				} actions: {
					Button("Add server") { editing = .new }
						.buttonStyle(.borderedProminent)
				}
			}
		}
		.sheet(item: $editing) { target in
			ServerEditView(target: target)
		}
		.confirmationDialog(
			"Remove \(confirmingRemoval?.displayName ?? "this server")?",
			isPresented: Binding(
				get: { confirmingRemoval != nil },
				set: { if !$0 { confirmingRemoval = nil } }
			),
			titleVisibility: .visible
		) {
			Button("Remove", role: .destructive) {
				if let config = confirmingRemoval { registry.remove(id: config.id) }
				confirmingRemoval = nil
			}
			Button("Cancel", role: .cancel) { confirmingRemoval = nil }
		} message: {
			// Worth saying, because "remove" next to a music library reads as
			// something more alarming than it is.
			Text("This removes the server from this app only. Nothing on the server changes.")
		}
		.task(id: registry.servers) {
			await refreshStatuses()
		}
	}

	/// Every enabled server is checked at once rather than in turn: one that is
	/// switched off takes the full request timeout to fail, and in sequence
	/// that delay would be paid by every server after it in the list.
	private func refreshStatuses() async {
		var targets: [(id: ServerId, url: URL, username: String, password: String)] = []
		for config in registry.servers where config.isEnabled {
			guard let url = config.baseURL else {
				statuses[config.id] = .unreachable("The address is not usable.")
				continue
			}
			guard let password = registry.password(for: config.id) else {
				// The Keychain item is gone but the configuration is not —
				// what a restore onto a new device looks like if the password
				// did not travel with it.
				statuses[config.id] = .unreachable("No saved password. Open the server to enter it again.")
				continue
			}
			targets.append((config.id, url, config.username, password))
		}

		let results = await withTaskGroup(of: (ServerId, ConnectionTest).self) { group in
			for target in targets {
				group.addTask {
					(
						target.id,
						await ConnectionTester.test(
							url: target.url,
							username: target.username,
							password: target.password
						)
					)
				}
			}
			var collected: [(ServerId, ConnectionTest)] = []
			for await result in group { collected.append(result) }
			return collected
		}

		for (id, outcome) in results {
			statuses[id] = outcome
		}
	}
}

private struct ServerRow: View {
	let config: ServerConfig
	let status: ConnectionTest?

	var body: some View {
		HStack(spacing: 12) {
			Circle()
				.fill(dotColour)
				.frame(width: 10, height: 10)
				.accessibilityLabel(dotDescription)
			VStack(alignment: .leading, spacing: 2) {
				Text(config.displayName)
					.foregroundStyle(config.isEnabled ? .primary : .secondary)
				Text("\(config.username) at \(config.displayAddress)")
					.font(.footnote)
					.foregroundStyle(.secondary)
			}
			Spacer()
			Image(systemName: "chevron.right")
				.font(.footnote.weight(.semibold))
				.foregroundStyle(.tertiary)
		}
		.contentShape(.rect)
	}

	private var dotColour: Color {
		guard config.isEnabled else { return .secondary }
		guard let status else { return .secondary.opacity(0.4) }
		return status.isSuccess ? .green : .red
	}

	private var dotDescription: String {
		guard config.isEnabled else { return "Disabled" }
		guard let status else { return "Checking" }
		switch status {
		case .reachable, .unverified: return "Reachable"
		case .rejected: return "Wrong credentials"
		case .unreachable: return "Unreachable"
		}
	}
}

#Preview {
	NavigationStack {
		ServersSettingsView()
			.environment(ServerRegistry())
	}
}

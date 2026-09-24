//	GainDrive for iOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

import SwiftUI

/// Add or edit one server. Adding the second server is the same screen as
/// adding the first.
struct ServerEditView: View {
	enum Target: Identifiable, Hashable {
		case new
		case existing(ServerConfig)

		var id: String {
			switch self {
			case .new: "new"
			case .existing(let config): config.id.description
			}
		}
	}

	let target: Target

	@Environment(ServerRegistry.self) private var registry
	@Environment(\.dismiss) private var dismiss

	@State private var name: String
	@State private var urlText: String
	@State private var username: String
	@State private var password: String
	@State private var isTesting = false
	@State private var testResult: ConnectionTest?
	@State private var saveError: String?

	init(target: Target) {
		self.target = target
		switch target {
		case .new:
			_name = State(initialValue: "")
			_urlText = State(initialValue: "")
			_username = State(initialValue: "")
			_password = State(initialValue: "")
		case .existing(let config):
			_name = State(initialValue: config.name)
			_urlText = State(initialValue: config.urlString)
			_username = State(initialValue: config.username)
			// Prefilled from the Keychain so "Test connection" works on an
			// existing server without retyping the password - which is the
			// main reason anyone opens this screen a second time.
			_password = State(initialValue: Keychain.password(for: config.id) ?? "")
		}
	}

	var body: some View {
		NavigationStack {
			Form {
				Section {
					TextField("Server address", text: $urlText)
						.keyboardType(.URL)
						.textContentType(.URL)
						.textInputAutocapitalization(.never)
						.autocorrectionDisabled()
				} header: {
					Text("Address")
				} footer: {
					// Shows what will actually be contacted, since the field
					// accepts a bare host and quietly gains a scheme.
					if !normalisedURL.isEmpty, normalisedURL != urlText {
						Text("Connects to \(normalisedURL)")
					} else {
						Text("For example, 192.0.2.9:4040")
					}
				}

				Section("Account") {
					TextField("Username", text: $username)
						.textContentType(.username)
						.textInputAutocapitalization(.never)
						.autocorrectionDisabled()
					SecureField("Password", text: $password)
						.textContentType(.password)
				}

				Section {
					TextField("Display name", text: $name, prompt: Text(namePlaceholder))
						.textInputAutocapitalization(.words)
				} footer: {
					Text("Optional. Defaults to the server's host name.")
				}

				Section {
					Button {
						Task { await runTest() }
					} label: {
						HStack {
							Text("Test connection")
							Spacer()
							if isTesting { ProgressView() }
						}
					}
					.disabled(!isComplete || isTesting)

					if let testResult {
						TestResultRow(outcome: testResult)
					}
				} footer: {
					// Saving without testing is allowed; the row in the list
					// simply shows the unreachable dot until something
					// succeeds.
					Text("Testing is optional.")
				}
			}
			.navigationTitle(isNew ? "Add Server" : "Edit Server")
			.navigationBarTitleDisplayMode(.inline)
			.toolbar {
				ToolbarItem(placement: .cancellationAction) {
					Button("Cancel") { dismiss() }
				}
				ToolbarItem(placement: .confirmationAction) {
					Button("Save") { save() }
						.disabled(!isComplete)
				}
			}
			.alert(
				"Could not save the password",
				isPresented: Binding(
					get: { saveError != nil },
					set: { if !$0 { saveError = nil } })
			) {
				Button("OK") { saveError = nil }
			} message: {
				Text(saveError ?? "")
			}
		}
	}

	private var isNew: Bool {
		if case .new = target { return true }
		return false
	}

	private var normalisedURL: String {
		ServerConfig.normalisedURL(urlText)
	}

	private var namePlaceholder: String {
		URL(string: normalisedURL)?.host() ?? "My music server"
	}

	/// The password is *not* required to save. A server whose account has no
	/// password is unusual but legal, and refusing to save one would be the
	/// app inventing a rule the server does not have.
	private var isComplete: Bool {
		ServerConfig.isUsable(urlString: normalisedURL)
			&& !username.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty
	}

	private func save() {
		let id: ServerId
		switch target {
		case .new:
			id = registry.add(
				name: name, urlString: urlText, username: username, password: password
			).id
		case .existing(let config):
			var updated = config
			updated.name = ServerConfig.normalisedName(name)
			updated.urlString = normalisedURL
			updated.username = ServerConfig.normalisedUsername(username)
			registry.update(updated, newPassword: password)
			id = config.id
		}

		// Read the password back rather than trusting that writing it worked.
		//
		// The Keychain can refuse a write for reasons that have nothing to do
		// with this screen - a signing or entitlement problem being the usual
		// one - and the symptom appears much later and somewhere else: the
		// server list saying it has no saved password, or every browse screen
		// failing to build a client. Catching it here names the right thing at
		// the moment it happened, and is robust to whatever the cause turns out
		// to be. See the log line in `Keychain` for the `OSStatus` itself.
		let trimmed = password.trimmingCharacters(in: .whitespacesAndNewlines)
		guard trimmed.isEmpty || registry.password(for: id) != nil else {
			saveError = """
				The server was saved but its password could not be stored in the \
				keychain. Playback and browsing will fail until it can be.
				"""
			return
		}
		dismiss()
	}

	private func runTest() async {
		guard let baseURL = URL(string: normalisedURL) else {
			testResult = .unreachable("The address is not usable.")
			return
		}
		isTesting = true
		testResult = nil
		testResult = await ConnectionTester.test(
			url: baseURL, username: username, password: password)
		isTesting = false
	}
}

private struct TestResultRow: View {
	let outcome: ConnectionTest

	var body: some View {
		Label {
			VStack(alignment: .leading, spacing: 2) {
				Text(title)
				if let detail {
					Text(detail)
						.font(.footnote)
						.foregroundStyle(.secondary)
				}
			}
		} icon: {
			Image(systemName: outcome.isSuccess ? "checkmark.circle.fill" : "xmark.circle.fill")
				.foregroundStyle(outcome.isSuccess ? .green : .red)
		}
	}

	private var title: String {
		switch outcome {
		case .reachable(let user): "Connected as \(user.username)"
		case .unverified: "Connected"
		case .rejected: "Wrong username or password"
		case .unreachable: "Could not connect"
		}
	}

	private var detail: String? {
		switch outcome {
		case .reachable(let user):
			// Roles are per server, so this is worth saying here rather than
			// anywhere global: the same person can be an admin on one server
			// and a restricted account on another.
			var roles: [String] = []
			if user.isAdmin { roles.append("admin") }
			if user.canUpload { roles.append("upload") }
			if let limit = user.maxBitRate, limit > 0 { roles.append("max \(limit) kbps") }
			return roles.isEmpty ? nil : roles.joined(separator: ", ")
		case .unverified:
			return "The server did not report account details."
		case .rejected(let message):
			// The server's own wording, when it sent one that says more than
			// our title already does - a self-hoster fixing something needs
			// the server's sentence, not ours.
			return message == title ? nil : message
		case .unreachable(let message):
			return message
		}
	}
}

#Preview {
	ServerEditView(target: .new)
		.environment(ServerRegistry())
}

//	GainDrive for macOS
//	Copyright (C) 2026 Kasper Peeters
//
//	This file is part of GainDrive, distributed under the GNU General
//	Public License version 3 or later, with the Application Distribution
//	Exception in ios/LICENSE. See LICENSE at the repository root for the
//	full text of the GPL.

#if os(macOS)

import SwiftUI

/// `PaneNavigator` on the Mac: the same path, the same 900/650-point pane
/// count and the same selection-driven levels, but no navigation stacks and no
/// bars. A Mac window has one toolbar, which holds what belongs to the whole
/// window; each pane instead gets a slim header row with its title and its own
/// actions (`.paneHeader`), and full-height dividers between them - Finder's
/// column view and Xcode's editors, rather than an iPad's stacked bars.
///
/// On a narrow window, one pane shows the deepest level, and its header gets a
/// back arrow; with two or three the level above is on screen and an arrow
/// would be noise.
struct MacPaneRow<R: Hashable, Root: View, Destination: View, Placeholder: View>: View {
	@Binding var path: [R]
	let maxLevels: Int
	let root: () -> Root
	let destination: (R) -> Destination
	let placeholder: (Int) -> Placeholder

	@Environment(\.paneHeaderLeadingInset) private var leadingInset

	var body: some View {
		GeometryReader { geometry in
			row(min(PaneMath.count(width: geometry.size.width), maxLevels))
		}
	}

	/// Keyed by what each pane shows, as on iOS, so a level inserted in front
	/// moves a pane along without rebuilding it.
	private func row(_ panes: Int) -> some View {
		let first = PaneMath.firstLevel(levels: path.count + 1, panes: panes)
		let shown = (first..<(first + panes)).map { Shown(level: $0, key: identity($0)) }
		return HStack(spacing: 0) {
			ForEach(shown, id: \.key) { pane in
				if pane.level > first { Divider() }
				column(pane.level, leading: pane.level == first)
					.environment(\.paneCount, panes)
					.transition(.move(edge: .trailing).combined(with: .opacity))
			}
		}
		// Opening a level slides the panes along and closing one slides them
		// back, as the per-pane stacks of the iPad layout do.
		.animation(.default, value: path)
	}

	private func column(_ level: Int, leading: Bool) -> some View {
		content(level)
			.frame(maxWidth: .infinity, maxHeight: .infinity)
			.paneHeaderHost(
				onBack: leading && level > 0 ? { path.removeLast() } : nil,
				leadingInset: leading ? leadingInset : 0)
			.clipped()
	}

	@ViewBuilder
	private func content(_ level: Int) -> some View {
		if level == 0 {
			root()
		} else if level <= path.count {
			destination(path[level - 1])
		} else {
			placeholder(level)
		}
	}

	private struct Shown {
		let level: Int
		let key: AnyHashable
	}

	private func identity(_ level: Int) -> AnyHashable {
		if level == 0 { return AnyHashable("root") }
		if level <= path.count { return AnyHashable(path[level - 1]) }
		return AnyHashable("empty-\(level)")
	}
}

extension View {
	/// Draws the header this view's screen declared with `.paneHeader`. Its
	/// room is reserved as a top inset, so a list scrolls beneath it rather
	/// than starting under it; the header is then drawn over that room.
	func paneHeaderHost(onBack: (() -> Void)?, leadingInset: CGFloat) -> some View {
		safeAreaInset(edge: .top, spacing: 0) {
			Color.clear.frame(height: PaneHeaderBar.height)
		}
		.overlayPreferenceValue(PaneHeaderKey.self, alignment: .top) { value in
			PaneHeaderBar(value: value, onBack: onBack, leadingInset: leadingInset)
		}
	}
}

/// One pane's header: an optional back arrow, the title, and the pane's own
/// actions, on the bar material with a hairline beneath.
struct PaneHeaderBar: View {
	static let height: CGFloat = 36

	let value: PaneHeaderValue?
	let onBack: (() -> Void)?
	let leadingInset: CGFloat

	var body: some View {
		HStack(spacing: 8) {
			if let onBack {
				Button(action: onBack) {
					Image(systemName: "chevron.backward")
				}
				.buttonStyle(.borderless)
				.accessibilityLabel("Back")
			}
			if let leading = value?.leading {
				HStack(spacing: 8) { leading }
					.buttonStyle(.borderless)
			}
			Text(value?.title ?? "")
				.font(.headline)
				.lineLimit(1)
			Spacer(minLength: 8)
			if let actions = value?.actions {
				// Icons only, in the app's red, as bar buttons are on iOS; a
				// header has no room for words beside its title.
				HStack(spacing: 12) { actions }
					.labelStyle(.iconOnly)
					.buttonStyle(HeaderButtonStyle())
					.menuStyle(.button)
					.menuIndicator(.hidden)
					.tint(Color.accentColor)
			}
		}
		.padding(.horizontal, 12)
		.padding(.leading, leadingInset)
		.frame(height: Self.height)
		.frame(maxWidth: .infinity)
		.background(.bar)
		// The headers are where the title bar was, so they are what the window
		// is moved by; the buttons in them still take their own clicks.
		.contentShape(.rect)
		.gesture(WindowDragGesture())
		.allowsWindowActivationEvents(true)
		.overlay(alignment: .bottom) { Divider() }
	}
}

/// A header button: the label alone, in the accent colour, dimmed while
/// pressed.
private struct HeaderButtonStyle: ButtonStyle {
	func makeBody(configuration: Configuration) -> some View {
		configuration.label
			.foregroundStyle(Color.accentColor)
			.opacity(configuration.isPressed ? 0.5 : 1)
			.contentShape(.rect)
	}
}

extension EnvironmentValues {
	/// Room kept at the leading end of the first pane's header for the window
	/// controls, which lie over it when the sidebar is collapsed.
	@Entry var paneHeaderLeadingInset: CGFloat = 0
}

#endif

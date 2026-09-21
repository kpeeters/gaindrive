package org.gaindrive.android.ui.player

import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.hilt.lifecycle.viewmodel.compose.hiltViewModel
import androidx.lifecycle.compose.collectAsStateWithLifecycle

/**
 * The transport row wired to the player. It resolves its own [PlayerViewModel]
 * (the activity-scoped instance the Now Playing sheet already drives), so a
 * host sheet embeds it with no plumbing; the sound-control sheets all do,
 * between their controls and the volume row.
 */
@Composable
internal fun TransportSection(viewModel: PlayerViewModel = hiltViewModel()) {
	val state by viewModel.state.collectAsStateWithLifecycle()
	TransportRow(
		playing = state.isPlaying,
		hasPrevious = state.hasPrevious,
		hasNext = state.hasNext,
		onPrevious = viewModel::previous,
		onTogglePlay = viewModel::togglePlayPause,
		onNext = viewModel::next,
	)
}

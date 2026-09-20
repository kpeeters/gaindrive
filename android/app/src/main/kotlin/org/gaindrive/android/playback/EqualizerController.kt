package org.gaindrive.android.playback

import android.media.audiofx.Equalizer
import android.util.Log
import androidx.media3.common.C
import androidx.media3.exoplayer.ExoPlayer
import androidx.media3.exoplayer.analytics.AnalyticsListener
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.launch
import org.gaindrive.android.data.EqStore
import javax.inject.Inject
import javax.inject.Singleton

/** One native band: where it sits and where its fader is. */
data class EqBand(val centerHz: Int, val levelMb: Int)

sealed interface EqState {
	/** No local player registered, or the effect has not been probed yet. */
	data object NoPlayer : EqState

	/** The device refused to create the effect; emulators routinely do. */
	data class Unavailable(val message: String) : EqState

	data class Ready(
		val enabled: Boolean,
		val minMb: Int,
		val maxMb: Int,
		val bands: List<EqBand>,
		/** The device's own preset names, in audiofx index order. */
		val presets: List<String>,
		/** The user's saved presets for this device, in name order. */
		val saved: List<String>,
		/** The preset the curve came from; null means hand-shaped. */
		val preset: String?,
	) : EqState
}

/**
 * Owns the [Equalizer] effect on the local player's audio session.
 *
 * Attached to the [ExoPlayer] directly, like [VideoSurface] and for the same
 * reason: an audio session id is a property of the concrete player, and the
 * session boundary has no notion of it. Only the local player is a target at
 * all; the cast player has no audio session, and while casting nothing this
 * effect could shape is audible.
 *
 * The effect is created lazily, on the web client's principle: a user who
 * never switches the equalizer on never gets an effect inserted into their
 * audio path. It does come up at service start when the stored flag says on,
 * so an enabled equalizer works with the sheet never opened.
 *
 * The band layout is whatever the device reports, not the web client's fixed
 * ten: equalizer settings are per output device, and sinks do not share
 * curves, only the panel concept.
 */
@Singleton
class EqualizerController @Inject constructor(
	private val scope: CoroutineScope,
	private val store: EqStore,
) {

	private var player: ExoPlayer? = null
	private var eq: Equalizer? = null

	/** What [EqState.Ready.preset] claims; the effect cannot be asked, since
	 * `getCurrentPreset` keeps naming a preset after the faders moved. */
	private var presetName: String? = null

	/** The saved presets, mirrored from the store so [publish] can stay
	 * synchronous. The collect keeps an open sheet following outside writes;
	 * mutations here update it optimistically so their publish is not a step
	 * behind the write it just made. */
	private var slots: Map<String, List<Int>> = emptyMap()

	private val _state = MutableStateFlow<EqState>(EqState.NoPlayer)
	val state: StateFlow<EqState> = _state.asStateFlow()

	init {
		scope.launch {
			store.slots().collect { fresh ->
				slots = fresh
				eq?.let { publish(it) }
			}
		}
	}

	private val listener = object : AnalyticsListener {
		override fun onAudioSessionIdChanged(
			eventTime: AnalyticsListener.EventTime,
			audioSessionId: Int,
		) {
			// The old effect is bound to the old session and shapes nothing
			// now. Recreated from the live state rather than the store, which
			// can lag behind a drag still in progress.
			val effect = eq ?: return
			val live = _state.value as? EqState.Ready
			effect.release()
			eq = null
			ensureEffect(carryOver = live)
		}
	}

	/** Called by [PlaybackService] with its player, and with null on destroy. */
	fun registerPlayer(player: ExoPlayer?) {
		this.player?.removeAnalyticsListener(listener)
		releaseEffect()
		this.player = player
		player?.addAnalyticsListener(listener)
		if (player == null) return
		scope.launch {
			if (store.enabled().first()) ensureEffect()
		}
	}

	/** The sheet's entry point: probes the effect even while disabled, since
	 * showing the band layout needs one to exist. */
	fun open() {
		ensureEffect()
	}

	fun setEnabled(enabled: Boolean) {
		val effect = eq ?: return
		val status = effect.setEnabled(enabled)
		if (status != Equalizer.SUCCESS) {
			Log.w(TAG, "setEnabled($enabled) returned $status")
		}
		publish(effect)
		scope.launch { store.setEnabled(enabled) }
	}

	/**
	 * Moves one fader. Published but deliberately not persisted: this fires
	 * on every drag tick, and the write belongs in [commitLevels], off the
	 * finger-tracking path.
	 */
	fun setBandLevel(band: Int, levelMb: Int) {
		val effect = eq ?: return
		effect.setBandLevel(band.toShort(), levelMb.toShort())
		// A moved fader makes the curve hand-shaped, whatever it started as.
		presetName = null
		publish(effect)
	}

	/** Persists the curve; the slider's onValueChangeFinished calls this. */
	fun commitLevels() {
		val effect = eq ?: return
		// Read off the effect before suspending: it can be released while the
		// write is in flight, and a released effect only throws.
		val levels = levels(effect)
		val preset = presetName
		scope.launch { store.setCurve(levels, preset) }
	}

	fun usePreset(index: Int) {
		val effect = eq ?: return
		if (index !in 0 until effect.numberOfPresets.toInt()) return
		effect.usePreset(index.toShort())
		presetName = effect.getPresetName(index.toShort())
		publish(effect)
		// The resulting levels are stored beside the name: restoring by
		// levels is what stays correct across a reboot and across a changed
		// band layout, where the name alone would be a guess.
		val levels = levels(effect)
		val preset = presetName
		scope.launch { store.setCurve(levels, preset) }
	}

	fun useSaved(name: String) {
		val effect = eq ?: return
		val stored = slots[name] ?: return
		if (!applyLevels(effect, stored)) return
		presetName = name
		publish(effect)
		// What the effect actually holds, not what was asked: the values were
		// coerced into the band range on the way in.
		val levels = levels(effect)
		scope.launch { store.setCurve(levels, name) }
	}

	/**
	 * Saves the current curve under [name] and selects it, returning a
	 * refusal message or null. A name already saved is overwritten, as on
	 * the web; a device preset's name is refused, since the dropdown would
	 * then carry it twice.
	 */
	fun saveSlot(name: String): String? {
		val effect = eq ?: return "The equalizer is not running."
		val trimmed = name.trim()
		if (trimmed.isEmpty()) return "A preset needs a name."
		val device = (0 until effect.numberOfPresets.toInt()).map {
			effect.getPresetName(it.toShort())
		}
		if (trimmed in device) return "“$trimmed” is already a built-in preset."
		val levels = levels(effect)
		slots = slots + (trimmed to levels)
		presetName = trimmed
		publish(effect)
		scope.launch {
			store.saveSlot(trimmed, levels)
			store.setCurve(levels, trimmed)
		}
		return null
	}

	/** Deletes the selected saved preset. The curve stays where it is and
	 * the selection drops to custom, as on the web. */
	fun deleteSlot() {
		val effect = eq ?: return
		val name = presetName?.takeIf { it in slots } ?: return
		slots = slots - name
		presetName = null
		publish(effect)
		val levels = levels(effect)
		scope.launch {
			store.deleteSlot(name)
			store.setCurve(levels, null)
		}
	}

	/**
	 * Creates the effect on the current audio session if there is none yet,
	 * applying [carryOver] when the effect is being rebuilt on a new session
	 * and the stored settings otherwise.
	 */
	private fun ensureEffect(carryOver: EqState.Ready? = null) {
		if (eq != null) return
		val player = player ?: run {
			_state.value = EqState.NoPlayer
			return
		}
		val sessionId = player.audioSessionId
		if (sessionId == C.AUDIO_SESSION_ID_UNSET) {
			// Media3 generates an id at player construction, so this is not
			// expected; if it happens the listener brings us back.
			Log.w(TAG, "no audio session id yet; waiting for the player")
			_state.value = EqState.NoPlayer
			return
		}
		val effect = try {
			Equalizer(/* priority = */ 0, sessionId)
		} catch (e: RuntimeException) {
			// Emulators and some OEM builds simply have no implementation.
			Log.e(TAG, "cannot create an equalizer on session $sessionId", e)
			_state.value = EqState.Unavailable("The equalizer is not available on this device.")
			return
		}
		eq = effect
		if (carryOver != null) {
			presetName = restore(
				effect,
				enabled = carryOver.enabled,
				levelsMb = carryOver.bands.map { it.levelMb },
				preset = carryOver.preset,
			)
			publish(effect)
		} else {
			scope.launch {
				val enabled = store.enabled().first()
				val levelsMb = store.levelsMb().first()
				val preset = store.preset().first()
				// The reads suspend, and the effect can be gone by the time
				// they return; a released one only throws.
				if (eq !== effect) return@launch
				presetName = restore(effect, enabled, levelsMb, preset)
				publish(effect)
			}
		}
	}

	/** Puts a stored or carried-over state onto the effect, returning the
	 * preset name that survived doing so. */
	private fun restore(
		effect: Equalizer,
		enabled: Boolean,
		levelsMb: List<Int>?,
		preset: String?,
	): String? {
		var name = preset
		if (levelsMb != null && !applyLevels(effect, levelsMb)) {
			// An OS update can change the band layout, and a stale curve on
			// the wrong bands is worse than starting over flat.
			name = null
		}
		val status = effect.setEnabled(enabled)
		if (status != Equalizer.SUCCESS) {
			Log.w(TAG, "setEnabled($enabled) returned $status")
		}
		return name
	}

	/** Puts a curve onto the effect, coerced into the band range; false and
	 * a log line when its band count is not the device's. */
	private fun applyLevels(effect: Equalizer, levelsMb: List<Int>): Boolean {
		val count = effect.numberOfBands.toInt()
		if (levelsMb.size != count) {
			Log.w(TAG, "curve has ${levelsMb.size} bands but the device has $count; leaving the faders alone")
			return false
		}
		val range = effect.bandLevelRange
		levelsMb.forEachIndexed { band, mb ->
			effect.setBandLevel(
				band.toShort(),
				mb.coerceIn(range[0].toInt(), range[1].toInt()).toShort(),
			)
		}
		return true
	}

	private fun publish(effect: Equalizer) {
		val range = effect.bandLevelRange
		val presets = (0 until effect.numberOfPresets.toInt()).map {
			effect.getPresetName(it.toShort())
		}
		val saved = slots.keys.sorted()
		_state.value = EqState.Ready(
			enabled = effect.enabled,
			minMb = range[0].toInt(),
			maxMb = range[1].toInt(),
			bands = (0 until effect.numberOfBands.toInt()).map { band ->
				EqBand(
					// getCenterFreq speaks milliHertz, levels millibels.
					centerHz = effect.getCenterFreq(band.toShort()) / 1000,
					levelMb = effect.getBandLevel(band.toShort()).toInt(),
				)
			},
			presets = presets,
			saved = saved,
			// A name no list carries any more, a slot deleted from another
			// session say, reads as custom rather than as a phantom entry.
			preset = presetName?.takeIf { it in presets || it in saved },
		)
	}

	private fun levels(effect: Equalizer): List<Int> =
		(0 until effect.numberOfBands.toInt()).map { effect.getBandLevel(it.toShort()).toInt() }

	private fun releaseEffect() {
		eq?.release()
		eq = null
		presetName = null
		_state.value = EqState.NoPlayer
	}

	private companion object {
		const val TAG = "GainDriveEq"
	}
}

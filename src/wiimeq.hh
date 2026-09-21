#pragma once

#include <optional>
#include <string>
#include <vector>

// The LinkPlay equalizer API a WiiM carries beside its Cast receiver:
// https://<address>/httpapi.asp?command=<command>, port 443, self-signed
// certificate. The Android client speaks it from the phone; a browser cannot
// (the certificate, and no CORS), so the server proxies it here. Everything
// about the API's shape (which commands, which quirks) is documented in
// android/WIIM.md and mirrored from playback/wiim/WiiMEq.kt, so the two
// implementations can be checked against each other.
namespace wiimeq {

	// The graphic EQ's ten fixed bands, in device index order. `param` is the
	// wire name EQSetBand must use; `label` is what a fader is captioned.
	struct Band
		{
		const char* param;
		const char* label;
		};
	extern const Band BANDS[10];

	// Fader scale. How 0..99 maps to decibels is unverified, which is why
	// clients show offsets from flat rather than dB figures.
	constexpr int LEVEL_MIN  = 0;
	constexpr int LEVEL_MAX  = 99;
	constexpr int LEVEL_FLAT = 50;

	// What EQGetBand reports. `bands` empty means the device answered but the
	// band array was missing or garbled (all ten or nothing), and a client
	// degrades to the switch and the preset list.
	struct State
		{
		bool             on = false;
		std::string      preset;   // the device's Name field
		std::vector<int> bands;    // in BANDS order
		};

	// Each returns nullopt (or false) when the device did not answer or the
	// reply was unusable. None of them throws.
	std::optional<State> state(const std::string& address);
	std::optional<std::vector<std::string>> presets(const std::string& address);
	bool load_preset(const std::string& address, const std::string& name);
	bool set_on(const std::string& address, bool on);
	bool set_bands(const std::string& address, const std::vector<int>& bands);
	}

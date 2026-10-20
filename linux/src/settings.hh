#pragma once

#include <map>
#include <string>
#include <vector>

// Per-user preferences, in settings.json in the config directory.
struct AppSettings {
	// A rough ceiling in kbps, as the other clients offer it; 0 is the
	// original file.
	int         quality_cap=0;
	// What a ceiling converts to when it has to. Kept across a spell on
	// Original, as on the apps.
	std::string format="opus";
	// A receiver fetches from the server itself, so the ceiling need not
	// apply to it. Off by default, as on the other clients.
	bool        cast_original=false;
	// Videos fetched and played as their soundtrack alone, as the other
	// clients' setting; here a toggle in the player bar.
	bool        video_audio_only=false;
	// "auto" leaves GTK's own light or dark choice alone; "light", "dark".
	std::string theme="auto";
	// A param of subtitle_sizes.
	std::string subtitle_size="medium";
	// Album order per section ("artists", "categories"): by name or by year.
	std::map<std::string, bool> sort_by_name;
	// The local equaliser, for this machine's output: on or off, the
	// curve in whole dB per band, and curves saved under a name.
	bool             eq_on=false;
	std::vector<int> eq_gains;
	std::map<std::string, std::vector<int>> eq_slots;
	// WiiM equaliser curves saved here, by Cast device id (which survives a
	// rename and a new address), then by name. Never on the device: its
	// preset store has no documented write.
	std::map<std::string, std::map<std::string, std::vector<int>>> wiim_slots;

	void load();
	void save() const;

	std::string quality_label() const;
	std::string theme_label() const;
	// The subtitle size as a fraction of the video view's height.
	double      subtitle_fraction() const;
};

// The local equaliser's faders run from -eq_range to +eq_range dB.
constexpr int eq_range = 12;

// The ceilings and formats offered, best format first.
extern const std::vector<int> quality_caps;
struct TranscodeFormat { const char *param, *label; };
extern const std::vector<TranscodeFormat> transcode_formats;

struct Choice { const char *param, *label; };
extern const std::vector<Choice> themes;
// Fractions of the picture's height, as a browser sizes its cues; medium
// is close to a browser's default.
struct SubtitleSize { const char *param, *label; double fraction; };
extern const std::vector<SubtitleSize> subtitle_sizes;

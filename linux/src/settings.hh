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
	// Album order per section ("artists", "categories"): by name or by year.
	std::map<std::string, bool> sort_by_name;
	// WiiM equaliser curves saved here, by Cast device id (which survives a
	// rename and a new address), then by name. Never on the device: its
	// preset store has no documented write.
	std::map<std::string, std::map<std::string, std::vector<int>>> wiim_slots;

	void load();
	void save() const;

	std::string quality_label() const;
};

// The ceilings and formats offered, best format first.
extern const std::vector<int> quality_caps;
struct TranscodeFormat { const char *param, *label; };
extern const std::vector<TranscodeFormat> transcode_formats;

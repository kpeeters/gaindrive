#include "settings.hh"
#include "configfile.hh"
#include "jsonread.hh"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

const std::vector<int> quality_caps = { 96, 128, 160, 192, 256 };

const std::vector<TranscodeFormat> transcode_formats = {
	{ "opus", "Opus" }, { "m4a", "AAC" }, { "mp3", "MP3" }
};

const std::vector<Choice> themes = {
	{ "auto", "Auto" }, { "light", "Light" }, { "dark", "Dark" }
};

const std::vector<SubtitleSize> subtitle_sizes = {
	{ "small", "Small", 0.035 }, { "medium", "Medium", 0.05 },
	{ "large", "Large", 0.065 }, { "xlarge", "Extra large", 0.08 }
};

namespace {
	// A local curve: whole dB within the faders' range, or empty when it is
	// not a list of numbers at all.
	std::vector<int> read_curve(const nlohmann::json& j)
		{
		std::vector<int> gains;
		if(!j.is_array()) return gains;
		for(const auto& v: j) {
			if(!v.is_number()) return {};
			gains.push_back(std::clamp(int(std::lround(v.get<double>())), -eq_range, eq_range));
			}
		return gains;
		}
}

void AppSettings::load()
	{
	auto j = read_config("settings.json");
	int cap = jint(j, "qualityCap");
	quality_cap = std::count(quality_caps.begin(), quality_caps.end(), cap) ? cap : 0;
	auto fmt = jstr(j, "transcodeFormat");
	for(const auto& f: transcode_formats)
		if(fmt==f.param) format = fmt;
	const auto& co = jsub(j, "castOriginal");
	cast_original = co.is_boolean() && co.get<bool>();
	const auto& vao = jsub(j, "videoAudioOnly");
	video_audio_only = vao.is_boolean() && vao.get<bool>();
	auto th = jstr(j, "theme");
	for(const auto& t: themes)
		if(th==t.param) theme = th;
	auto ss = jstr(j, "subtitleSize");
	for(const auto& z: subtitle_sizes)
		if(ss==z.param) subtitle_size = ss;
	const auto& sorts = jsub(j, "sortByName");
	if(sorts.is_object())
		for(const auto& [k, v]: sorts.items())
			if(v.is_boolean()) sort_by_name[k] = v.get<bool>();
	const auto& on = jsub(j, "eqOn");
	eq_on = on.is_boolean() && on.get<bool>();
	eq_gains = read_curve(jsub(j, "eqGains"));
	const auto& slots = jsub(j, "eqSlots");
	if(slots.is_object())
		for(const auto& [name, curve]: slots.items()) {
			auto gains = read_curve(curve);
			if(!gains.empty()) eq_slots[name] = gains;
			}
	const auto& wiim = jsub(j, "wiimSlots");
	if(wiim.is_object())
		for(const auto& [dev, slots]: wiim.items()) {
			if(!slots.is_object()) continue;
			for(const auto& [name, curve]: slots.items()) {
				if(!curve.is_array()) continue;
				std::vector<int> bands;
				for(const auto& v: curve)
					if(v.is_number_integer()) bands.push_back(v.get<int>());
				if(bands.size()==curve.size() && !bands.empty())
					wiim_slots[dev][name] = bands;
				}
			}
	}

void AppSettings::save() const
	{
	write_config("settings.json", {
		{"qualityCap", quality_cap}, {"transcodeFormat", format},
		{"castOriginal", cast_original}, {"videoAudioOnly", video_audio_only},
		{"theme", theme}, {"subtitleSize", subtitle_size},
		{"sortByName", sort_by_name},
		{"eqOn", eq_on}, {"eqGains", eq_gains}, {"eqSlots", eq_slots},
		{"wiimSlots", wiim_slots}
		});
	}

std::string AppSettings::quality_label() const
	{
	if(quality_cap==0) return "Original";
	std::string label = format;
	for(const auto& f: transcode_formats)
		if(format==f.param) label = f.label;
	return "Up to " + std::to_string(quality_cap) + " kbps, " + label + " when converted";
	}

std::string AppSettings::theme_label() const
	{
	for(const auto& t: themes)
		if(theme==t.param) return t.label;
	return theme;
	}

double AppSettings::subtitle_fraction() const
	{
	for(const auto& z: subtitle_sizes)
		if(subtitle_size==z.param) return z.fraction;
	return 0.05;
	}

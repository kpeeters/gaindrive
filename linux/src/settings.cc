#include "settings.hh"
#include "configfile.hh"
#include "jsonread.hh"

#include <nlohmann/json.hpp>

#include <algorithm>

const std::vector<int> quality_caps = { 96, 128, 160, 192, 256 };

const std::vector<TranscodeFormat> transcode_formats = {
	{ "opus", "Opus" }, { "m4a", "AAC" }, { "mp3", "MP3" }
};

void AppSettings::load()
	{
	auto j = read_config("settings.json");
	int cap = jint(j, "qualityCap");
	quality_cap = std::count(quality_caps.begin(), quality_caps.end(), cap) ? cap : 0;
	auto fmt = jstr(j, "transcodeFormat");
	for(const auto& f: transcode_formats)
		if(fmt==f.param) format = fmt;
	const auto& sorts = jsub(j, "sortByName");
	if(sorts.is_object())
		for(const auto& [k, v]: sorts.items())
			if(v.is_boolean()) sort_by_name[k] = v.get<bool>();
	}

void AppSettings::save() const
	{
	write_config("settings.json", {
		{"qualityCap", quality_cap}, {"transcodeFormat", format}, {"sortByName", sort_by_name}
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

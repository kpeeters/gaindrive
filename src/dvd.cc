#include "dvd.hh"

#include <cctype>
#include <algorithm>

bool dvd_parse_vob(const std::string& filename, DvdVobName& out)
	{
	// Hand-parsed rather than regex: this runs once per file in a directory
	// walk, and the shape is rigid enough that a regex would add nothing.
	if (filename.size() != 12) return false;                  // VTS_nn_m.VOB
	std::string f = filename;
	std::transform(f.begin(), f.end(), f.begin(), ::toupper);
	if (f.compare(0, 4, "VTS_") != 0) return false;
	if (f[6] != '_')                  return false;
	if (f.compare(8, 4, ".VOB") != 0) return false;
	for (int i : {4, 5, 7})
		if (!std::isdigit(static_cast<unsigned char>(f[i]))) return false;
	out.titleset = std::stoi(f.substr(4, 2));
	out.part     = f[7] - '0';
	return true;
	}

std::filesystem::path dvd_video_ts_dir(
	const std::filesystem::path& folder)
	{
	std::error_code ec;
	for (const char* name : { "VIDEO_TS", "video_ts" }) {
		auto cand = folder / name;
		if (std::filesystem::is_directory(cand, ec)) return cand;
		}
	return {};
	}

std::map<int, std::vector<std::filesystem::path>> dvd_titlesets(
	const std::filesystem::path& video_ts)
	{
	std::map<int, std::vector<std::pair<int, std::filesystem::path>>> byset;
	std::error_code ec;
	for (const auto& e : std::filesystem::directory_iterator(video_ts, ec)) {
		if (!e.is_regular_file(ec)) continue;
		DvdVobName n;
		if (!dvd_parse_vob(e.path().filename().string(), n)) continue;
		if (n.part == 0) continue;   // titleset menu, not programme
		byset[n.titleset].push_back({ n.part, e.path() });
		}

	std::map<int, std::vector<std::filesystem::path>> out;
	for (auto& [ts, parts] : byset) {
		std::sort(parts.begin(), parts.end(),
		          [](const auto& a, const auto& b) { return a.first < b.first; });
		for (auto& [part, path] : parts) out[ts].push_back(path);
		}
	return out;
	}

std::vector<std::filesystem::path> dvd_parts_for(
	const std::filesystem::path& first_vob)
	{
	DvdVobName n;
	if (!dvd_parse_vob(first_vob.filename().string(), n) || n.part == 0)
		return { first_vob };
	auto sets = dvd_titlesets(first_vob.parent_path());
	auto it   = sets.find(n.titleset);
	if (it == sets.end() || it->second.empty()) return { first_vob };
	return it->second;
	}

std::string dvd_input(const std::filesystem::path& first_vob)
	{
	auto parts = dvd_parts_for(first_vob);
	if (parts.size() < 2) return first_vob.string();
	std::string s = "concat:";
	for (size_t i = 0; i < parts.size(); ++i) {
		if (i) s += '|';
		s += parts[i].string();
		}
	return s;
	}

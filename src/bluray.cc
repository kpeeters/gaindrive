#include "bluray.hh"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <iterator>

static uint32_t be32(const std::string& d, size_t at)
	{
	return (uint32_t(uint8_t(d[at])) << 24) | (uint32_t(uint8_t(d[at + 1])) << 16)
	     | (uint32_t(uint8_t(d[at + 2])) << 8) | uint32_t(uint8_t(d[at + 3]));
	}

static uint16_t be16(const std::string& d, size_t at)
	{
	return uint16_t((uint8_t(d[at]) << 8) | uint8_t(d[at + 1]));
	}

static std::string lower(std::string s)
	{
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return s;
	}

std::optional<MplsPlaylist> bd_parse_mpls(const std::filesystem::path& mpls)
	{
	// Playlists are a few kB; anything far larger is not one.
	std::error_code ec;
	auto size = std::filesystem::file_size(mpls, ec);
	if (ec || size < 20 || size > 1024 * 1024) return std::nullopt;

	std::ifstream in(mpls, std::ios::binary);
	std::string d((std::istreambuf_iterator<char>(in)),
	              std::istreambuf_iterator<char>());
	if (d.size() < 20 || d.compare(0, 4, "MPLS") != 0) return std::nullopt;

	// PlayList(): length(4) reserved(2) number_of_PlayItems(2)
	// number_of_SubPaths(2), then the PlayItems.
	size_t pl = be32(d, 8);
	if (pl + 10 > d.size()) return std::nullopt;
	int items = be16(d, pl + 6);

	MplsPlaylist out;
	size_t at = pl + 10;
	for (int i = 0; i < items; ++i) {
		// PlayItem(): length(2), then clip name(5) codec id(4) flags(2)
		// ref_to_STC_id(1) IN_time(4) OUT_time(4); the length lets us skip
		// angles and the STN table without parsing them.
		if (at + 2 > d.size()) return std::nullopt;
		size_t len = be16(d, at);
		if (len < 20 || at + 2 + len > d.size()) return std::nullopt;
		const size_t p = at + 2;
		std::string clip = d.substr(p, 5);
		if (!std::all_of(clip.begin(), clip.end(),
		                 [](char c) { return std::isdigit(uint8_t(c)); }))
			return std::nullopt;
		uint32_t t_in  = be32(d, p + 12);
		uint32_t t_out = be32(d, p + 16);
		if (t_out > t_in) out.ticks += t_out - t_in;
		out.clips.push_back(std::move(clip));
		at = p + len;
		}
	if (out.clips.empty()) return std::nullopt;
	return out;
	}

std::filesystem::path bd_bdmv_dir(const std::filesystem::path& folder)
	{
	std::error_code ec;
	for (const char* name : { "BDMV", "bdmv" }) {
		auto cand = folder / name;
		if (std::filesystem::is_directory(cand, ec)) return cand;
		}
	return {};
	}

// Spec names are upper case; lower-cased rips are accepted as in dvd.cc.
static std::filesystem::path child_dir(const std::filesystem::path& dir,
                                       const std::string& upper)
	{
	std::error_code ec;
	for (const auto& name : { upper, lower(upper) }) {
		auto cand = dir / name;
		if (std::filesystem::is_directory(cand, ec)) return cand;
		}
	return {};
	}

static std::filesystem::path clip_file(const std::filesystem::path& stream,
                                       const std::string& clip)
	{
	std::error_code ec;
	for (const char* ext : { ".m2ts", ".M2TS" }) {
		auto cand = stream / (clip + ext);
		if (std::filesystem::is_regular_file(cand, ec)) return cand;
		}
	return {};
	}

static std::vector<std::filesystem::path> parts_of(
	const std::filesystem::path& stream, const MplsPlaylist& pl)
	{
	std::vector<std::filesystem::path> out;
	const std::string* prev = nullptr;
	for (const auto& clip : pl.clips) {
		if (prev && *prev == clip) continue;
		prev = &clip;
		auto f = clip_file(stream, clip);
		if (f.empty()) return {};
		out.push_back(std::move(f));
		}
	return out;
	}

std::filesystem::path bd_main_playlist(const std::filesystem::path& bdmv)
	{
	auto playlists = child_dir(bdmv, "PLAYLIST");
	auto stream    = child_dir(bdmv, "STREAM");
	if (playlists.empty() || stream.empty()) return {};

	std::filesystem::path best;
	int64_t               best_ticks = 0;
	std::error_code       ec;
	for (const auto& e : std::filesystem::directory_iterator(playlists, ec)) {
		if (!e.is_regular_file(ec) || !bd_is_playlist(e.path())) continue;
		auto pl = bd_parse_mpls(e.path());
		if (!pl || pl->ticks < best_ticks) continue;
		// Directory order is arbitrary; without a tie-break a rescan could
		// pick the other twin and the row's path would change under it.
		if (pl->ticks == best_ticks && !best.empty() && e.path() > best) continue;
		if (parts_of(stream, *pl).empty()) continue;
		best       = e.path();
		best_ticks = pl->ticks;
		}
	return best;
	}

std::vector<std::filesystem::path> bd_parts_for(
	const std::filesystem::path& mpls)
	{
	auto pl = bd_parse_mpls(mpls);
	if (!pl) return {};
	// PLAYLIST and STREAM are siblings under BDMV.
	auto stream = child_dir(mpls.parent_path().parent_path(), "STREAM");
	if (stream.empty()) return {};
	return parts_of(stream, *pl);
	}

bool bd_is_playlist(const std::filesystem::path& path)
	{
	return lower(path.extension().string()) == ".mpls";
	}

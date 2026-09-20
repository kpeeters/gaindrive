#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

// DVD-rip support: a folder holding a VIDEO_TS directory of .VOB files.
//
// The one fact that shapes everything here is that VOBs are MPEG-2 program
// streams **split at 1 GB boundaries**.  VTS_01_1.VOB, VTS_01_2.VOB and so on
// are contiguous pieces of a single stream, not separate videos, so they have
// to be handed to ffmpeg together via the concat: protocol.  Playing one part
// alone gives a film that stops after twenty minutes.
//
// Naming, for reference:
//   VIDEO_TS.IFO / .BUP    disc-level metadata
//   VIDEO_TS.VOB           first-play / disc menu
//   VTS_nn_0.VOB           titleset nn's *menu* — never content
//   VTS_nn_m.VOB (m >= 1)  titleset nn's programme, part m
//
// Both the scanner and the streamer need the same ordered part lists, so the
// logic lives here rather than being derived twice and drifting.

// Titleset and part numbers parsed from a VOB filename. part == 0 marks a
// menu, which is never programme content.
struct DvdVobName
	{
	int titleset = 0;
	int part     = 0;
	};

// Parses "VTS_03_2.VOB" into {3, 2}. Returns false for VIDEO_TS.VOB, for any
// other filename, and for names that merely look similar.
//
// The DVD spec fixes this name exactly: VTS_ + two digits + _ + one digit +
// .VOB, twelve characters, which caps a titleset at nine content parts.
// Compared case-insensitively because rips off case-insensitive filesystems
// routinely arrive lower-cased.
bool dvd_parse_vob(const std::string& filename, DvdVobName& out);

// The VIDEO_TS directory inside `folder`, or an empty path if there is none.
// The name is upper-case by the DVD spec, but rips off case-insensitive
// filesystems turn up lower-cased often enough to be worth accepting.
std::filesystem::path dvd_video_ts_dir(
	const std::filesystem::path& folder);

// Content VOBs grouped by titleset number, each list ordered by part. Menus
// (part 0) and every non-VOB file are excluded, so a titleset that appears
// here always has something playable in it.
std::map<int, std::vector<std::filesystem::path>> dvd_titlesets(
	const std::filesystem::path& video_ts);

// The ordered parts of the titleset that `first_vob` belongs to. Returns just
// the file itself when it is not a DVD titleset VOB, which is what makes a
// stray .vob elsewhere in the library play normally.
std::vector<std::filesystem::path> dvd_parts_for(
	const std::filesystem::path& first_vob);

// The ffmpeg input specifier for a VOB: "concat:a|b|c" when the titleset is
// split across parts, the plain path otherwise. ffmpeg's concat protocol
// implements seeking across the joined files, so -ss keeps working.
std::string dvd_input(const std::filesystem::path& first_vob);

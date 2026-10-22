#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Blu-ray rip support: a folder holding a BDMV directory.
//
// The film is not a file.  BDMV/STREAM holds numbered .m2ts clips, and a
// title is a playlist (BDMV/PLAYLIST/nnnnn.mpls) naming the clips to play in
// order.  Clips are shared between playlists, so no single .m2ts identifies
// a title; that is why a Blu-ray row's songs.path is the .mpls itself.
//
// Only the main title becomes a row, chosen as the longest playlist, which
// is also what libbluray (and so mpv) picks.  Listing every long playlist is
// not an option: copy-protected discs carry hundreds of near-identical decoy
// playlists.  CERTIFICATE/, BDMV/BACKUP/ and rippers' own files such as
// DVDFab.miniso are never looked at.
//
// Known gap: a PlayItem's in/out times are ignored and whole clips are
// played.  Main features nearly always use their clips whole, which is also
// what makes the playlist's chapter times line up with what is played.

struct MplsPlaylist
	{
	// Clip basenames ("00800"), in play order.
	std::vector<std::string> clips;
	// Sum of the PlayItems' out minus in times, in 45 kHz ticks.
	int64_t                  ticks = 0;
	// Chapter starts on that same timeline, ascending.  Discs name no
	// chapters, only place them.
	std::vector<int64_t>     marks;
	};

// Reads the PlayItem list of an .mpls file.  Empty when the file is not a
// playlist or is truncated; the format comes off a disc image of unknown
// provenance, so every offset is bounds-checked.
std::optional<MplsPlaylist> bd_parse_mpls(const std::filesystem::path& mpls);

// The BDMV directory inside `folder`, or an empty path if there is none.
// Accepts lower case for the same reason dvd_video_ts_dir() does.
std::filesystem::path bd_bdmv_dir(const std::filesystem::path& folder);

// The longest playlist in bdmv/PLAYLIST whose clips all exist in
// bdmv/STREAM, or an empty path.  A ripper that keeps only the main movie
// leaves the other playlists pointing at clips that are gone.
std::filesystem::path bd_main_playlist(const std::filesystem::path& bdmv);

// The .m2ts files a playlist plays, in order.  A clip repeated in adjacent
// PlayItems is listed once, since whole clips are played.  Empty when the
// playlist cannot be read or names a missing clip.
std::vector<std::filesystem::path> bd_parts_for(
	const std::filesystem::path& mpls);

// Whether `path` names an .mpls playlist, which is how the streamer tells a
// Blu-ray row from any other.
bool bd_is_playlist(const std::filesystem::path& path);

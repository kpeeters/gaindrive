#pragma once

// The shapes the API returns for a song, a playlist and a stream decision, and
// the validators for the two stream parameters that are not plain numbers.
//
// Split out of gaindrive.cc because browse, playlist and stream all serialise
// the same song entry, and because what stream.view will actually send has to
// be decided identically whether a client is being told about it in a listing
// or is asking for the bytes.

#include <string>

#include <httplib.h>
#include <tinyxml2.h>
#include <nlohmann/json.hpp>

#include "codecs.hh"
#include "mediastore.hh"
#include "streamer.hh"

// One <child>/<song>/<entry> element, in both spellings. max_bitrate and
// format describe what stream.view would send, so that a listing can declare
// transcodedContentType without the client having to ask.
nlohmann::json song_entry_json(const MediaStore::ChildEntry& c,
                               int max_bitrate = 0,
                               const std::string& format = "");
tinyxml2::XMLElement* song_entry_xml(tinyxml2::XMLDocument& doc,
                                     const MediaStore::ChildEntry& c,
                                     const char* tag,
                                     int max_bitrate = 0,
                                     const std::string& format = "");

// A complete getPlaylist response.
std::string playlist_body(const MediaStore::PlaylistInfo& pl, bool use_json,
                          int max_bitrate);

// Bridges MediaStore's song record to the streamer's.  Written once because
// the video fields are easy to forget in an aggregate initialiser - and a
// dropped is_video sends a video down the audio ladder, where TARGETS has no
// entry for its container and the whole tier decision is skipped.
// size_override exists for the Cast probe, which deliberately serves a slice.
Streamer::SongInfo streamer_song(const MediaStore::SongInfo& s,
                                 const std::string& abs,
                                 int64_t size_override = 0);

// The per-user bitrate cap that applies to this request, or 0 for none.
int request_max_bitrate(const httplib::Request& req, MediaStore& store);

// A frame size as the API spells it, "WxH", or empty for anything else.
//
// This is validation rather than parsing, and it is load-bearing: `size`
// reaches Streamer::video_ffmpeg_argv(), which splices it either side of the
// `x` into an ffmpeg **filtergraph** - `scale=<W>:<H>:force_original_...`. A
// filtergraph is its own language, it is not the shell but it is not inert
// either, and `-vf` accepts source filters: `movie=` and `subtitles=` both
// name a file to read, the latter rendering a text file straight into the
// frames. The trailing option can be absorbed by ending an injected chain with
// another scale, so there is no accidental protection in the concatenation.
//
// Everything else on that path was already safe - maxBitRate, timeOffset and
// duration go through to_int() and come back out through std::to_string, and
// `format` is whitelisted by target_for(). This was the one that was not.
std::string sane_video_size(const std::string& s);

// What a client declared it can be sent untouched, from the comma list
// stream.view spells `playable`. See the definition for the token grammar and
// for why an unrecognised token is dropped rather than refused.
Playable parse_playable(const std::string& s);

// Whether a caption track came from the container or from a sidecar file.
const char* caption_source(const MediaStore::CaptionTrack& c);

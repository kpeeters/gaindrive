#pragma once

#include <array>
#include <optional>
#include <string_view>

// Audio format table — the single source of truth for "what is this format
// called, and how do we ask ffmpeg for it".
//
// The `codec` column in the songs table holds a lowercased *file extension*,
// not a codec name (MediaStore derives it from the filename), so this table is
// keyed by both spellings: "m4a" and "aac" are separate entries resolving to
// different muxers for the same encoder.
//
// The distinction matters because `ffmpeg -f` takes a *muxer* name and there is
// no muxer called "m4a", "aac" or "wma".  Feeding the extension straight through
// makes ffmpeg exit before writing a byte, which reached the client as a 200
// with an empty body.  Anything built from this table passes `muxer`, never
// `name`.
struct Target
	{
	std::string_view name;     // lookup key: format name or file extension
	std::string_view muxer;    // ffmpeg -f
	std::string_view encoder;  // ffmpeg -c:a; empty => not a valid encode target
	std::string_view ext;      // cache-file extension, leading dot included
	std::string_view mime;
	bool             lossy;    // false => -b:a is meaningless, so don't pass it
	};

// Opus is muxed into a plain Ogg container rather than ffmpeg's dedicated
// "opus" muxer: the Ogg form is what clients reliably seek in (see the Navidrome
// report at github.com/navidrome/navidrome/issues/2563, where the dedicated
// muxer produced unseekable streams for Symfonium and Substreamer).
//
// wma has an empty encoder — ffmpeg can decode it, so it is a valid *source*
// for copy mode, but there is no free encoder and it must never be a target.
inline constexpr std::array<Target, 10> TARGETS = {{
	{ "opus",   "opus", "libopus",   ".opus", "audio/ogg",       true  },
	{ "ogg",    "ogg",  "libvorbis", ".ogg",  "audio/ogg",       true  },
	{ "oga",    "ogg",  "libvorbis", ".oga",  "audio/ogg",       true  },
	{ "vorbis", "ogg",  "libvorbis", ".ogg",  "audio/ogg",       true  },
	{ "mp3",    "mp3",  "libmp3lame",".mp3",  "audio/mpeg",      true  },
	{ "aac",    "adts", "aac",       ".aac",  "audio/aac",       true  },
	{ "m4a",    "ipod", "aac",       ".m4a",  "audio/mp4",       true  },
	{ "flac",   "flac", "flac",      ".flac", "audio/flac",      false },
	{ "wav",    "wav",  "pcm_s16le", ".wav",  "audio/wav",       false },
	{ "wma",    "asf",  "",          ".wma",  "audio/x-ms-wma",  true  },
	}};

// Looks up by format name or by file extension (they share a namespace here).
// Empty when the name is unknown.
inline std::optional<Target> target_for(std::string_view name)
	{
	for (const auto& t : TARGETS)
		if (t.name == name) return t;
	return std::nullopt;
	}

// Video source containers.  Deliberately a *separate* table rather than more
// rows in TARGETS: target_for() is what validates the `format` parameter of
// stream.view, and a video extension must never resolve there as an audio
// encode target.  These entries describe what a file *is*, not what ffmpeg can
// be asked to produce — the two video output formats (fragmented MP4 for
// progressive playback, MPEG-TS for HLS segments) are chosen by the streamer,
// never by the client naming an extension.
struct VideoTarget
	{
	std::string_view name;   // file extension
	std::string_view mime;
	};

inline constexpr std::array<VideoTarget, 9> VIDEO_TARGETS = {{
	{ "mkv",  "video/x-matroska" },
	{ "mp4",  "video/mp4"        },
	{ "m4v",  "video/mp4"        },
	{ "avi",  "video/x-msvideo"  },
	{ "mpg",  "video/mpeg"       },
	{ "mpeg", "video/mpeg"       },
	{ "mov",  "video/quicktime"  },
	{ "webm", "video/webm"       },
	{ "wmv",  "video/x-ms-asf"   },
	}};

inline std::optional<VideoTarget> video_target_for(std::string_view ext)
	{
	for (const auto& t : VIDEO_TARGETS)
		if (t.name == ext) return t;
	return std::nullopt;
	}

inline bool is_video_ext(std::string_view ext)
	{
	return video_target_for(ext).has_value();
	}

// songs.codec holds a lowercased extension for video rows too, so every
// existing caller of this function keeps working once the video table is
// consulted as a fallback.  Audio wins on a tie; there is no overlap today.
inline std::string_view codec_to_mime(std::string_view codec)
	{
	if (auto t = target_for(codec))       return t->mime;
	if (auto v = video_target_for(codec)) return v->mime;
	return std::string_view("application/octet-stream");
	}

// What a video transcode produces.  Tier 1 (remux) and Tier 2 (re-encode)
// share the fragmented-MP4 form; HLS segments use MPEG-TS.
inline constexpr std::string_view VIDEO_MP4_MIME = "video/mp4";
inline constexpr std::string_view VIDEO_TS_MIME  = "video/mp2t";

// Codecs a browser can be expected to decode without help, and containers it
// will accept them in.  The lists are deliberately conservative: being wrong
// in the permissive direction means a black player and a support question,
// while being wrong in the strict direction only costs a remux.
//
// These live here rather than in streamer.cc because two callers must agree on
// them: Streamer::serve_video() picks the tier, and the API advertises to the
// client whether the resulting stream can be seeked natively.  If those two
// ever disagreed, a client would seek into a stream that has no Range support
// and the seek would silently do nothing.
inline bool browser_video_codec(std::string_view c)
	{
	return c == "h264" || c == "vp8" || c == "vp9" || c == "av1";
	}

inline bool browser_audio_codec(std::string_view c)
	{
	return c == "aac" || c == "mp3" || c == "opus" || c == "vorbis"
	    || c == "flac";
	}

inline bool browser_container(std::string_view ext)
	{
	return ext == "mp4" || ext == "m4v" || ext == "webm";
	}

// True when the served stream will carry a Content-Length and answer Range
// requests, so the client can let the media element seek by itself.
//
// This is deliberately **not** "does ffmpeg run": the remux tier runs ffmpeg
// but writes a real file through the transcode cache, so it seeks just as well
// as the untouched original.  Only a re-encode is chunked and unseekable.
// Conflating the two would make a client send timeOffset for a remuxable MKV,
// which sets partial=true in serve_video() and demotes a cheap -c copy into a
// full re-encode — the exact opposite of what the tier ladder is for.
//
// An empty audio codec counts as playable: a silent video is fine, and a file
// the scanner could not probe at all is better handled by the fallbacks in
// serve_video() than by pessimising every request.
inline bool video_seeks_natively(std::string_view video_codec,
                                 std::string_view audio_codec)
	{
	return browser_video_codec(video_codec)
	    && (audio_codec.empty() || browser_audio_codec(audio_codec));
	}

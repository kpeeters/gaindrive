#pragma once

#include <functional>
#include <string>
#include <vector>
#include <httplib.h>

#include "codecs.hh"
#include "transcodecache.hh"

// Sentinel values returned by the get_position() callback during Cast streaming.
// The producer (gaindrive.cc) and consumer (streamer.cc) must use the same values.
inline constexpr float CAST_POS_STOP      = -2.0f;  // new stream started → exit immediately
inline constexpr float CAST_POS_BUFFERING = -1.0f;  // receiver seeking → suppress throttle

class Streamer {
	public:
		struct SongInfo {
			std::string path;
			std::string codec;
			int         bitrate;   // kbps; 0 = unknown
			double      duration;
			int64_t     file_size;
			int         id;            // song id, for the transcode cache key
			int64_t     file_modified; // ditto; invalidates on re-tag
			};

		// Builds the ffmpeg command line for one transcode.  Shared by the
		// streaming path and the transcode cache so the two can never drift
		// into producing different bytes for the same request.
		// out is "pipe:1" or a destination file path.
		// target_bitrate == 0 means copy mode (seek without re-encoding).
		static std::vector<std::string> ffmpeg_argv(const SongInfo& song,
		                                            int target_bitrate,
		                                            const Target& target,
		                                            int time_offset,
		                                            const std::string& out);

		// Decides direct-serve vs transcode and sends the audio response.
		// max_bitrate=0 means no limit.  format="" or "raw" means pass-through.
		// time_offset is in whole seconds (0 = from the start).
		// get_position, if set, returns the Cast receiver's current playback position
		// in seconds; used to drive adaptive buffering so the receiver never starves
		// or overflows regardless of its buffer size.
		// `cache` is always present; a disabled cache simply never produces an
		// entry, so there is no nullability to reason about at the call site.
		static void serve(const httplib::Request& req, httplib::Response& res,
		                  const SongInfo& song, TranscodeCache& cache,
		                  int max_bitrate,
		                  const std::string& format, int time_offset,
		                  bool cast_stream = false,
		                  std::function<float()> get_position = {},
		                  bool estimate_length = false);

		// Serves the original file, never transcoded and never throttled.
		// download.view is defined as "the original media data", so it must not
		// go through serve(): that would pace the response at 1x playback rate
		// for anything with a browser User-Agent.
		static void serve_raw(const httplib::Request& req, httplib::Response& res,
		                      const SongInfo& song);

	private:
		// keepalive, when set, is held for the lifetime of the response so a
		// transcode-cache entry cannot be pruned while it is being sent.  It is
		// otherwise unused — the throttle and range handling are untouched.
		static void serve_direct(const httplib::Request& req, httplib::Response& res,
		                         const SongInfo& song, bool is_browser,
		                         std::function<float()> get_position,
		                         std::shared_ptr<const TranscodeCache::Entry>
		                             keepalive = {});

		// est_length > 0 switches from a chunked response to a known-length one
		// carrying that Content-Length, truncating or zero-padding the output
		// to match.  Requested by the client via estimateContentLength.
		static void serve_transcoded(httplib::Response& res, const SongInfo& song,
		                             int target_bitrate, const Target& target,
		                             int time_offset, bool is_browser,
		                             std::function<float()> get_position,
		                             int64_t est_length = 0);
	};

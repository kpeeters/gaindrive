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
			// Video only. The codec pair decides which tier serve() takes, so
			// it has to be here rather than fetched again inside the streamer.
			bool        is_video = false;
			int         width    = 0;
			int         height   = 0;
			std::string video_codec;
			std::string audio_codec;
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
		// video_size caps the output frame size ("1280x720"); empty means keep
		// the source size. segment_duration > 0 bounds the output to that many
		// seconds from time_offset and switches the container to MPEG-TS —
		// that is one HLS segment. Both are ignored for audio.
		//
		// `pace=true` on the query string is a gaindrive extension asking for
		// the audio to be delivered at roughly 1x playback rate instead of as
		// fast as the socket takes it.  It is part of stream.view's contract
		// because only the caller knows: see serve_direct() in streamer.cc for
		// who has to send it and what happens to a connection that does not.
		static void serve(const httplib::Request& req, httplib::Response& res,
		                  const SongInfo& song, TranscodeCache& cache,
		                  int max_bitrate,
		                  const std::string& format, int time_offset,
		                  bool cast_stream = false,
		                  std::function<float()> get_position = {},
		                  bool estimate_length = false,
		                  const std::string& video_size = "",
		                  int segment_duration = 0);

		// Serves the original file, never transcoded and never paced.
		// download.view is defined as "the original media data", so it must not
		// go through serve(): that would deliver the response at 1x playback
		// rate for a browser, or for anyone asking with pace=true.
		static void serve_raw(const httplib::Request& req, httplib::Response& res,
		                      const SongInfo& song);

	private:
		// keepalive, when set, is held for the lifetime of the response so a
		// transcode-cache entry cannot be pruned while it is being sent.  It is
		// otherwise unused — the throttle and range handling are untouched.
		static void serve_direct(const httplib::Request& req, httplib::Response& res,
		                         const SongInfo& song, bool pace,
		                         std::function<float()> get_position,
		                         std::shared_ptr<const TranscodeCache::Entry>
		                             keepalive = {});

		// Builds the ffmpeg command line for one video transcode.  The video
		// counterpart of ffmpeg_argv(), and shared by the remux-to-cache and
		// encode-to-pipe paths for the same reason: two builders would drift.
		// copy=true remuxes without re-encoding; mpegts=true emits one HLS
		// segment instead of fragmented MP4.
		static std::vector<std::string> video_ffmpeg_argv(
			const SongInfo& song, bool copy, int max_bitrate,
			const std::string& size, int time_offset, int segment_duration,
			bool mpegts, const std::string& out);

		// Picks Tier 0/1/2 and sends the response.  Split out of serve() only
		// for length; it is not separately callable.  It takes no `pace`: every
		// tier passes false, for the reason given at the first of them.
		static void serve_video(const httplib::Request& req,
		                        httplib::Response& res, const SongInfo& song,
		                        TranscodeCache& cache, int max_bitrate,
		                        const std::string& format, int time_offset,
		                        const std::string& video_size,
		                        int segment_duration,
		                        std::function<float()> get_position);

		// Runs `args` and pipes its stdout to the client.  Takes a prebuilt
		// argv rather than building one, so the audio and video paths share
		// the process lifecycle — the SIGKILL-on-abort and the reproc++ EOF
		// guard are subtle enough that a second copy would be a liability.
		// bps paces the throttle; est_length > 0 switches from a chunked
		// response to a known-length one carrying that Content-Length,
		// truncating or zero-padding to match (estimateContentLength).
		static void serve_transcoded(httplib::Response& res,
		                             std::vector<std::string> args,
		                             const std::string& mime, float bps,
		                             bool pace,
		                             std::function<float()> get_position,
		                             int64_t est_length = 0);
	};

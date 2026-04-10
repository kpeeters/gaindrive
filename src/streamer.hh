#pragma once

#include <functional>
#include <string>
#include <httplib.h>

class Streamer {
	public:
		struct SongInfo {
			std::string path;
			std::string codec;
			int         bitrate;   // kbps; 0 = unknown
			double      duration;
			int64_t     file_size;
			};

		// Decides direct-serve vs transcode and sends the audio response.
		// max_bitrate=0 means no limit.  format="" or "raw" means pass-through.
		// time_offset is in whole seconds (0 = from the start).
		// get_position, if set, returns the Cast receiver's current playback position
		// in seconds; used to drive adaptive buffering so the receiver never starves
		// or overflows regardless of its buffer size.
		static void serve(const httplib::Request& req, httplib::Response& res,
		                  const SongInfo& song, int max_bitrate,
		                  const std::string& format, int time_offset,
		                  bool cast_stream = false,
		                  std::function<float()> get_position = {});

	private:
		static void serve_direct(const httplib::Request& req, httplib::Response& res,
		                         const SongInfo& song,
		                         std::function<float()> get_position);

		static void serve_transcoded(httplib::Response& res, const SongInfo& song,
		                             int target_bitrate, const std::string& target_fmt,
		                             int time_offset,
		                             std::function<float()> get_position);
	};

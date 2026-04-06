#pragma once

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
		static void serve(const httplib::Request& req, httplib::Response& res,
		                  const SongInfo& song, int max_bitrate,
		                  const std::string& format, int time_offset);

	private:
		// Serve the file directly with Range request support (HTTP 206).
		static void serve_direct(const httplib::Request& req, httplib::Response& res,
		                         const SongInfo& song);

		// Transcode via ffmpeg pipe using reproc.
		static void serve_transcoded(httplib::Response& res, const SongInfo& song,
		                             int target_bitrate, const std::string& target_fmt,
		                             int time_offset);
	};

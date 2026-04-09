#include "streamer.hh"
#include "stamp.hh"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>

#include <reproc++/reproc.hpp>

// Duplicated from gaindrive.cc — too small to warrant a shared header.
static const char* codec_to_mime(const std::string& codec)
	{
	if (codec == "flac")  return "audio/flac";
	if (codec == "mp3")   return "audio/mpeg";
	if (codec == "ogg")   return "audio/ogg";
	if (codec == "opus")  return "audio/ogg";
	if (codec == "m4a")   return "audio/mp4";
	if (codec == "aac")   return "audio/aac";
	if (codec == "wav")   return "audio/wav";
	if (codec == "wma")   return "audio/x-ms-wma";
	return "application/octet-stream";
	}

// ---- Streamer --------------------------------------------------------

void Streamer::serve(const httplib::Request& req, httplib::Response& res,
                     const SongInfo& song, int max_bitrate,
                     const std::string& format, int time_offset, bool cast_stream)
	{
	bool needs_transcode = false;
	if (time_offset > 0)
		needs_transcode = true;
	if (!format.empty() && format != "raw" && format != song.codec)
		needs_transcode = true;
	if (max_bitrate > 0 && song.bitrate > 0 && song.bitrate > max_bitrate)
		needs_transcode = true;

	// Default transcode target: mp3 (universally supported).
	std::string target_fmt     = (!format.empty() && format != "raw") ? format : "mp3";
	int         target_bitrate = (max_bitrate > 0) ? max_bitrate : 128;

	std::cout << stamp() << "stream ["
	          << song.path << "] codec=" << song.codec
	          << " src_bitrate=" << song.bitrate
	          << " max_bitrate=" << max_bitrate
	          << " format=" << (format.empty() ? "(none)" : format)
	          << " time_offset=" << time_offset
	          << " transcode=" << (needs_transcode ? "yes" : "no")
	          << " cast=" << (cast_stream ? "yes" : "no")
	          << std::endl;

	if (needs_transcode)
		serve_transcoded(res, song, target_bitrate, target_fmt, time_offset, cast_stream);
	else
		serve_direct(req, res, song, cast_stream);
	}

void Streamer::serve_direct(const httplib::Request& req, httplib::Response& res,
                            const SongInfo& song, bool cast_stream)
	{
	// Cast receivers (WiiM) have a ~20 MB download buffer.  Without throttling they
	// download the entire buffer at LAN speed, close the connection, and stop playing
	// when the buffer runs out.
	//
	// Fix: allow a 30-second pre-buffer at full speed so playback starts immediately,
	// then throttle to ~1.05× bitrate.  At 1.05× the buffer grows by only 0.05 s per
	// second of playback, so even a 50-minute track never accumulates more than ~15 MB.
	//
	// pre_bps  = 0 means "no limit" (pre-buffer phase, send as fast as possible)
	// post_bps = 0 means "no throttle" (non-cast streams)
	const size_t prebuf_bytes = (cast_stream && song.bitrate > 0)
	    ? static_cast<size_t>(song.bitrate) * 125 * 30   // 30 s at 1× rate
	    : 0;
	const size_t post_bps = (cast_stream && song.bitrate > 0)
	    ? static_cast<size_t>(song.bitrate) * 131         // ≈ 1.048× rate
	    : 0;

	// httplib parses the Range header and calls our provider with the correct
	// offset/length when a known content-length is supplied.
	res.set_content_provider(
		static_cast<size_t>(song.file_size),
		codec_to_mime(song.codec),
		[path = song.path, cast_stream, prebuf_bytes, post_bps]
		(size_t offset, size_t length, httplib::DataSink& sink) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			f.seekg(static_cast<std::streamoff>(offset));
			char   buf[65536];
			size_t remaining = length;

			// Timer and counter for the throttle phase (starts after prebuf_bytes).
			using Clock = std::chrono::steady_clock;
			Clock::time_point throttle_t0{};
			size_t            throttle_sent = 0;

			while (remaining > 0) {
				auto to_read = static_cast<std::streamsize>(
				    std::min(remaining, sizeof(buf)));
				f.read(buf, to_read);
				auto n = static_cast<size_t>(f.gcount());
				if (n == 0) break;
				if (!sink.write(buf, n)) {
					if (cast_stream)
						std::cout << stamp()
						          << "cast stream: write failed at "
						          << (offset + length - remaining + n)
						          << " bytes" << std::endl;
					return false;
					}
				remaining -= n;

				if (post_bps > 0) {
					// Global file position after this write.
					size_t pos = offset + (length - remaining);
					if (pos > prebuf_bytes) {
						if (throttle_t0 == Clock::time_point{}) {
							// First chunk past the pre-buffer — start the throttle clock.
							throttle_t0   = Clock::now();
							throttle_sent = pos - prebuf_bytes;
							}
						else {
							throttle_sent += n;
							}
						namespace ch = std::chrono;
						auto want_us = throttle_sent * 1'000'000ULL / post_bps;
						auto have_us = static_cast<uint64_t>(
						    ch::duration_cast<ch::microseconds>(
						        Clock::now() - throttle_t0).count());
						if (want_us > have_us + 10'000)
							std::this_thread::sleep_for(
							    ch::microseconds(want_us - have_us));
						}
					}
				}
			return true;
			});
	}

void Streamer::serve_transcoded(httplib::Response& res, const SongInfo& song,
                                int target_bitrate, const std::string& target_fmt,
                                int time_offset, bool cast_stream)
	{
	std::vector<std::string> args;
	args.push_back("ffmpeg");
	if (time_offset > 0) {
		// -ss before -i for fast input seek (keyframe-accurate is close enough).
		args.push_back("-ss");
		args.push_back(std::to_string(time_offset));
		}
	args.push_back("-i");
	args.push_back(song.path);
	args.push_back("-f");
	args.push_back(target_fmt);
	args.push_back("-b:a");
	args.push_back(std::to_string(target_bitrate) + "k");
	args.push_back("pipe:1");

	auto proc = std::make_shared<reproc::process>();
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;

	auto ec = proc->start(args, opts);
	if (ec) {
		std::cout << stamp() << "stream: ffmpeg launch failed: "
		          << ec.message() << std::endl;
		res.status = 500;
		return;
		}

	// Same two-phase throttle as serve_direct: 30-second pre-buffer then ~1.05×.
	const size_t prebuf_bytes = cast_stream
	    ? static_cast<size_t>(target_bitrate) * 125 * 30
	    : 0;
	const size_t post_bps = cast_stream
	    ? static_cast<size_t>(target_bitrate) * 131
	    : 0;

	struct ThrottleState {
		std::chrono::steady_clock::time_point t0{};
		size_t total_sent = 0;
		size_t throttle_sent = 0;
		};
	auto ts = std::make_shared<ThrottleState>();

	// Content-length is unknown for transcoded output.
	res.set_header("Accept-Ranges", "none");
	res.set_content_provider(
		codec_to_mime(target_fmt),
		[proc, ts, prebuf_bytes, post_bps](size_t /*offset*/, httplib::DataSink& sink) {
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf, sizeof(buf));
			if (n == 0) {
				// EOF or error — ffmpeg is done.
				sink.done();
				return false;
				}
			if (!sink.write(reinterpret_cast<char*>(buf), n)) return false;
			ts->total_sent += n;
			if (post_bps > 0 && ts->total_sent > prebuf_bytes) {
				if (ts->t0 == std::chrono::steady_clock::time_point{}) {
					ts->t0           = std::chrono::steady_clock::now();
					ts->throttle_sent = ts->total_sent - prebuf_bytes;
					}
				else {
					ts->throttle_sent += n;
					}
				auto want_us = ts->throttle_sent * 1'000'000ULL / post_bps;
				auto have_us = static_cast<uint64_t>(
				    std::chrono::duration_cast<std::chrono::microseconds>(
				        std::chrono::steady_clock::now() - ts->t0).count());
				if (want_us > have_us + 10'000)
					std::this_thread::sleep_for(
					    std::chrono::microseconds(want_us - have_us));
				}
			return true;
			},
		[proc](bool success) {
			// On client disconnect (success=false) stop ffmpeg; always wait.
			if (!success) proc->terminate();
			proc->wait(reproc::infinite);
			});
	}

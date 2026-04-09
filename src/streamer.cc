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
                     const std::string& format, int time_offset, bool throttle)
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
	          << " throttle=" << (throttle ? "yes" : "no")
	          << std::endl;

	if (needs_transcode)
		serve_transcoded(res, song, target_bitrate, target_fmt, time_offset, throttle);
	else
		serve_direct(req, res, song, throttle);
	}

void Streamer::serve_direct(const httplib::Request& req, httplib::Response& res,
                            const SongInfo& song, bool throttle)
	{
	// When throttling (Cast streams): limit delivery to ~1.024× the audio bitrate so
	// the receiver never buffers far ahead and closes the connection early.
	// bytes_per_sec = bitrate_kbps * 1000/8 * 1.024 ≈ bitrate_kbps * 128.
	const size_t bytes_per_sec = (throttle && song.bitrate > 0)
	    ? static_cast<size_t>(song.bitrate) * 128
	    : size_t{0};

	// httplib parses the Range header and calls our provider with the correct
	// offset/length when a known content-length is supplied.
	res.set_content_provider(
		static_cast<size_t>(song.file_size),
		codec_to_mime(song.codec),
		[path = song.path, bytes_per_sec](size_t offset, size_t length,
		                                   httplib::DataSink& sink) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			f.seekg(static_cast<std::streamoff>(offset));
			char   buf[65536];
			size_t remaining = length;
			auto   t0   = std::chrono::steady_clock::now();
			size_t sent = 0;
			while (remaining > 0) {
				auto to_read = static_cast<std::streamsize>(
				    std::min(remaining, sizeof(buf)));
				f.read(buf, to_read);
				auto n = static_cast<size_t>(f.gcount());
				if (n == 0) break;
				if (!sink.write(buf, n)) return false;
				remaining -= n;
				sent      += n;
				if (bytes_per_sec > 0) {
					namespace ch = std::chrono;
					auto want_us = sent * 1'000'000ULL / bytes_per_sec;
					auto have_us = static_cast<uint64_t>(
					    ch::duration_cast<ch::microseconds>(
					        ch::steady_clock::now() - t0).count());
					if (want_us > have_us + 10'000)
						std::this_thread::sleep_for(
						    ch::microseconds(want_us - have_us));
					}
				}
			return true;
			});
	}

void Streamer::serve_transcoded(httplib::Response& res, const SongInfo& song,
                                int target_bitrate, const std::string& target_fmt,
                                int time_offset, bool throttle)
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

	// Same throttle as serve_direct: bytes/s = target_bitrate_kbps * 128 ≈ 1.024×.
	const size_t bytes_per_sec = (throttle && target_bitrate > 0)
	    ? static_cast<size_t>(target_bitrate) * 128
	    : size_t{0};

	struct Pace {
		std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
		size_t sent = 0;
		};
	auto pace = std::make_shared<Pace>();

	// Content-length is unknown for transcoded output.
	res.set_header("Accept-Ranges", "none");
	res.set_content_provider(
		codec_to_mime(target_fmt),
		[proc, pace, bytes_per_sec](size_t /*offset*/, httplib::DataSink& sink) {
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf, sizeof(buf));
			if (n == 0) {
				// EOF or error — ffmpeg is done.
				sink.done();
				return false;
				}
			if (!sink.write(reinterpret_cast<char*>(buf), n)) return false;
			pace->sent += n;
			if (bytes_per_sec > 0) {
				namespace ch = std::chrono;
				auto want_us = pace->sent * 1'000'000ULL / bytes_per_sec;
				auto have_us = static_cast<uint64_t>(
				    ch::duration_cast<ch::microseconds>(
				        ch::steady_clock::now() - pace->t0).count());
				if (want_us > have_us + 10'000)
					std::this_thread::sleep_for(
					    ch::microseconds(want_us - have_us));
				}
			return true;
			},
		[proc](bool success) {
			// On client disconnect (success=false) stop ffmpeg; always wait.
			if (!success) proc->terminate();
			proc->wait(reproc::infinite);
			});
	}

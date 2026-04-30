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

// Seconds of audio to keep buffered ahead of the Cast receiver's playback
// position.  Large enough to absorb brief network jitter; small enough that
// any receiver can hold it.
static constexpr float TARGET_BUF = 15.0f;

// ---- Streamer --------------------------------------------------------

void Streamer::serve(const httplib::Request& req, httplib::Response& res,
                     const SongInfo& song, int max_bitrate,
                     const std::string& format, int time_offset,
                     bool cast_stream, std::function<float()> get_position)
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
		serve_transcoded(res, song, target_bitrate, target_fmt, time_offset,
		                 std::move(get_position));
	else
		serve_direct(req, res, song, std::move(get_position));
	}

void Streamer::serve_direct(const httplib::Request& req, httplib::Response& res,
                            const SongInfo& song,
                            std::function<float()> get_position)
	{
	// Cast adaptive buffering:
	// - Send the first 30 s of audio at full LAN speed (pre-buffer) so the
	//   receiver starts playing immediately.
	// - After the pre-buffer, check the receiver's actual playback position
	//   via get_position() and sleep whenever we are more than TARGET_BUF
	//   seconds ahead.  This keeps the buffer stable without any device-
	//   specific size constants and handles pause correctly (current_time
	//   freezes → we stop sending until the user resumes).
	const float  bytes_per_sec = (get_position && song.bitrate > 0)
	    ? static_cast<float>(song.bitrate) * 125.0f
	    : 0.0f;
	const size_t prebuf_bytes = bytes_per_sec > 0
	    ? static_cast<size_t>(bytes_per_sec) * 30   // 30 s at 1× rate
	    : 0;
	// httplib parses the Range header and calls our provider with the correct
	// offset/length when a known content-length is supplied.
	res.set_content_provider(
		static_cast<size_t>(song.file_size),
		codec_to_mime(song.codec),
		[path = song.path, bytes_per_sec, prebuf_bytes,
		 get_position = std::move(get_position)]
		(size_t offset, size_t length, httplib::DataSink& sink) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			f.seekg(static_cast<std::streamoff>(offset));
			char   buf[65536];
			size_t remaining = length;

			while (remaining > 0) {
				auto to_read = static_cast<std::streamsize>(
				    std::min(remaining, sizeof(buf)));
				f.read(buf, to_read);
				auto n = static_cast<size_t>(f.gcount());
				if (n == 0) break;
				if (!sink.write(buf, n)) {
					if (get_position)
						std::cout << stamp()
						          << "cast stream: write failed at "
						          << (offset + length - remaining + n)
						          << " bytes" << std::endl;
					return false;
					}
				remaining -= n;

				// Adaptive throttle: keep the receiver's buffer at ~TARGET_BUF s.
				// A negative position signals BUFFERING (seeking) — skip throttle so
				// data flows freely until the receiver transitions to PLAYING.
				// Guard uses bytes sent within THIS range request (not offset+sent) so
				// that Range requests starting near the end of the file (e.g. metadata
				// fetches for OGG/FLAC seeking) are not immediately throttled.
				if (get_position && bytes_per_sec > 0) {
					size_t bytes_sent = length - remaining;
					if (bytes_sent > prebuf_bytes) {
						float receiver_pos = get_position();
						if (receiver_pos >= 0.0f) {
							float audio_sent = static_cast<float>(offset + bytes_sent) / bytes_per_sec;
							float buf_secs   = audio_sent - receiver_pos;
							if (buf_secs > TARGET_BUF) {
								auto sleep_ms = static_cast<long>(
								    (buf_secs - TARGET_BUF) * 1000.0f);
								std::this_thread::sleep_for(
								    std::chrono::milliseconds(sleep_ms));
								}
							}
						}
					}
				}
			return true;
			});
	}

void Streamer::serve_transcoded(httplib::Response& res, const SongInfo& song,
                                int target_bitrate, const std::string& target_fmt,
                                int time_offset,
                                std::function<float()> get_position)
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

	// Same adaptive buffering as serve_direct; target_bitrate is always known.
	const float  bps        = static_cast<float>(target_bitrate) * 125.0f;
	const size_t prebuf_bytes = get_position
	    ? static_cast<size_t>(bps) * 30
	    : 0;

	// total_sent persists across repeated provider calls (one per chunk).
	auto total_sent = std::make_shared<size_t>(0);

	// Content-length is unknown for transcoded output.
	res.set_header("Accept-Ranges", "none");
	res.set_content_provider(
		codec_to_mime(target_fmt),
		[proc, bps, prebuf_bytes, total_sent,
		 get_position = std::move(get_position)]
		(size_t /*offset*/, httplib::DataSink& sink) {
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf, sizeof(buf));
			if (n == 0) {
				// EOF or error — ffmpeg is done.
				sink.done();
				return false;
				}
			if (!sink.write(reinterpret_cast<char*>(buf), n)) return false;
			*total_sent += n;

			if (get_position && bps > 0 && *total_sent > prebuf_bytes) {
				float receiver_pos = get_position();
				if (receiver_pos >= 0.0f) {
					float audio_sent = static_cast<float>(*total_sent) / bps;
					float buf_secs   = audio_sent - receiver_pos;
					if (buf_secs > TARGET_BUF) {
						auto sleep_ms = static_cast<long>(
						    (buf_secs - TARGET_BUF) * 1000.0f);
						std::this_thread::sleep_for(
						    std::chrono::milliseconds(sleep_ms));
						}
					}
				}
			return true;
			},
		[proc](bool success) {
			// On client disconnect (success=false) stop ffmpeg; always wait.
			if (!success) proc->terminate();
			proc->wait(reproc::infinite);
			});
	}

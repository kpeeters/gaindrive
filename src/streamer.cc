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
static constexpr float  TARGET_BUF  = 15.0f;
static constexpr size_t WRITE_CHUNK = 4096;

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

	// Determine transcode target.  When only a time-offset seek is needed
	// (no format conversion, no bitrate limit) preserve the original codec via
	// ffmpeg -c:a copy so there is no quality loss.  target_bitrate == 0
	// signals copy mode to serve_transcoded.
	bool format_change = !format.empty() && format != "raw" && format != song.codec;
	bool bitrate_limit = max_bitrate > 0 && song.bitrate > 0 && song.bitrate > max_bitrate;

	std::string target_fmt;
	int         target_bitrate;
	if (format_change) {
		target_fmt     = format;
		target_bitrate = (max_bitrate > 0) ? max_bitrate : 128;
		}
	else if (bitrate_limit) {
		target_fmt     = "mp3";
		target_bitrate = max_bitrate;
		}
	else {
		// Seek only: no re-encode.
		target_fmt     = song.codec;
		target_bitrate = 0;
		}

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
				if (get_position && get_position() < CAST_POS_BUFFERING) return false;
				auto to_read = static_cast<std::streamsize>(
				    std::min(remaining, sizeof(buf)));
				f.read(buf, to_read);
				auto n = static_cast<size_t>(f.gcount());
				if (n == 0) break;

				for (size_t woff = 0; woff < n; ) {
					if (get_position && get_position() < CAST_POS_BUFFERING) return false;
					size_t piece = std::min(WRITE_CHUNK, n - woff);
					if (!sink.write(buf + woff, piece)) {
						if (get_position)
							std::cout << stamp()
							          << "cast stream: write failed at "
							          << (offset + length - remaining + woff + piece)
							          << " bytes" << std::endl;
						return false;
						}
					woff += piece;
					}
				remaining -= n;

				if (get_position) {
					size_t bytes_sent = length - remaining;
					float pos = get_position();
					if (pos < CAST_POS_BUFFERING) return false;
					// Throttling only runs while the receiver is actually PLAYING.
					// During IDLE/BUFFERING (pos < 0) we send freely — TCP
					// backpressure paces us once the receiver's buffer fills, but
					// any explicit throttle here causes the receiver to time out
					// while we sleep, which manifests as "track click → never
					// starts playing" or "seek → playback stops altogether".
					if (pos >= 0.0f && bytes_per_sec > 0 && bytes_sent > prebuf_bytes) {
						// Throttle in **absolute** audio time so that Range requests
						// (cast native seek issues these against the same URL) compare
						// correctly: bytes_sent is local to this request, but pos is the
						// receiver's absolute position in the track.
						float audio_sent_abs = static_cast<float>(offset + bytes_sent)
						                     / bytes_per_sec;
						// If the receiver is far ahead of what this connection is
						// serving, it has moved on to a different Range request (cast
						// seek pattern: original no-Range from byte 0, then receiver
						// issues a Range at the seek byte and stops reading the first).
						// Exit so we don't blast bytes that won't be consumed.
						if (pos > audio_sent_abs + TARGET_BUF * 2.0f) return false;
						float buf_secs = audio_sent_abs - pos;
						if (buf_secs > TARGET_BUF) {
							auto sleep_ms = static_cast<long>(
							    (buf_secs - TARGET_BUF) * 1000.0f);
							auto deadline = std::chrono::steady_clock::now()
							              + std::chrono::milliseconds(sleep_ms);
							while (std::chrono::steady_clock::now() < deadline) {
								std::this_thread::sleep_for(
								    std::chrono::milliseconds(100));
								float pos_now = get_position();
								if (pos_now < CAST_POS_BUFFERING) return false;
								if (pos_now > audio_sent_abs + TARGET_BUF * 2.0f)
									return false;
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
	if (target_bitrate > 0) {
		args.push_back("-b:a");
		args.push_back(std::to_string(target_bitrate) + "k");
		}
	else {
		// Seek only — copy audio without re-encoding.
		args.push_back("-c:a");
		args.push_back("copy");
		}
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

	// For copy mode use the source bitrate for throttling; otherwise the target.
	const float  bps        = target_bitrate > 0
	    ? static_cast<float>(target_bitrate) * 125.0f
	    : static_cast<float>(song.bitrate)   * 125.0f;
	// total_sent persists across repeated provider calls (one per chunk).
	auto total_sent = std::make_shared<size_t>(0);

	// Content-length is unknown for transcoded output.
	res.set_header("Accept-Ranges", "none");
	res.set_content_provider(
		codec_to_mime(target_fmt),
		[proc, bps, total_sent,
		 get_position = std::move(get_position)]
		(size_t /*offset*/, httplib::DataSink& sink) {
			if (get_position && get_position() < CAST_POS_BUFFERING) {
				std::cout << stamp() << "stream: transcoded abort (gen mismatch or stop)" << std::endl;
				return false;
				}
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf, sizeof(buf));
			if (n == 0 || err) {
				// Check err before n: reproc wraps negative C return values into
				// size_t, so err is the reliable EOF/error indicator.
				if (err && err != std::make_error_code(std::errc::broken_pipe))
					std::cout << stamp() << "stream: ffmpeg pipe error: "
					          << err.message() << std::endl;
				sink.done();
				return false;
				}
			for (size_t off = 0; off < n; ) {
				if (get_position && get_position() < CAST_POS_BUFFERING) return false;
				size_t piece = std::min(WRITE_CHUNK, n - off);
				if (!sink.write(reinterpret_cast<char*>(buf + off), piece)) {
					std::cout << stamp() << "stream: transcoded write failed at "
					          << *total_sent << " bytes" << std::endl;
					return false;
					}
				off         += piece;
				*total_sent += piece;
				}
			if (get_position) {
				float pos = get_position();
				if (pos < CAST_POS_BUFFERING) return false;
				if (bps > 0) {
					float audio_sent = static_cast<float>(*total_sent) / bps;
					// During BUFFERING (pos=-1), treat effective position as 0 so
					// the throttle applies immediately and the receiver's buffer
					// can't be flooded before playback begins.
					float effective_pos = (pos >= 0.0f) ? pos : 0.0f;
					float buf_secs   = audio_sent - effective_pos;
					if (buf_secs > TARGET_BUF) {
						auto sleep_ms = static_cast<long>(
						    (buf_secs - TARGET_BUF) * 1000.0f);
						auto deadline = std::chrono::steady_clock::now()
						              + std::chrono::milliseconds(sleep_ms);
						while (std::chrono::steady_clock::now() < deadline) {
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(100));
							if (get_position() < CAST_POS_BUFFERING) return false;
							}
						}
					}
				}
			return true;
			},
		[proc](bool success) {
			if (!success) proc->kill();
			auto [status, ec] = proc->wait(reproc::infinite);
			if (!success || status != 0)
				std::cout << stamp() << "stream: ffmpeg exit status=" << status
				          << (ec ? " (" + ec.message() + ")" : "") << std::endl;
			});
	}

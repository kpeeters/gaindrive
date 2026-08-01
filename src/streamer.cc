#include "streamer.hh"
#include "stamp.hh"

#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>

#include <cstdint>
#include <cstdio>

#include <reproc++/reproc.hpp>

// Reads the tail of a captured stderr stream for logging.  ffmpeg says nothing
// useful on success and prints the reason on the last line or two when it
// fails, so the tail is the part worth keeping.  Newlines are flattened so one
// failure produces one log line.
static std::string stderr_tail(FILE* f, size_t max_bytes = 4096)
	{
	if (!f) return {};
	std::fflush(f);
	if (std::fseek(f, 0, SEEK_END) != 0) return {};
	long end = std::ftell(f);
	if (end <= 0) return {};
	long start = end > static_cast<long>(max_bytes)
	           ? end - static_cast<long>(max_bytes) : 0;
	if (std::fseek(f, start, SEEK_SET) != 0) return {};
	std::string buf(static_cast<size_t>(end - start), '\0');
	buf.resize(std::fread(buf.data(), 1, buf.size(), f));
	for (auto& c : buf)
		if (c == '\n' || c == '\r') c = ' ';
	return buf;
	}

// Outcome of one transcode-provider iteration.  Eof and Abort are distinct
// because the two provider variants end differently: the chunked one calls
// sink.done(), the known-length one pads out to the length it promised.
enum class Pump { More, Eof, Abort };

// Seconds of audio to keep buffered ahead of the Cast receiver's playback
// position.  Large enough to absorb brief network jitter; small enough that
// any receiver can hold it.
static constexpr float  TARGET_BUF  = 15.0f;
static constexpr size_t WRITE_CHUNK = 4096;

// ---- Streamer --------------------------------------------------------

void Streamer::serve(const httplib::Request& req, httplib::Response& res,
                     const SongInfo& song, TranscodeCache& cache,
                     int max_bitrate,
                     const std::string& format, int time_offset,
                     bool cast_stream, std::function<float()> get_position,
                     bool estimate_length)
	{
	auto source = target_for(song.codec);

	// A format request that resolves to the same muxer and encoder as the
	// source is not a change: ".oga" and "ogg" are the same thing spelled two
	// ways, and re-encoding one into the other would lose quality for nothing.
	std::optional<Target> wanted;
	if (!format.empty() && format != "raw")
		wanted = target_for(format);
	bool format_change = wanted && (!source
	                     || source->muxer   != wanted->muxer
	                     || source->encoder != wanted->encoder);
	bool bitrate_limit = max_bitrate > 0 && song.bitrate > 0 && song.bitrate > max_bitrate;
	bool needs_transcode = time_offset > 0 || format_change || bitrate_limit;

	// Determine transcode target.  When only a time-offset seek is needed
	// (no format conversion, no bitrate limit) preserve the original codec via
	// ffmpeg -c:a copy so there is no quality loss.  target_bitrate == 0
	// signals copy mode to serve_transcoded.
	std::optional<Target> target;
	int                   target_bitrate;
	if (format_change) {
		target         = wanted;
		target_bitrate = (max_bitrate > 0 && max_bitrate < 320) ? max_bitrate : 320;
		}
	else if (bitrate_limit) {
		target         = target_for("mp3");
		target_bitrate = max_bitrate;
		}
	else {
		// Seek only: no re-encode.
		target         = source;
		target_bitrate = 0;
		}

	// No table entry means no ffmpeg muxer for this container.  Passing the
	// extension through as -f made ffmpeg exit before writing a byte, which the
	// client saw as a 200 with an empty body.  Serving the raw file instead
	// ignores the seek, which is a far better failure than silence.
	if (needs_transcode && !target) {
		std::cout << stamp() << "stream: no muxer for codec '" << song.codec
		          << "', serving raw" << std::endl;
		needs_transcode = false;
		}

	bool is_browser = req.get_header_value("User-Agent").find("Mozilla/")
	                  != std::string::npos;

	std::cout << stamp() << "stream ["
	          << song.path << "] codec=" << song.codec
	          << " src_bitrate=" << song.bitrate
	          << " max_bitrate=" << max_bitrate
	          << " format=" << (format.empty() ? "(none)" : format)
	          << " time_offset=" << time_offset
	          << " transcode=" << (needs_transcode ? "yes" : "no")
	          << (needs_transcode ? " target=" + std::string(target->name) : "")
	          << " cast=" << (cast_stream ? "yes" : "no")
	          << " browser=" << (is_browser ? "yes" : "no")
	          << std::endl;

	if (!needs_transcode) {
		serve_direct(req, res, song, is_browser, std::move(get_position));
		return;
		}

	// Cast never reaches the cache.  Today it cannot: a cast URL carries no
	// format and no maxBitRate, and its time_offset is always 0 under native
	// seek, so needs_transcode is already false.  The guard is here so that if
	// Cast seeking is ever changed, a Chromecast cannot end up waiting on a
	// blocking materialise — the receiver drops the session after ~60 s with no
	// data, which surfaces as error 103 and looks like a metadata bug.
	//
	// A seek is not cached either: the web client's local-seek trick needs the
	// stream to *start* at the offset, and caching one file per offset has no
	// bound.
	if (!cast_stream && time_offset == 0) {
		// The mtime in the key is what invalidates a stale transcode after the
		// source file is re-tagged or replaced.
		std::string key = std::to_string(song.id) + "-"
		                + std::to_string(song.file_modified) + "-"
		                + std::string(target->name)
		                + std::to_string(target_bitrate);
		static const std::string OUT = "\x01cache-out\x01";
		auto argv = ffmpeg_argv(song, target_bitrate, *target, 0, OUT);
		if (auto entry = cache.get_or_build(key, std::string(target->ext),
		                                    argv, OUT)) {
			// Serving a real file rather than a pipe is the whole point: it
			// carries a Content-Length, answers Range requests, and (because
			// ffmpeg could seek backwards while writing it) actually has a
			// XING header or seektable to seek with.
			SongInfo cached{ entry->path().string(), std::string(target->name),
			                 target_bitrate, song.duration, entry->size(),
			                 song.id, song.file_modified };
			// Hold the entry for as long as the response lives so prune()
			// cannot delete the file mid-send.
			res.set_header("X-Gaindrive-Transcode",
			               entry->hit() ? "hit" : "miss");
			serve_direct(req, res, cached, is_browser,
			             std::move(get_position), entry);
			return;
			}
		}

	// httplib's post-handler range_error() rejects any Range request whose
	// last byte exceeds the response's known length.  Our transcoded output
	// uses a chunked content provider with unknown length (= 0 in the
	// check), so a browser's automatic "Range: bytes=0-" lands as 416.
	// Drop the parsed ranges before the check runs — the transcoded stream
	// is sequential anyway and there is nothing to seek into byte-wise.
	const_cast<httplib::Request&>(req).ranges.clear();

	// Only meaningful on this path: a cache hit already carries a real length.
	int64_t est_length = 0;
	if (estimate_length) {
		int    kbps = target_bitrate > 0 ? target_bitrate : song.bitrate;
		double secs = song.duration - time_offset;
		if (kbps > 0 && secs > 0)
			est_length = static_cast<int64_t>(secs * kbps * 125.0);
		}

	serve_transcoded(res, song, target_bitrate, *target, time_offset,
	                 is_browser, std::move(get_position), est_length);
	}

void Streamer::serve_raw(const httplib::Request& req, httplib::Response& res,
                         const SongInfo& song)
	{
	// is_browser=false deliberately, regardless of the actual User-Agent: the
	// throttle exists to keep a *playback* connection alive, and a download
	// wants the bytes as fast as the socket takes them.
	serve_direct(req, res, song, false, {});
	}

void Streamer::serve_direct(const httplib::Request& req, httplib::Response& res,
                            const SongInfo& song, bool is_browser,
                            std::function<float()> get_position,
                            std::shared_ptr<const TranscodeCache::Entry> keepalive)
	{
	// Adaptive buffering:
	// - Cast: send the first 30 s at full speed, then throttle to stay at most
	//   TARGET_BUF seconds ahead of the receiver's reported playback position.
	// - Browser (non-cast): same throttle using wall-clock elapsed time as a
	//   proxy for playback position.  Keeps the HTTP connection alive for the
	//   full track duration so the browser never needs to evict and re-fetch
	//   buffered content — which was the root cause of intermittent
	//   MEDIA_ERR_NETWORK errors on the audio element.
	// - Other clients (Subsonic apps, etc.): no throttle; TCP backpressure
	//   paces the send rate naturally.
	const float  bytes_per_sec = (song.bitrate > 0)
	    ? static_cast<float>(song.bitrate) * 125.0f
	    : 0.0f;
	const size_t prebuf_bytes = bytes_per_sec > 0
	    ? static_cast<size_t>(bytes_per_sec) * 30   // 30 s at 1× rate
	    : 0;
	// httplib parses the Range header and calls our provider with the correct
	// offset/length when a known content-length is supplied.
	res.set_content_provider(
		static_cast<size_t>(song.file_size),
		std::string(codec_to_mime(song.codec)),
		[path = song.path, bytes_per_sec, prebuf_bytes, is_browser,
		 get_position = std::move(get_position), keepalive = std::move(keepalive)]
		(size_t offset, size_t length, httplib::DataSink& sink) {
			std::ifstream f(path, std::ios::binary);
			if (!f) return false;
			f.seekg(static_cast<std::streamoff>(offset));
			char   buf[65536];
			size_t remaining = length;
			auto   t_start   = std::chrono::steady_clock::now();

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
						std::cout << stamp()
						          << (get_position ? "cast " : "") << "stream: write failed at "
						          << (offset + length - remaining + woff + piece)
						          << " bytes (range offset=" << offset << ")" << std::endl;
						return false;
						}
					woff += piece;
					}
				remaining -= n;

				if (bytes_per_sec > 0) {
					size_t bytes_sent = length - remaining;
					if (get_position) {
						float pos = get_position();
						if (pos < CAST_POS_BUFFERING) return false;
						// Throttling only runs while the receiver is actually PLAYING.
						// During IDLE/BUFFERING (pos < 0) we send freely — TCP
						// backpressure paces us once the receiver's buffer fills, but
						// any explicit throttle here causes the receiver to time out
						// while we sleep, which manifests as "track click → never
						// starts playing" or "seek → playback stops altogether".
						if (pos >= 0.0f && bytes_sent > prebuf_bytes) {
							// Throttle in **absolute** audio time so that Range requests
							// (MP3 native seek issues these against the same URL) compare
							// correctly: bytes_sent is local to this request, but pos is the
							// receiver's absolute position in the track.  Non-MP3 codecs go
							// through serve_transcoded for any seek, so this path only sees
							// MP3 native seek among the cast cases.
							// No "receiver moved on" early-exit: when MP3 native seek opens
							// a new Range at the seek byte it abandons this connection
							// without a reliable signal here, and pos jumps ahead of what
							// we've sent.  Trust TCP backpressure plus the receiver's own
							// connection-close to clean it up; a lingering blocked
							// sink.write costs at worst the 1-hour httplib write timeout.
							float audio_sent_abs = static_cast<float>(offset + bytes_sent)
							                     / bytes_per_sec;
							float buf_secs = audio_sent_abs - pos;
							if (buf_secs > TARGET_BUF) {
								// Cap to 2 s: bytes_sent overcounts what the receiver
								// actually has (the local kernel send buffer can hold
								// many seconds beyond what reached the device).  A
								// longer single sleep silences the server past the
								// Chromecast's no-data network timeout (~60 s, manifests
								// as error 103).  A short cap forces the outer loop to
								// wake, write a chunk, and re-evaluate; TCP backpressure
								// then paces the rest naturally.
								auto sleep_ms = std::min(static_cast<long>(
								    (buf_secs - TARGET_BUF) * 1000.0f), 2000L);
								auto deadline = std::chrono::steady_clock::now()
								              + std::chrono::milliseconds(sleep_ms);
								while (std::chrono::steady_clock::now() < deadline) {
									std::this_thread::sleep_for(
									    std::chrono::milliseconds(100));
									float pos_now = get_position();
									if (pos_now < CAST_POS_BUFFERING) return false;
									}
								}
							}
						} else if (is_browser && bytes_sent > prebuf_bytes) {
						// Browser (non-cast): use wall-clock elapsed time since this
						// request started as a proxy for the browser's playback position.
						// For a Range request starting at byte `offset`, the estimated
						// absolute position is offset/bps + elapsed.  No cancellation
						// check is needed (no get_position); the 2 s sleep cap keeps
						// the server responsive to any next Range request.
						float elapsed = std::chrono::duration<float>(
						    std::chrono::steady_clock::now() - t_start).count();
						float estimated_pos  = static_cast<float>(offset) / bytes_per_sec
						                     + elapsed;
						float audio_sent_abs = static_cast<float>(offset + bytes_sent)
						                     / bytes_per_sec;
						float buf_secs       = audio_sent_abs - estimated_pos;
						if (buf_secs > TARGET_BUF) {
							auto sleep_ms = std::min(static_cast<long>(
							    (buf_secs - TARGET_BUF) * 1000.0f), 2000L);
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(sleep_ms));
							}
						}
					}
				}
			return true;
			});
	}

std::vector<std::string> Streamer::ffmpeg_argv(const SongInfo& song,
                                               int target_bitrate,
                                               const Target& target,
                                               int time_offset,
                                               const std::string& out)
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
	// Drop attached pictures (m4a album art is exposed as a video stream)
	// and source metadata.  Without -vn, ffmpeg copies embedded JPEG/PNG
	// art into the mp3's ID3v2 tag at the *start* of the stream — Firefox
	// must download the whole tag before reaching the first audio frame,
	// which delays playback by seconds for tracks with large art.
	args.push_back("-vn");
	args.push_back("-map_metadata");
	args.push_back("-1");
	args.push_back("-f");
	args.push_back(std::string(target.muxer));
	if (target_bitrate > 0) {
		// Name the encoder rather than trusting the muxer's default: the
		// default is build-dependent, and a silently different codec is
		// indistinguishable from a corrupt file at the client.
		args.push_back("-c:a");
		args.push_back(std::string(target.encoder));
		// -b:a is meaningless for flac/wav and ffmpeg warns about it.
		if (target.lossy) {
			args.push_back("-b:a");
			args.push_back(std::to_string(target_bitrate) + "k");
			}
		}
	else {
		// Seek only — copy audio without re-encoding.
		args.push_back("-c:a");
		args.push_back("copy");
		}
	// The MP4 family writes its index after the audio and so needs to seek
	// back in the output.  A cache file can be seeked; a pipe cannot, and
	// ffmpeg aborts rather than produce a headerless file.  A fragmented
	// layout is the only form that streams.
	if (out == "pipe:1" && (target.muxer == "ipod" || target.muxer == "mp4")) {
		args.push_back("-movflags");
		args.push_back("frag_keyframe+empty_moov");
		}
	args.push_back(out);
	return args;
	}

void Streamer::serve_transcoded(httplib::Response& res, const SongInfo& song,
                                int target_bitrate, const Target& target,
                                int time_offset, bool is_browser,
                                std::function<float()> get_position,
                                int64_t est_length)
	{
	auto args = ffmpeg_argv(song, target_bitrate, target, time_offset, "pipe:1");

	{
	std::string cmd;
	for (const auto& a : args) { cmd += ' '; cmd += a; }
	std::cout << stamp() << "stream: ffmpeg:" << cmd << std::endl;
	}

	auto proc = std::make_shared<reproc::process>();
	reproc::options opts;
	// Capture stderr to a temp file rather than discarding it.  A pipe would
	// deadlock (nothing drains it while the content provider is blocked on
	// stdout); tmpfile() is unlinked on close so there is nothing to clean up.
	// Without this, every ffmpeg failure reached the client as an empty body
	// with no trace in the log of why.
	auto errf = std::shared_ptr<FILE>(std::tmpfile(),
		[](FILE* f){ if (f) std::fclose(f); });
	if (errf) {
		opts.redirect.err.type = reproc::redirect::type::file_;
		opts.redirect.err.file = errf.get();
		}
	else
		opts.redirect.err.type = reproc::redirect::type::discard;

	auto ec = proc->start(args, opts);
	if (ec) {
		std::cout << stamp() << "stream: ffmpeg launch failed: "
		          << ec.message() << std::endl;
		res.status = 500;
		return;
		}

	// For copy mode use the source bitrate for throttling; otherwise the target.
	// Fall back to 1000 kbps when the database has no bitrate (e.g. FLAC files
	// stored without a bitrate tag) so the throttle always fires.
	const float  bps        = target_bitrate > 0
	    ? static_cast<float>(target_bitrate) * 125.0f
	    : (song.bitrate > 0
	           ? static_cast<float>(song.bitrate)
	           : 1000.0f) * 125.0f;
	std::cout << stamp() << "stream: bps=" << bps
	          << " (src_bitrate=" << song.bitrate << ")" << std::endl;
	// total_sent persists across repeated provider calls (one per chunk).
	auto total_sent = std::make_shared<size_t>(0);

	// Content-length is unknown for transcoded output.  Use the *chunked*
	// content-provider variant, not the plain unknown-length one — the
	// latter sets neither Content-Length nor Transfer-Encoding, leaving
	// the response close-delimited (HTTP/1.0-style framing).  Firefox
	// waits for connection close before starting playback in that mode,
	// so the user hears nothing until ffmpeg finishes the whole transcode.
	// Transfer-Encoding: chunked frames each write so the browser starts
	// decoding as bytes arrive.
	res.set_header("Accept-Ranges", "none");
	std::string mime(target.mime);
	// t_start is the wall-clock moment the first byte is sent to the browser.
	// The non-cast throttle branch uses it as a proxy for playback position.
	auto t_start = std::chrono::steady_clock::now();

	// One iteration of read-from-ffmpeg, write-to-client, throttle.  Factored
	// out so the chunked and known-length variants below share it exactly;
	// `cap` bounds what may be written this call, which is how the estimate
	// variant stops precisely on the length it promised.
	auto pump = [proc, bps, total_sent, t_start, is_browser,
	             get_position = std::move(get_position)]
	            (httplib::DataSink& sink, size_t cap) -> Pump {
			if (get_position && get_position() < CAST_POS_BUFFERING) {
				std::cout << stamp() << "stream: transcoded abort (gen mismatch or stop)" << std::endl;
				return Pump::Abort;
				}
			if (cap == 0) return Pump::Eof;
			uint8_t buf[65536];
			auto [n, err] = proc->read(reproc::stream::out, buf,
			                           std::min(cap, sizeof(buf)));
			if (n == 0 || err) {
				// Check err before n: reproc wraps negative C return values into
				// size_t, so err is the reliable EOF/error indicator.
				if (err && err != std::make_error_code(std::errc::broken_pipe))
					std::cout << stamp() << "stream: ffmpeg pipe error: "
					          << err.message() << std::endl;
				return Pump::Eof;
				}
			for (size_t off = 0; off < n; ) {
				if (get_position && get_position() < CAST_POS_BUFFERING) return Pump::Abort;
				size_t piece = std::min(WRITE_CHUNK, n - off);
				if (!sink.write(reinterpret_cast<char*>(buf + off), piece)) {
					std::cout << stamp() << "stream: transcoded write failed at "
					          << *total_sent << " bytes" << std::endl;
					return Pump::Abort;
					}
				off         += piece;
				*total_sent += piece;
				}
			if (get_position) {
				float pos = get_position();
				if (pos < CAST_POS_BUFFERING) return Pump::Abort;
				if (bps > 0) {
					float audio_sent = static_cast<float>(*total_sent) / bps;
					// During BUFFERING (pos=-1) treat effective position as 0 so
					// the throttle caps the pre-buffer at TARGET_BUF seconds.
					// serve_transcoded cannot rely on TCP backpressure alone the
					// way serve_direct can, because ffmpeg produces data faster
					// than real-time and would flood the receiver's buffers.
					float effective_pos = (pos >= 0.0f) ? pos : 0.0f;
					float buf_secs   = audio_sent - effective_pos;
					if (buf_secs > TARGET_BUF) {
						// Cap single-sleep at 2 s — see serve_direct comment.
						auto sleep_ms = std::min(static_cast<long>(
						    (buf_secs - TARGET_BUF) * 1000.0f), 2000L);
						auto deadline = std::chrono::steady_clock::now()
						              + std::chrono::milliseconds(sleep_ms);
						while (std::chrono::steady_clock::now() < deadline) {
							std::this_thread::sleep_for(
							    std::chrono::milliseconds(100));
							if (get_position() < CAST_POS_BUFFERING) return Pump::Abort;
							}
						}
					}
				} else if (is_browser && bps > 0) {
				// Browser (non-cast): use wall-clock elapsed time as a proxy for
				// playback position, same approach as serve_direct.
				float elapsed    = std::chrono::duration<float>(
				    std::chrono::steady_clock::now() - t_start).count();
				float audio_sent = static_cast<float>(*total_sent) / bps;
				float buf_secs   = audio_sent - elapsed;
				if (buf_secs > TARGET_BUF) {
					auto sleep_ms = std::min(static_cast<long>(
					    (buf_secs - TARGET_BUF) * 1000.0f), 2000L);
					std::this_thread::sleep_for(
					    std::chrono::milliseconds(sleep_ms));
					}
				}
			return Pump::More;
			};

	auto releaser = [proc, total_sent, errf](bool success) {
			if (!success) proc->kill();
			auto [status, ec] = proc->wait(reproc::infinite);
			std::cout << stamp() << "stream: ffmpeg exit status=" << status
			          << " total=" << *total_sent << " bytes"
			          << (ec ? " (" + ec.message() + ")" : "") << std::endl;
			// Only on a real failure: a killed-on-seek ffmpeg exits non-zero
			// as a matter of course and its stderr is noise.
			if (success && status != 0) {
				auto tail = stderr_tail(errf.get());
				if (!tail.empty())
					std::cout << stamp() << "stream: ffmpeg stderr: "
					          << tail << std::endl;
				}
			};

	if (est_length > 0) {
		// estimateContentLength: the client asked for a Content-Length on a
		// stream whose true size is not knowable until ffmpeg finishes.  The
		// estimate becomes a contract — a client promised N bytes and given
		// fewer treats the connection as broken — so an overrun is truncated
		// and an underrun is zero-padded.  Trailing zeros fail MP3 frame-sync
		// and Ogg page-sync, so decoders discard them rather than play noise.
		//
		// Note this is the *known-length* provider, not the chunked one: a
		// Content-Length alongside Transfer-Encoding: chunked is malformed
		// (RFC 9112 6.1) and recipients are required to ignore the length.
		std::cout << stamp() << "stream: estimateContentLength=" << est_length
		          << std::endl;
		auto padded = std::make_shared<bool>(false);
		res.set_content_provider(
			static_cast<size_t>(est_length), mime,
			[pump, est_length, padded](size_t offset, size_t /*length*/,
			                            httplib::DataSink& sink) {
				size_t remaining = static_cast<size_t>(est_length) - offset;
				if (remaining == 0) return true;
				switch (pump(sink, remaining)) {
					case Pump::Abort: return false;
					case Pump::More:  return true;
					case Pump::Eof:   break;
					}
				if (!*padded) {
					std::cout << stamp() << "stream: ffmpeg ended "
					          << remaining << " bytes short of the estimate, "
					          << "padding" << std::endl;
					*padded = true;
					}
				static const char zeros[8192] = {};
				return sink.write(zeros, std::min(remaining, sizeof(zeros)));
				},
			releaser);
		return;
		}

	res.set_chunked_content_provider(
		mime,
		[pump](size_t /*offset*/, httplib::DataSink& sink) {
			switch (pump(sink, SIZE_MAX)) {
				case Pump::More:  return true;
				case Pump::Abort: return false;
				case Pump::Eof:   break;
				}
			sink.done();
			return false;
			},
		releaser);
	}

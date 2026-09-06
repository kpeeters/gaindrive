#include "streamer.hh"
#include "stamp.hh"
#include "dvd.hh"
#include "proc.hh"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <vector>
#include <string>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <reproc++/reproc.hpp>

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

Streamer::TranscodePlan Streamer::plan_transcode(const SongInfo& song,
                                                 const std::string& format,
                                                 int max_bitrate,
                                                 int time_offset)
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

	// A video's soundtrack already in a codec the requested container can hold
	// is a remux, not an encode — see audio_copy_target() in codecs.hh for
	// what that saves.  Two things about the shape of this test:
	//
	//  * It is expressible from `format` alone, and has to be.  A Chromecast
	//    fetches stream.view for itself and runs this function again; if it
	//    reached a different plan it would build a second cache entry under a
	//    different key while the one cast_load_song() warmed sat unused.  So
	//    the *decision* is which format goes on the URL, and this only agrees
	//    with it.
	//  * max_bitrate == 0 is a guard rather than an accident.  songs.bitrate on
	//    a video is the *container's* rate, so it cannot honestly be compared
	//    against an audio ceiling; a client that named one gets the encode.
	std::optional<Target> copy_t;
	if (song.is_video) copy_t = audio_copy_target(song.audio_codec);
	bool copy_audio = wanted && copy_t && max_bitrate == 0
	               && copy_t->name == wanted->name;

	TranscodePlan plan;
	plan.needed = time_offset > 0 || format_change || bitrate_limit;

	// When only a time-offset seek is needed (no format conversion, no bitrate
	// limit) preserve the original codec via ffmpeg -c:a copy so there is no
	// quality loss.  bitrate == 0 signals copy mode to serve_transcoded, and is
	// what the soundtrack copy above rides on too.
	if (copy_audio) {
		plan.target  = wanted;
		plan.bitrate = 0;
		}
	else if (format_change) {
		plan.target  = wanted;
		plan.bitrate = (max_bitrate > 0 && max_bitrate < 320) ? max_bitrate : 320;
		}
	else if (bitrate_limit) {
		plan.target  = target_for("mp3");
		plan.bitrate = max_bitrate;
		}
	else {
		plan.target  = source;
		plan.bitrate = 0;
		}

	// No table entry means no ffmpeg muxer for this container.  Passing the
	// extension through as -f made ffmpeg exit before writing a byte, which the
	// client saw as a 200 with an empty body.  Serving the raw file instead
	// ignores the seek, which is a far better failure than silence.
	if (plan.needed && !plan.target) {
		std::cout << stamp() << "stream: no muxer for codec '" << song.codec
		          << "', serving raw" << std::endl;
		plan.needed = false;
		}
	return plan;
	}

std::shared_ptr<const TranscodeCache::Entry>
Streamer::cache_entry(const SongInfo& song, TranscodeCache& cache,
                      const TranscodePlan& plan)
	{
	if (!plan.needed || !plan.target) return {};
	// The mtime in the key is what invalidates a stale transcode after the
	// source file is re-tagged or replaced.
	std::string key = std::to_string(song.id) + "-"
	                + std::to_string(song.file_modified) + "-"
	                + std::string(plan.target->name)
	                + std::to_string(plan.bitrate);
	static const std::string OUT = TranscodeCache::OUT_PLACEHOLDER;
	auto argv = ffmpeg_argv(song, plan.bitrate, *plan.target, 0, OUT);
	return cache.get_or_build(key, std::string(plan.target->ext), argv, OUT);
	}

void Streamer::serve(const httplib::Request& req, httplib::Response& res,
                     const SongInfo& song, TranscodeCache& cache,
                     int max_bitrate,
                     const std::string& format, int time_offset,
                     bool cast_stream, std::function<float()> get_position,
                     bool estimate_length,
                     const std::string& video_size, int segment_duration)
	{
	// "Paced" means deliver at roughly 1x playback rate rather than as fast as
	// the socket takes it.  Two clients want that and they ask in different
	// ways, which is why this is an OR and not one test:
	//
	//  * a browser, recognised by its User-Agent.  The web player's <audio>
	//    element is the original reason the throttle exists.
	//  * anything else, by putting pace=true on the URL.  That is how the
	//    Android app marks a URL it is about to hand to a Cast receiver on the
	//    direct route, where the receiver fetches from us for itself.
	//
	// Opt-in rather than sniffed, because a receiver cannot be recognised: a
	// WiiM sends no Mozilla/, and — fetching a URL the app built with ordinary
	// u/t/s credentials — no castToken either, so it is indistinguishable from
	// a third-party Subsonic client, which wants the opposite treatment.  What
	// goes wrong without it is set out in serve_direct().
	auto pace_it = req.params.find("pace");
	bool pace = req.get_header_value("User-Agent").find("Mozilla/")
	                != std::string::npos
	         || (pace_it != req.params.end() && pace_it->second == "true");

	// A video whose request names an audio format is a request for its
	// soundtrack alone, and falling through to the audio path below is the
	// entire implementation of that — see audio_only_request() in codecs.hh.
	// It works because target_for() has no entry for a video container, so
	// `wanted` always counts as a format change and the encode branch is
	// taken; the `-vn` that path already passes is the extraction.
	bool audio_only = audio_only_request(song.is_video, format,
	                                     song.audio_codec);

	// Video otherwise takes a different ladder entirely — see VIDEO.md.  None
	// of the negotiation below applies to it: TARGETS has no entry for a video
	// container, and format/maxBitRate here are about audio muxers.
	if (song.is_video && !audio_only) {
		serve_video(req, res, song, cache, max_bitrate, format, time_offset,
		            video_size, segment_duration, std::move(get_position));
		return;
		}

	const TranscodePlan plan = plan_transcode(song, format, max_bitrate,
	                                          time_offset);
	bool                  needs_transcode = plan.needed;
	const auto&           target          = plan.target;
	const int             target_bitrate  = plan.bitrate;

	// The User-Agent is logged because the process that fetches a stream is
	// very often not the one that asked for it — a Cast receiver fetches for
	// itself — and nothing else in the log records what it is.  Behind a
	// reverse proxy the address does not answer that either: client_addr() in
	// gaindrive.cc reports X-Forwarded-For when there is one and the proxy's
	// own address when there is not, and "which of those is this" was the
	// first thing that had to be guessed the last time a cast stream died.
	// Range for the same reason: a metadata probe and a playback fetch are the
	// same URL, and this header is the only thing telling them apart.  Both are
	// bracketed so an empty or space-carrying value stays readable.
	std::string ua    = req.get_header_value("User-Agent");
	std::string range = req.get_header_value("Range");

	std::cout << stamp() << "stream ["
	          << song.path << "] codec=" << song.codec
	          << " src_bitrate=" << song.bitrate
	          << " max_bitrate=" << max_bitrate
	          << " format=" << (format.empty() ? "(none)" : format)
	          << " time_offset=" << time_offset
	          << " transcode=" << (needs_transcode ? "yes" : "no")
	          << (needs_transcode ? " target=" + std::string(target->name) : "")
	          << (audio_only ? " audio_only=yes" : "")
	          << " cast=" << (cast_stream ? "yes" : "no")
	          << " pace=" << (pace ? "yes" : "no")
	          << " range=[" << (range.empty() ? "none" : range) << "]"
	          << " ua=[" << (ua.empty() ? "none" : ua) << "]"
	          << std::endl;

	if (!needs_transcode) {
		serve_direct(req, res, song, pace, std::move(get_position));
		return;
		}

	// A *server-driven* cast reaches the cache for exactly one kind of request:
	// a video's soundtrack.  Every other cast URL castLoad builds carries no
	// format and no maxBitRate, and its time_offset is always 0 under native
	// seek, so needs_transcode is already false and this branch is not reached.
	//
	// **The audio_only exception is only sound because cast_load_song() warms
	// the entry before the receiver is told anything** — see the `prepare` hook
	// on CastManager::LoadRequest.  A cold entry answers nothing until ffmpeg
	// has finished, which for a film's soundtrack is minutes; a receiver drops
	// the session after ~60 s with no data on the HTTP body, which surfaces as
	// error 103 and reads as a metadata bug.  Remove the warm and this line
	// silently becomes that bug.  With the warm in place the lookup here is a
	// hit, and serving a real file is what gives the receiver a Content-Length
	// and a XING header — without which a piped MP3 of a two-hour film is the
	// dur=0 saga in CLAUDE.md all over again.
	//
	// It does **not** cover the Android app's direct route, and that is worth
	// knowing before trusting this guard.  The app runs its own cast session,
	// so its URLs carry no castToken and cast_stream is false here; and it does
	// send format+maxBitRate whenever castOriginal is off, the item is a
	// video's soundtrack, or the type is not one the receiver plays.  Such a
	// request goes through the cache like any other, and a cold entry answers
	// nothing until ffmpeg has finished — the same >60 s of silence, arriving
	// before the first byte, where pacing cannot help.  The remedy if it ever
	// bites is CastUrls.warmTranscode for audio, which already exists on the
	// video side for exactly this reason.
	//
	// A seek is not cached either: the web client's local-seek trick needs the
	// stream to *start* at the offset, and caching one file per offset has no
	// bound.
	if ((!cast_stream || audio_only) && time_offset == 0) {
		if (auto entry = cache_entry(song, cache, plan)) {
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
			serve_direct(req, res, cached, pace,
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

	// For copy mode use the source bitrate for throttling; otherwise the target.
	// Fall back to 1000 kbps when the database has no bitrate (e.g. FLAC files
	// stored without a bitrate tag) so the throttle always fires.
	const float bps = target_bitrate > 0
	    ? static_cast<float>(target_bitrate) * 125.0f
	    : (song.bitrate > 0 ? static_cast<float>(song.bitrate) : 1000.0f)
	      * 125.0f;

	serve_transcoded(res, ffmpeg_argv(song, target_bitrate, *target,
	                                  time_offset, "pipe:1"),
	                 std::string(target->mime), bps,
	                 pace, std::move(get_position), est_length);
	}

void Streamer::serve_raw(const httplib::Request& req, httplib::Response& res,
                         const SongInfo& song)
	{
	// pace=false deliberately, whatever the User-Agent says and whatever the
	// query string asks for.  The throttle exists to keep a *playback*
	// connection alive; download.view is defined as the original media data,
	// and a paced download of a forty-minute FLAC would take forty minutes.
	serve_direct(req, res, song, false, {});
	}

void Streamer::serve_direct(const httplib::Request& req, httplib::Response& res,
                            const SongInfo& song, bool pace,
                            std::function<float()> get_position,
                            std::shared_ptr<const TranscodeCache::Entry> keepalive)
	{
	// Delivery rate.  Three cases, and the third is the one that bit.
	//
	// - Cast with a position callback: send the first 30 s at full speed, then
	//   throttle to stay at most TARGET_BUF seconds ahead of the receiver's
	//   *reported* playback position.
	// - Paced without one: the same throttle, with wall-clock elapsed time
	//   standing in for a position we cannot observe.  Asked for by the web
	//   player — whose <audio> element would otherwise evict and re-fetch
	//   buffered content, the root cause of intermittent MEDIA_ERR_NETWORK —
	//   and by a Cast receiver the Android app has pointed straight at this
	//   server on its direct route.
	// - Everything else — download.view, pinning, ExoPlayer, third-party
	//   Subsonic clients: no throttle at all.  Nothing is playing off the
	//   connection in real time and they want the bytes as fast as the socket
	//   takes them.
	//
	// Why a Cast receiver has to be paced even though it is not a browser, and
	// why TCP backpressure is not enough on its own.  A receiver reads at 1x
	// and stops reading once its buffer is full.  Unthrottled we fill the
	// kernel send buffer and the receiver's window within seconds — 21.8 MB,
	// 208 s of a 261 s FLAC, measured against a WiiM — and then block inside
	// one sink.write with nothing left to do.  From that moment the connection
	// carries no bytes at all, and things on the path are counting: a
	// Chromecast-built-in receiver gives up after ~60 s with no data on the
	// HTTP body, and Apache's ProxyTimeout defaults to Timeout's 60 s.
	// Whichever fires first tears the connection down mid-track, and the music
	// stops half a minute later when the buffer runs dry.  It is never our own
	// write timeout: that is an hour (gaindrive.cc).
	//
	// So the point of pacing here is not politeness towards the receiver's
	// buffer.  It is that the connection is never idle: the 2 s sleep cap
	// below means the longest silence this code can produce is two seconds,
	// comfortably inside every 60 s timer on the path.
	//
	// Pacing was unconditional for non-cast streams until 81e428b gated it on
	// the User-Agent, which is what put a receiver in the third bucket.  The
	// gate was right — a download must not be paced — but the discriminator
	// was wrong, because a User-Agent cannot tell a receiver from a Subsonic
	// client.  It is now `pace`, set by whoever hands the URL to something
	// that will read it at 1x, which is knowledge only the client has.
	//
	// A stored bitrate of 0 would switch the throttle off entirely and make
	// `pace` silently do nothing, which is a far worse failure than a slightly
	// wrong rate: size/duration is exact for CBR and close enough for VBR.
	// (The cast metadata probe in gaindrive.cc clamps file_size to 32768 while
	// keeping the real duration, so its derived rate is nonsense — harmless,
	// because that request is neither paced nor position-driven.)
	const float  bytes_per_sec = (song.bitrate > 0)
	    ? static_cast<float>(song.bitrate) * 125.0f
	    : (song.duration > 0
	       ? static_cast<float>(song.file_size) / static_cast<float>(song.duration)
	       : 0.0f);
	const size_t prebuf_bytes = bytes_per_sec > 0
	    ? static_cast<size_t>(bytes_per_sec) * 30   // 30 s at 1× rate
	    : 0;
	// httplib parses the Range header and calls our provider with the correct
	// offset/length when a known content-length is supplied.
	res.set_content_provider(
		static_cast<size_t>(song.file_size),
		std::string(codec_to_mime(song.codec)),
		[path = song.path, bytes_per_sec, prebuf_bytes, pace,
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
						// errno first, before anything below can overwrite it.
						// sink.write bottoms out in send(2), so this is usually
						// the real reason the far end went away: EPIPE or
						// ECONNRESET when it closed, ETIMEDOUT when the kernel
						// gave up retransmitting into a window that never
						// reopened — the signature of a client that stopped
						// reading and was then killed by somebody's idle timer.
						// The elapsed time says whether we died at the far end's
						// timeout or at our own; they differ by an hour.
						int   err  = errno;
						float secs = std::chrono::duration<float>(
						    std::chrono::steady_clock::now() - t_start).count();
						std::cout << stamp()
						          << (get_position ? "cast " : "") << "stream: write failed at "
						          << (offset + length - remaining + woff + piece)
						          << " bytes (range offset=" << offset << ")"
						          << " after " << secs << "s"
						          << " errno=" << err << " (" << std::strerror(err) << ")"
						          << std::endl;
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
						} else if (pace && bytes_sent > prebuf_bytes) {
						// No position to compare against, so wall-clock elapsed time
						// since this request started stands in for one.  For a Range
						// request starting at byte `offset` the estimated absolute
						// position is offset/bps + elapsed: exactly right for a player
						// that started there and has been playing since, optimistic for
						// one that has not started yet — so we send too much, never too
						// little.  That is the right direction to be wrong in; sending
						// early costs buffer, sending late costs a dropout.
						//
						// No cancellation check is needed (no get_position), and the
						// 2 s sleep cap serves both readers of this branch: the
						// browser's next Range request is answered promptly, and the
						// connection is never silent long enough for a receiver or a
						// reverse proxy to declare it dead.
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
	// A DVD titleset is split across 1 GB VOBs that are one continuous stream,
	// so its soundtrack is the concatenated list rather than the first part —
	// otherwise the extracted audio reports and stops after twenty minutes.
	// dvd_input() returns the plain path for everything else, so this is a
	// no-op for audio files.
	args.push_back(song.is_video ? dvd_input(song.path) : song.path);
	// Drop attached pictures (m4a album art is exposed as a video stream)
	// and source metadata.  Without -vn, ffmpeg copies embedded JPEG/PNG
	// art into the mp3's ID3v2 tag at the *start* of the stream — Firefox
	// must download the whole tag before reaching the first audio frame,
	// which delays playback by seconds for tracks with large art.
	//
	// On a video source the same flag is the audio extraction itself, which is
	// why the audio-only path needed no separate builder.
	args.push_back("-vn");
	if (song.is_video) {
		// Name the track rather than leaving it to ffmpeg's "best stream"
		// rule, which scores by channel count and would prefer a 5.1
		// commentary or a DTS track over the film's own stereo mix.  No
		// trailing '?': audio_only_request() has already excluded the silent
		// video, and a hard failure beats an empty file entering the cache.
		args.push_back("-map");
		args.push_back("0:a:0");
		// Downmix, as the video re-encode tier already does.  A film's 5.1
		// AC3 or DTS track encoded as surround costs several times the
		// bitrate the audio-quality setting is asking for, and phones and
		// browsers play the stereo mix regardless.
		//
		// Only when something is being encoded.  -ac is an encoder option and
		// a stream copy has no encoder to give it to — ffmpeg accepts it and
		// ignores it, so leaving it here would only make the argv claim a
		// downmix that is not happening.  Nothing is lost by that: decoding a
		// multichannel track purely to fold it down is the entire cost the
		// copy exists to avoid, and a receiver decoding it downmixes to
		// whatever its output actually has.
		if (target_bitrate > 0) {
			args.push_back("-ac");
			args.push_back("2");
			}
		}
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

// ---- Video -----------------------------------------------------------

std::vector<std::string> Streamer::video_ffmpeg_argv(
	const SongInfo& song, bool copy, int max_bitrate,
	const std::string& size, int time_offset, int segment_duration,
	bool mpegts, const std::string& out)
	{
	std::vector<std::string> a;
	a.push_back("ffmpeg");
	if (time_offset > 0) {
		// -ss before -i is the fast input seek, and lands on a keyframe.  That
		// makes HLS segment boundaries drift slightly; Subsonic lives with the
		// same thing rather than pay for output-accurate seeking.
		a.push_back("-ss");
		a.push_back(std::to_string(time_offset));
		}
	a.push_back("-i");
	// A DVD titleset is split across 1 GB VOBs that are one continuous stream,
	// so the input is a concat: list rather than a single file.  dvd_input()
	// returns the plain path for everything else, including a stray .vob that
	// is not part of a titleset.  ffmpeg's concat protocol seeks across the
	// joined files, so the -ss above still lands where it should.
	a.push_back(dvd_input(song.path));
	if (segment_duration > 0) {
		a.push_back("-t");
		a.push_back(std::to_string(segment_duration));
		}
	// First video and first audio stream only.  The trailing '?' makes the
	// audio mapping optional so a silent video still produces output instead
	// of ffmpeg exiting with "Stream map matches no streams".  Subtitles are
	// dropped; they are served separately through getCaptions.
	a.push_back("-map");
	a.push_back("0:v:0");
	a.push_back("-map");
	a.push_back("0:a:0?");
	a.push_back("-map_metadata");
	a.push_back("-1");

	if (copy) {
		a.push_back("-c");
		a.push_back("copy");
		}
	else {
		a.push_back("-c:v");
		a.push_back("libx264");
		a.push_back("-preset");
		a.push_back("veryfast");
		a.push_back("-crf");
		a.push_back("23");
		// yadif=deint=interlaced only touches frames the decoder flagged as
		// interlaced, so this can be unconditional — no stored per-file flag,
		// and progressive material passes through untouched.
		std::string vf = "yadif=deint=interlaced";
		if (!size.empty()) {
			auto x = size.find('x');
			if (x != std::string::npos)
				vf += ",scale=" + size.substr(0, x) + ":" + size.substr(x + 1)
				    + ":force_original_aspect_ratio=decrease";
			}
		a.push_back("-vf");
		a.push_back(vf);
		if (max_bitrate > 0) {
			// Leave room for the audio track inside the requested ceiling; a
			// client that asked for 2000 kbps means the whole stream.
			int vkbps = std::max(200, max_bitrate - 128);
			a.push_back("-maxrate");
			a.push_back(std::to_string(vkbps) + "k");
			a.push_back("-bufsize");
			a.push_back(std::to_string(vkbps * 2) + "k");
			}
		a.push_back("-c:a");
		a.push_back("aac");
		a.push_back("-b:a");
		a.push_back("128k");
		a.push_back("-ac");
		a.push_back("2");
		// Keeps audio aligned with video after a keyframe seek, which is the
		// same reason Subsonic passes it.
		a.push_back("-async");
		a.push_back("1");
		}

	// An HLS segment must carry the timestamps its position in the playlist
	// claims.  -ss before -i rebases the output, so without this every segment
	// starts at PTS 0 while the playlist says segment 190 belongs at 1900s.  A
	// player seeds its timestamp adjuster from the first segment it loads and
	// reuses it for the rest, so the second segment maps to the same instant as
	// the first and the timeline simply stops advancing — the stream stalls
	// with no error, because bytes are still arriving and nothing has failed.
	//
	// NOT -copyts, which is the more accurate answer: it keeps the input's own
	// timestamps, so a keyframe at 1897 stays at 1897 rather than being labelled
	// 1900.  But -t is measured from zero unless -start_at_zero is also given,
	// so with absolute timestamps ffmpeg sees a first packet far beyond the
	// requested 10 seconds and writes an empty segment.  -output_ts_offset is
	// applied by the muxer, after -t has already done its work.
	//
	// Segments only.  A bare timeOffset with no duration is the progressive
	// tier-2 path, where web/app.js adds player.localOffset back itself and
	// therefore needs the stream to start at zero.
	if (mpegts && time_offset > 0) {
		a.push_back("-output_ts_offset");
		a.push_back(std::to_string(time_offset));
		}

	a.push_back("-f");
	if (mpegts)
		a.push_back("mpegts");
	else {
		a.push_back("mp4");
		// The layout depends on the destination, exactly as it does in the
		// audio builder above.
		//
		// A normal moov atom is written after the media and needs a seek back
		// to the start of the output.  A pipe cannot do that and ffmpeg aborts
		// rather than emit a headerless file, so the fragmented layout is the
		// only form that streams there.  For a cache file it is the wrong
		// choice: the remux tier exists to produce a *seekable file with a
		// real index*, and an empty moov is precisely the weaker form of that.
		//
		// The option is spelled `default_base_moof`.  "default-base-is-moof"
		// is the ISO BMFF field name and appears only in ffmpeg's description
		// of the option; using it as the value makes ffmpeg reject the whole
		// movflags argument and exit before writing a byte.
		// A file gets NO movflags at all, deliberately.  ffmpeg then writes a
		// normal MP4 with the moov at the end, in a single pass, and that is
		// fully seekable over HTTP: the response carries a Content-Length and
		// honours Range, so the client fetches the tail once to find the index
		// and then seeks freely.  +faststart would move the index to the front
		// but rewrites the whole output to do it, doubling the I/O of a
		// multi-gigabyte remux to save exactly one request.
		if (out == "pipe:1") {
			a.push_back("-movflags");
			a.push_back("frag_keyframe+empty_moov+default_base_moof");
			}
		}
	a.push_back(out);
	return a;
	}

void Streamer::serve_video(const httplib::Request& req, httplib::Response& res,
                           const SongInfo& song, TranscodeCache& cache,
                           int max_bitrate, const std::string& format,
                           int time_offset, const std::string& video_size,
                           int segment_duration,
                           std::function<float()> get_position)
	{
	// Same predicate the API uses to tell the client whether it may seek
	// natively — see video_seeks_natively() in codecs.hh for why the two must
	// not drift apart.
	bool codecs_ok = video_seeks_natively(song.video_codec, song.audio_codec);
	// Anything that changes the picture or bounds the output forces a real
	// encode; a seek or a segment forces the pipe because neither a raw file
	// nor a whole-file remux can start partway in.
	bool constrained = !video_size.empty() || max_bitrate > 0
	                || (!format.empty() && format != "raw");
	bool partial     = time_offset > 0 || segment_duration > 0;

	// A VP9/Opus .mkv is a WebM file wearing the wrong extension: it can be
	// served untouched, but only once relabelled (see the Direct branch).
	bool relabel_webm = song.codec == "mkv"
	                 && webm_codecs(song.video_codec, song.audio_codec);

	enum class Tier { Direct, Remux, Encode };
	Tier tier = Tier::Encode;
	if (codecs_ok && !constrained && !partial)
		tier = video_direct_playable(song.codec, song.video_codec,
		                             song.audio_codec)
		     ? Tier::Direct : Tier::Remux;
	const char* tier_name = tier == Tier::Direct ? "direct"
	                      : tier == Tier::Remux  ? "remux" : "encode";

	// Logged for the reason given on the audio line: a receiver fetching for
	// itself is not the client that asked, and nothing else records what it is.
	std::string ua = req.get_header_value("User-Agent");

	std::cout << stamp() << "video [" << song.path << "]"
	          << " v=" << (song.video_codec.empty() ? "?" : song.video_codec)
	          << " a=" << (song.audio_codec.empty() ? "?" : song.audio_codec)
	          << " " << song.width << "x" << song.height
	          << " tier=" << tier_name
	          << (video_size.empty() ? "" : " size=" + video_size)
	          << (max_bitrate > 0 ? " max=" + std::to_string(max_bitrate) : "")
	          << (time_offset > 0 ? " offset=" + std::to_string(time_offset) : "")
	          << (segment_duration > 0
	              ? " seg=" + std::to_string(segment_duration) : "")
	          << " ua=[" << (ua.empty() ? "none" : ua) << "]"
	          << std::endl;

	if (tier == Tier::Direct) {
		// The raw file, byte ranges and all, and never paced: TARGET_BUF is
		// 15 s of *audio* and would cap a 6 Mbps video at 15 s of buffer on a
		// link that could do far better.
		//
		// Note what that leaves.  A cast video has exactly the shape the audio
		// path was fixed for — a receiver that stops reading, a connection that
		// then goes idle past somebody's 60 s timeout — because serve_video
		// honours no pace= at any tier.  If a cast film ever dies about a
		// minute in, this is the first place to look, and the answer is a
		// video-sized pacing window, not the absence of one.
		//
		// A qualifying .mkv is relabelled video/webm on the way out: the bytes
		// are already a valid WebM stream, but browsers refuse
		// video/x-matroska on the MIME alone.  codec is only read here for
		// codec_to_mime(), so overriding the copy is enough.
		if (relabel_webm) {
			SongInfo as_webm = song;
			as_webm.codec = "webm";
			serve_direct(req, res, as_webm, false, std::move(get_position));
			return;
			}
		serve_direct(req, res, song, false, std::move(get_position));
		return;
		}

	if (tier == Tier::Remux) {
		// Worth materialising, unlike an encode: -c copy runs at disk speed,
		// the key is still id+mtime, and the result is a real MP4 with a
		// Content-Length that answers Range requests.
		std::string key = std::to_string(song.id) + "-"
		                + std::to_string(song.file_modified) + "-remux";
		static const std::string OUT = TranscodeCache::OUT_PLACEHOLDER;
		auto argv = video_ffmpeg_argv(song, true, 0, "", 0, 0, false, OUT);
		if (auto entry = cache.get_or_build(key, ".mp4", argv, OUT)) {
			SongInfo cached{ entry->path().string(), "mp4", song.bitrate,
			                 song.duration, entry->size(), song.id,
			                 song.file_modified };
			res.set_header("X-Gaindrive-Transcode",
			               entry->hit() ? "hit" : "miss");
			serve_direct(req, res, cached, false, std::move(get_position),
			             entry);
			return;
			}
		// Cache disabled, disk full or ffmpeg unhappy — fall through and pipe
		// it instead.  The user still sees the video.
		std::cout << stamp() << "video: remux cache unavailable, piping"
		          << std::endl;
		}

	// Chunked output has no length to range into, and a browser's automatic
	// "Range: bytes=0-" would otherwise be rejected as 416 by httplib's
	// post-handler range check.  Same reasoning as the audio path.
	const_cast<httplib::Request&>(req).ranges.clear();

	bool  copy = (tier == Tier::Remux);
	float bps  = (song.bitrate > 0 ? static_cast<float>(song.bitrate)
	                               : 2000.0f) * 125.0f;
	auto  argv = video_ffmpeg_argv(song, copy, max_bitrate, video_size,
	                               time_offset, segment_duration,
	                               segment_duration > 0, "pipe:1");
	std::string mime(segment_duration > 0 ? VIDEO_TS_MIME : VIDEO_MP4_MIME);

	// Unpaced for the same reason as Tier 0, and with the same residual risk
	// recorded there: the audio throttle's 15 s window starves a player that
	// wants to buffer a video.
	serve_transcoded(res, std::move(argv), mime, bps, false,
	                 std::move(get_position));
	}

void Streamer::serve_transcoded(httplib::Response& res,
                                std::vector<std::string> args,
                                const std::string& mime, float bps,
                                bool pace,
                                std::function<float()> get_position,
                                int64_t est_length)
	{
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

	std::cout << stamp() << "stream: bps=" << bps << std::endl;
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
	// t_start is the wall-clock moment the first byte is sent to the browser.
	// The non-cast throttle branch uses it as a proxy for playback position.
	auto t_start = std::chrono::steady_clock::now();

	// One iteration of read-from-ffmpeg, write-to-client, throttle.  Factored
	// out so the chunked and known-length variants below share it exactly;
	// `cap` bounds what may be written this call, which is how the estimate
	// variant stops precisely on the length it promised.
	auto pump = [proc, bps, total_sent, t_start, pace,
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
					// errno and the elapsed time for the same reason as in
					// serve_direct: they are what say whether the far end hung
					// up, or timed us out, or we timed ourselves out.
					int   sys_err = errno;
					float secs    = std::chrono::duration<float>(
					    std::chrono::steady_clock::now() - t_start).count();
					std::cout << stamp() << "stream: transcoded write failed at "
					          << *total_sent << " bytes"
					          << " after " << secs << "s"
					          << " errno=" << sys_err
					          << " (" << std::strerror(sys_err) << ")"
					          << std::endl;
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
				} else if (pace && bps > 0) {
				// The same wall-clock stand-in as serve_direct, and needed here
				// too: a direct cast of a transcoded format lands on this path
				// whenever the transcode cache cannot produce a file, and
				// ffmpeg fills the pipe far faster than real time.  Unpaced,
				// the receiver is flooded in seconds and the connection then
				// goes silent for exactly as long, and for exactly the reason,
				// set out in serve_direct.
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

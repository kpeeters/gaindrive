#pragma once

#include <array>
#include <functional>
#include <optional>
#include <set>
#include <string>
#include <string_view>

// Audio format table — the single source of truth for "what is this format
// called, and how do we ask ffmpeg for it".
//
// The `codec` column in the songs table holds a lowercased *file extension*,
// not a codec name (MediaStore derives it from the filename), so this table is
// keyed by both spellings: "m4a" and "aac" are separate entries resolving to
// different muxers for the same encoder.
//
// The distinction matters because `ffmpeg -f` takes a *muxer* name and there is
// no muxer called "m4a", "aac" or "wma".  Feeding the extension straight through
// makes ffmpeg exit before writing a byte, which reached the client as a 200
// with an empty body.  Anything built from this table passes `muxer`, never
// `name`.
struct Target
	{
	std::string_view name;     // lookup key: format name or file extension
	std::string_view muxer;    // ffmpeg -f
	std::string_view encoder;  // ffmpeg -c:a; empty => not a valid encode target
	std::string_view ext;      // cache-file extension, leading dot included
	std::string_view mime;
	bool             lossy;    // false => -b:a is meaningless, so don't pass it
	};

// Opus is muxed into a plain Ogg container rather than ffmpeg's dedicated
// "opus" muxer: the Ogg form is what clients reliably seek in (see the Navidrome
// report at github.com/navidrome/navidrome/issues/2563, where the dedicated
// muxer produced unseekable streams for Symfonium and Substreamer).
//
// wma has an empty encoder — ffmpeg can decode it, so it is a valid *source*
// for copy mode, but there is no free encoder and it must never be a target.
inline constexpr std::array<Target, 10> TARGETS = {{
	{ "opus",   "opus", "libopus",   ".opus", "audio/ogg",       true  },
	{ "ogg",    "ogg",  "libvorbis", ".ogg",  "audio/ogg",       true  },
	{ "oga",    "ogg",  "libvorbis", ".oga",  "audio/ogg",       true  },
	{ "vorbis", "ogg",  "libvorbis", ".ogg",  "audio/ogg",       true  },
	{ "mp3",    "mp3",  "libmp3lame",".mp3",  "audio/mpeg",      true  },
	{ "aac",    "adts", "aac",       ".aac",  "audio/aac",       true  },
	{ "m4a",    "ipod", "aac",       ".m4a",  "audio/mp4",       true  },
	{ "flac",   "flac", "flac",      ".flac", "audio/flac",      false },
	{ "wav",    "wav",  "pcm_s16le", ".wav",  "audio/wav",       false },
	{ "wma",    "asf",  "",          ".wma",  "audio/x-ms-wma",  true  },
	}};

// Looks up by format name or by file extension (they share a namespace here).
// Empty when the name is unknown.
inline std::optional<Target> target_for(std::string_view name)
	{
	for (const auto& t : TARGETS)
		if (t.name == name) return t;
	return std::nullopt;
	}

// Video source containers.  Deliberately a *separate* table rather than more
// rows in TARGETS: target_for() is what validates the `format` parameter of
// stream.view, and a video extension must never resolve there as an audio
// encode target.  These entries describe what a file *is*, not what ffmpeg can
// be asked to produce — the two video output formats (fragmented MP4 for
// progressive playback, MPEG-TS for HLS segments) are chosen by the streamer,
// never by the client naming an extension.
struct VideoTarget
	{
	std::string_view name;   // file extension
	std::string_view mime;
	};

inline constexpr std::array<VideoTarget, 10> VIDEO_TARGETS = {{
	{ "mkv",  "video/x-matroska" },
	{ "mp4",  "video/mp4"        },
	{ "m4v",  "video/mp4"        },
	{ "avi",  "video/x-msvideo"  },
	{ "mpg",  "video/mpeg"       },
	{ "mpeg", "video/mpeg"       },
	{ "mov",  "video/quicktime"  },
	{ "webm", "video/webm"       },
	{ "wmv",  "video/x-ms-asf"   },
	{ "vob",  "video/mpeg"       },
	}};

inline std::optional<VideoTarget> video_target_for(std::string_view ext)
	{
	for (const auto& t : VIDEO_TARGETS)
		if (t.name == ext) return t;
	return std::nullopt;
	}

inline bool is_video_ext(std::string_view ext)
	{
	return video_target_for(ext).has_value();
	}

// A container a *client* may declare it demuxes for itself — see
// video_direct_playable_for() below.  Every video container qualifies except
// vob, and that exclusion is not caution.
//
// A DVD titleset's songs.path names only the *first* VOB of a set that is one
// continuous stream split at 1 GB boundaries; dvd_input() is what hands ffmpeg
// the concat: list of the rest.  Served untouched, that is twenty minutes of a
// two-hour film — and nothing anywhere reports an error, because what goes out
// is a valid program stream that simply ends.
inline bool container_declarable(std::string_view ext)
	{
	return is_video_ext(ext) && ext != "vob";
	}

// songs.codec holds a lowercased extension for video rows too, so every
// existing caller of this function keeps working once the video table is
// consulted as a fallback.  Audio wins on a tie; there is no overlap today.
inline std::string_view codec_to_mime(std::string_view codec)
	{
	if (auto t = target_for(codec))       return t->mime;
	if (auto v = video_target_for(codec)) return v->mime;
	return std::string_view("application/octet-stream");
	}

// What a video transcode produces.  Tier 1 (remux) and Tier 2 (re-encode)
// share the fragmented-MP4 form; HLS segments use MPEG-TS.
inline constexpr std::string_view VIDEO_MP4_MIME = "video/mp4";
inline constexpr std::string_view VIDEO_TS_MIME  = "video/mp2t";

// Codecs a browser can be expected to decode without help, and containers it
// will accept them in.  The lists are deliberately conservative: being wrong
// in the permissive direction means a black player and a support question,
// while being wrong in the strict direction only costs a remux.
//
// These live here rather than in streamer.cc because two callers must agree on
// them: Streamer::serve_video() picks the tier, and the API advertises to the
// client whether the resulting stream can be seeked natively.  If those two
// ever disagreed, a client would seek into a stream that has no Range support
// and the seek would silently do nothing.
inline bool browser_video_codec(std::string_view c)
	{
	return c == "h264" || c == "vp8" || c == "vp9" || c == "av1";
	}

inline bool browser_audio_codec(std::string_view c)
	{
	return c == "aac" || c == "mp3" || c == "opus" || c == "vorbis"
	    || c == "flac";
	}

inline bool browser_container(std::string_view ext)
	{
	return ext == "mp4" || ext == "m4v" || ext == "webm";
	}

// True when the served stream will carry a Content-Length and answer Range
// requests, so the client can let the media element seek by itself.
//
// This is deliberately **not** "does ffmpeg run": the remux tier runs ffmpeg
// but writes a real file through the transcode cache, so it seeks just as well
// as the untouched original.  Only a re-encode is chunked and unseekable.
// Conflating the two would make a client send timeOffset for a remuxable MKV,
// which sets partial=true in serve_video() and demotes a cheap -c copy into a
// full re-encode — the exact opposite of what the tier ladder is for.
//
// An empty audio codec counts as playable: a silent video is fine, and a file
// the scanner could not probe at all is better handled by the fallbacks in
// serve_video() than by pessimising every request.
inline bool video_seeks_natively(std::string_view video_codec,
                                 std::string_view audio_codec)
	{
	return browser_video_codec(video_codec)
	    && (audio_codec.empty() || browser_audio_codec(audio_codec));
	}

// WebM is a profile of Matroska, so an .mkv carrying these codecs is a WebM
// file in all but name and every browser that plays WebM will decode it.
inline bool webm_codecs(std::string_view video_codec,
                        std::string_view audio_codec)
	{
	return (video_codec == "vp8" || video_codec == "vp9"
	        || video_codec == "av1")
	    && (audio_codec.empty() || audio_codec == "vorbis"
	        || audio_codec == "opus");
	}

// True when the file can go to a browser untouched.  Two callers must agree on
// this — serve_video()'s tier choice and the transcoded* fields the API
// advertises — for the same reason as video_seeks_natively() above.
//
// The .mkv case is not a technicality: a VP9/Opus Matroska (yt-dlp's usual
// output) would otherwise pay a whole-file remux to produce something it
// already is.  It must be *relabelled* video/webm when served, though —
// browsers reject video/x-matroska on the MIME alone, whatever the bytes hold.
inline bool video_direct_playable(std::string_view container,
                                  std::string_view video_codec,
                                  std::string_view audio_codec)
	{
	if (browser_container(container)
	        && video_seeks_natively(video_codec, audio_codec))
		return true;
	return container == "mkv" && webm_codecs(video_codec, audio_codec);
	}

// The containers one particular client declared it can demux for itself, on
// that client's own stream.view request.  Empty for everybody else.
//
// std::less<> so find() takes a string_view without allocating.
using ClientContainers = std::set<std::string, std::less<>>;

// The Direct tier for a client that declared containers of its own, which is a
// different question from video_direct_playable() and is deliberately a
// different function rather than a fourth defaulted argument.
//
// video_direct_playable() says two callers must agree on it: serve_video()'s
// tier choice and the transcoded* fields the API advertises.  That stays true
// only while those two ask the *same* question — and a defaulted argument is
// exactly how one of them quietly stops.  transcode_target() fills
// transcodedContentType/transcodedSuffix, which a client caches and which the
// Android app hands a Cast receiver as the LOAD's contentType; cast_tier_for()
// answers for a receiver, which declared nothing.  Neither may ever acquire
// this answer, and two names cannot drift because there is no argument to
// forget.
//
// **Only the container is widened.  video_seeks_natively() is untouched**, and
// that is the whole safety argument: the two tiers this moves a file between
// are both seekable, so nativeSeek is identical either side of it and no
// advertised field becomes client-dependent.  Widening the *codec* pair could
// not be done this way — nativeSeek is what a client picks its transport with,
// and what the Android app refuses to cast on.
//
// container_declarable() is re-checked here rather than trusted from whoever
// built the set, because the failure a declared vob produces is a film that
// stops after twenty minutes and says nothing.
inline bool video_direct_playable_for(std::string_view container,
                                      std::string_view video_codec,
                                      std::string_view audio_codec,
                                      const ClientContainers& client)
	{
	if (video_direct_playable(container, video_codec, audio_codec)) return true;
	if (!container_declarable(container))       return false;
	if (client.find(container) == client.end()) return false;
	return video_seeks_natively(video_codec, audio_codec);
	}

// Which of serve_video()'s three tiers a Chromecast's fetch will land on.
//
// A cast URL carries no format, no size, no maxBitRate and no timeOffset — the
// receiver seeks natively and the account ceiling is skipped for a cast token —
// so `constrained` and `partial` are both false in serve_video() and the tier
// follows from the codec pair alone.  That is what makes it answerable here,
// before a byte is served, which two things need: the LOAD message announces a
// contentType in advance, and castLoad/castSession report the tier to the
// client rather than letting it work the ladder out a second time.
//
// The third caller of the tier predicate, after serve_video() and
// transcode_target() — the same drift rule those two document applies here.
// Note it is written the way serve_video() writes it, on video_seeks_natively()
// *and* video_direct_playable(), rather than on the second alone: those two
// disagree exactly on the remux tier, which is a file whose codecs a browser
// takes in a container it does not, and collapsing them would report an H.264
// AVI as a re-encode when it is a -c copy.
enum class CastTier { Direct, Remux, Encode };

inline CastTier cast_tier_for(std::string_view container,
                              std::string_view video_codec,
                              std::string_view audio_codec)
	{
	// Audio is byte-ranged off disk: no format and no ceiling means
	// needs_transcode is false and serve() never reaches a transcode at all.
	// The film-soundtrack case is not this — it puts `format` on the URL, and
	// so is decided by cast_load_song() rather than by the codec pair.
	if (!is_video_ext(container)) return CastTier::Direct;
	if (!video_seeks_natively(video_codec, audio_codec))
		return CastTier::Encode;
	return video_direct_playable(container, video_codec, audio_codec)
	     ? CastTier::Direct : CastTier::Remux;
	}

inline std::string_view cast_tier_name(CastTier t)
	{
	return t == CastTier::Direct ? std::string_view("direct")
	     : t == CastTier::Remux  ? std::string_view("remux")
	                             : std::string_view("encode");
	}

// What a Chromecast will actually receive from stream.view, which is not the
// same thing as what the file is.
//
// It has to be computable in advance: the LOAD message announces a contentType
// before a byte is served, and a receiver told video/x-matroska while being
// sent MP4 refuses the media outright.  Every source the remux or encode tier
// touches arrives as MP4 whatever it started as.
//
// The .mkv relabel is the one case where the bytes go out untouched under a
// type that is not the container's own, and it is the case transcode_target()
// does not have to answer because it only reports *that* a transcode happens.
inline std::string_view cast_mime_for(std::string_view container,
                                      std::string_view video_codec,
                                      std::string_view audio_codec)
	{
	if (!is_video_ext(container)) return codec_to_mime(container);
	if (cast_tier_for(container, video_codec, audio_codec) != CastTier::Direct)
		return VIDEO_MP4_MIME;
	if (container == "mkv" && webm_codecs(video_codec, audio_codec))
		return std::string_view("video/webm");
	return codec_to_mime(container);
	}

// The format a server-driven cast asks for when the receiver cannot show a
// picture and the soundtrack cannot be copied out as it stands — see
// audio_copy_target() below, which is tried first and covers AAC, MP3, FLAC,
// Opus and Vorbis.  So this is reached only for AC3, DTS, TrueHD and PCM,
// which is to say most disc rips.
//
// **FLAC, because this is the one path that must re-encode.**  The source is
// already lossy, and encoding it again imposes a second generation of loss on
// the only route that had no choice about transcoding.  Everything that argued
// for mp3 here has since stopped applying:
//
//  * "every receiver takes mp3" — FLAC is in Google Cast's baseline audio
//    support (documented to 96 kHz / 24-bit; a film's track is 48 kHz), so it
//    no longer selects for the least capable device.
//  * "the extraction is a full transcode whatever it lands in, so nothing is
//    preserved by choosing a fancier one" — true before the copy tier existed
//    and false now.  This constant is what is left *after* copying has been
//    ruled out, which is exactly when the choice of encoder decides how much
//    is thrown away.
//  * "CBR mp3's length is predictable, which serve()'s estimate_length path
//    would need if the cache warm were given up" — a cast URL never sets
//    estimateContentLength, so that was hypothetical.  What it points at is
//    real and worth knowing: an unwarmed FLAC pipe carries no Content-Length
//    and no seek table, so a failed warm degrades further than it used to.
//    Not fatal, since the LOAD announces the duration itself.
//
// It also costs less time, not more: FLAC encodes several times faster than
// LAME, and on this path the wait before the LOAD is the whole problem.
inline constexpr const char* CAST_AUDIO_ONLY_FORMAT = "flac";

// The container to stream-copy a soundtrack into, keyed on the *codec* rather
// than on the source container.  nullopt means it has to be encoded.
//
// This is what makes a film's soundtrack cheap.  Extracting it is otherwise a
// demux plus a decode plus a LAME encode of a two-hour track, materialised in
// full before the receiver is told anything — minutes, on a path whose whole
// purpose is that the wait happens before the LOAD.  When the track is already
// in a codec the receiver decodes, `-c:a copy` reduces that to the demux.
//
// The codec set is the one browser_audio_codec() lists, and for the same
// reason — a Cast receiver is a Chrome media stack — but written out here
// rather than reused, because this has to name a container per codec and that
// predicate has no notion of one.  Everything else (AC3, DTS, TrueHD, PCM, so
// most DVD and Blu-ray rips) falls back to CAST_AUDIO_ONLY_FORMAT.
//
// The containers are chosen for what ffmpeg will actually mux the codec into
// with a seekable output: `ipod` writes a normal MP4 whose moov lands at the
// end, `mp3` still writes its Xing/Info header under -c:a copy, and both Ogg
// forms carry their own granule positions.  So a copied soundtrack is as
// seekable as an encoded one, which native cast seek needs.
inline std::optional<Target> audio_copy_target(std::string_view audio_codec)
	{
	std::string_view name;
	if      (audio_codec == "aac")    name = "m4a";
	else if (audio_codec == "mp3")    name = "mp3";
	else if (audio_codec == "flac")   name = "flac";
	else if (audio_codec == "opus")   name = "opus";
	else if (audio_codec == "vorbis") name = "ogg";
	else return std::nullopt;
	return target_for(name);
	}

// True when a request naming a video is really a request for its soundtrack.
//
// This is the whole audio-only contract, and it is deliberately spelled with
// existing Subsonic parameters rather than an extension: `format` already
// names an audio container, `target_for()` already validates it, and a client
// asking for format=mp3 on a video is asking for exactly this.  VIDEO.md
// specified it from the start — the unconditional `-vn` in
// Streamer::ffmpeg_argv() exists for m4a cover art, but on a video file it
// *is* audio extraction, so the audio path needed almost nothing added to it.
//
// Two callers must agree on this, for the same reason as the two predicates
// above: Streamer::serve() decides whether to take the video ladder at all,
// and the stream.view handler decides whether the account's maxBitRate ceiling
// applies — it must not for a video, and it must for the soundtrack of one.
//
// An empty audio_codec keeps the request on the video ladder.  A silent video
// has nothing to extract, and `-map 0:a:0` against one makes ffmpeg exit
// before writing a byte, which reaches the client as a 200 with an empty body
// — the failure the format validation in stream.view exists to prevent.
inline bool audio_only_request(bool is_video, std::string_view format,
                               std::string_view audio_codec)
	{
	if (!is_video || audio_codec.empty())  return false;
	if (format.empty() || format == "raw") return false;
	auto t = target_for(format);
	return t && !t->encoder.empty();
	}

// Which audio format a cast soundtrack is asked for, or empty when the film
// has nothing to extract — a silent video, or one the scanner could not probe.
//
// One definition because two callers must agree exactly: cast_load_song()
// decides *whether* the receiver gets the soundtrack, and soundtrack_load()
// builds the URL that asks for it.  A disagreement there is a LOAD announcing
// one content type while another arrives, which a receiver refuses outright.
//
// Copy first, encode second.  Everything about that choice is in
// audio_copy_target() and CAST_AUDIO_ONLY_FORMAT above.
inline std::string cast_soundtrack_format(std::string_view audio_codec)
	{
	auto t = audio_copy_target(audio_codec);
	std::string fmt = t ? std::string(t->name)
	                    : std::string(CAST_AUDIO_ONLY_FORMAT);
	return audio_only_request(true, fmt, audio_codec) ? fmt : std::string();
	}

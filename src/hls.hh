#pragma once

#include <condition_variable>
#include <cstddef>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "mkvcues.hh"
#include "streamer.hh"

// One rendition of a film: the constraints a client put on the picture, and the
// audio codecs beyond AAC it said it plays in fragmented MP4.  No constraint
// means "as close to the source as possible", which is the only case in which
// the video may be copied.
struct HlsVariant {
	int         kbps = 0;
	std::string size;
	// Canonical, from hls_audio_codecs(): so it can go into a URL and into a
	// session's identity as it is.
	std::string audio;
	bool constrained() const { return kbps > 0 || !size.empty(); }
	bool takes(const std::string& codec) const;
	};

// The audioCodecs parameter, reduced to the codecs it may name - opus and flac,
// the two besides AAC that ffmpeg copies into MP4 and browsers play from it -
// in a fixed order, comma-separated.  Anything else is dropped: the value is
// written into playlist bodies.
std::string hls_audio_codecs(const std::string& param);

// How a film is cut and what each segment is made of.  Segment k covers
// [bounds[k], bounds[k+1]).
struct HlsPlan {
	bool                copy_video = false;
	bool                copy_audio = false;
	// Matroska's timestamp unit when copying; the cut tolerance is half of it.
	double              tick       = 0.001;
	std::vector<double> bounds;

	size_t segments() const { return bounds.empty() ? 0 : bounds.size() - 1; }
	// A copied stream can be cut anywhere a keyframe is, so each segment is
	// its own ffmpeg run.  An encoded audio track cannot: every AAC encoder
	// starts with a frame of priming silence, which is the gap heard at every
	// boundary of the old per-segment HLS.  Those plans run one encoder per
	// viewer instead - see HlsSessions.
	bool   stateless() const { return copy_video && copy_audio; }
	};

// The server side of hls.m3u8: playlists, init segments and media segments,
// all fragmented MP4.
class Hls {
	public:
		// `work` holds the running sessions' segment files.  Wiped here, since
		// nothing in it survives a restart usefully.
		explicit Hls(std::filesystem::path work);
		~Hls();

		HlsPlan plan(const Streamer::SongInfo& song, const HlsVariant& v);

		// The init segment: ftyp and moov, no media.  Always from a short
		// run of its own, whichever way the segments are made; the codec
		// configuration is the same for every run of one plan.
		void serve_init(httplib::Response& res, const Streamer::SongInfo& song,
		                const HlsVariant& v);

		// Media segment k.  `owner` names the viewer; a session belongs to
		// one owner, and a new film for the same owner ends the old session.
		void serve_segment(httplib::Response& res, const Streamer::SongInfo& song,
		                   const HlsVariant& v, size_t k,
		                   const std::string& owner);

	private:
		struct Session;

		// The owner's session for this film, replacing one for another film.
		// Empty when too many are running.
		std::shared_ptr<Session> session_for(const Streamer::SongInfo& song,
		                                     const HlsVariant& v,
		                                     const HlsPlan& p,
		                                     const std::string& owner);
		void reap();

		std::filesystem::path                           work_;
		std::mutex                                      mu_;
		std::map<std::string, std::shared_ptr<Session>> sessions_;
		uint64_t                                        next_id_ = 0;

		std::condition_variable                         reaper_cv_;
		bool                                            stopping_ = false;
		std::thread                                     reaper_;

		// Keyframe indexes by song id and mtime; reading one is cheap, but
		// not per segment.
		std::mutex                                           cues_mu_;
		std::map<std::pair<int, int64_t>, MkvKeyframes>      cues_;
	};

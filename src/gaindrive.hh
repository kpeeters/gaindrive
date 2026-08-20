#pragma once

// Normally supplied by CMake from the root VERSION file, or from the git tag
// for a release build. The fallback only exists so the tree still compiles if
// it is built by hand outside CMake.
#ifndef GAINDRIVE_VERSION
#define GAINDRIVE_VERSION "0.0-dev"
#endif

#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <httplib.h>

#include "mediastore.hh"
#include "castmanager.hh"
#include "coverart.hh"
#include "folderwatcher.hh"
#include "transcodecache.hh"

class GainDrive {
	public:
		// transcode_cache_dir empty = derived from db_path.
		// transcode_cache_mb 0 = cache disabled.
		// video_art_px 0 = do not manufacture cover art for videos.
		// video_art_frames and video_art_embedded enable the two local tiers,
		// both off by default.
		GainDrive(const std::string& db_path,
		          const std::vector<MediaStore::Root>& roots,
		          const std::string& upload_dir,
		          bool no_scan,
		          bool debug = false,
		          bool flat_multi_disc = true,
		          const std::string& user_db_path = "",
		          const std::string& transcode_cache_dir = "",
		          int transcode_cache_mb = 1024,
		          int transcode_jobs = 0,
		          int video_art_px = 640,
		          bool video_art_frames = false,
		          bool video_art_embedded = false);
		~GainDrive();
		// Binds and serves. Returns false without serving if the port could
		// not be acquired, so the caller can exit non-zero rather than treat a
		// doomed start as a clean shutdown.
		bool listen(const std::string& host, int port);

	private:
		bool            debug_;
		bool            flat_multi_disc_;
		// Held from construction until listen() decides whether to scan. The
		// scan cannot start in the constructor: a server that fails to bind
		// must do no work and exit at once.
		bool            no_scan_ = false;
		std::string     upload_dir_;
		// Absolute path of the uploads root, or empty when none is configured
		// — in which case the upload endpoints refuse rather than writing into
		// a library root. Personal files live at <users_dir_>/<username>/.
		std::string     users_dir_;
		// Name of the uploads root, i.e. the first component of the stored
		// paths for anything under it. Empty when there is no uploads root.
		std::string     uploads_root_name_;
		MediaStore      store_;
		TranscodeCache  transcode_cache_;
		CoverArtCache   cover_cache_;
		CastManager     cast_manager_;
		std::string     last_cast_song_id_;
		float           last_cast_offset_ = 0.0f;

		// SSE-as-heartbeat: castEvents.view is the only long-lived browser
		// connection during a cast session, so its disappearance acts as a
		// "client gone" signal. When the listener count drops to 0 while a
		// cast is active, a detached watchdog thread waits CAST_IDLE_GRACE_S
		// before tearing the session down. cast_wd_gen_ is bumped on every
		// listener attach/detach so a stale watchdog from a previous
		// disconnect bails out when its generation no longer matches.
		std::atomic<int> cast_sse_listeners_{0};
		std::atomic<int> cast_wd_gen_{0};

		// Stop the cast session and clear the HTTP-layer cast state
		// (last_cast_song_id_, last_cast_offset_). Called both from the
		// stopCast endpoint and from the SSE watchdog thread.
		void cast_teardown();

		// ---- Artist portraits ----
		//
		// Resolving an artist means MusicBrainz, then Wikidata, then
		// Wikipedia, then TheAudioDB, then Discogs, with two deliberate
		// one-second pacing waits along the way. That ran inside the request
		// thread, so a client showing a grid of artists could occupy the whole
		// HTTP pool in network waits — and the bytes it fetched were kept only
		// in memory, so a restart did it all again.
		//
		// It now happens here instead: one background thread works through the
		// artists that have no portrait yet, writes both the metadata and the
		// image to the DB, and getCoverArt does nothing but read. A request
		// for an artist not yet resolved pushes it to the *front* of the
		// queue — what somebody is looking at beats alphabetical order — and
		// answers 404 straight away.
		//
		// Same shape as TMDB's Phase 3c, and for the same reason: a slow first
		// pass over a large library must block nothing.
		struct PortraitJob {
			int         folder_id = 0;
			std::string path;
			std::string name;
			};
		void portrait_worker();
		void portrait_seed();
		void portrait_request_front(int folder_id, const std::string& path,
		                            const std::string& name);
		// Downloads one portrait URL and normalises it to something storable.
		// Empty on any failure; it never throws.
		MediaStore::ArtistArtRow portrait_fetch(const std::string& url);

		std::thread             portrait_thread_;
		std::mutex              portrait_mu_;
		std::condition_variable portrait_cv_;
		std::deque<PortraitJob> portrait_queue_;
		std::set<std::string>   portrait_queued_;
		std::atomic<bool>       portrait_stop_{false};

		FolderWatcher   watcher_;
		httplib::Server server_;
	};

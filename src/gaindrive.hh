#pragma once

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <httplib.h>

#include "mediastore.hh"
#include "castmanager.hh"
#include "folderwatcher.hh"

class GainDrive {
	public:
		GainDrive(const std::string& db_path,
		          const std::string& music_root,
		          const std::string& upload_dir,
		          bool no_scan,
		          bool debug = false,
		          bool flat_multi_disc = true,
		          const std::string& user_db_path = "");
		void listen(const std::string& host, int port);

	private:
		bool            debug_;
		bool            flat_multi_disc_;
		std::string     upload_dir_;
		std::string     users_dir_;
		MediaStore      store_;
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

		// Proxies an external artist portrait URL, caching the result in memory.
		void serve_artist_portrait(httplib::Response& res,
		                           const std::string& image_url, int folder_id);

		// folder_id -> { Content-Type, body } for artist portrait images.
		std::mutex                                        artist_img_cache_mu_;
		std::unordered_map<int, std::pair<std::string, std::string>> artist_img_cache_;

		FolderWatcher   watcher_;
		httplib::Server server_;
	};

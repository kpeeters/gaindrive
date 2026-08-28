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
#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <httplib.h>

#include "mediastore.hh"
#include "castmanager.hh"
#include "coverart.hh"
#include "folderwatcher.hh"
#include "transcodecache.hh"
#include "urlfetch.hh"

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
		          bool video_art_embedded = false,
		          // Chromecasts named in the configuration, for the ones mDNS
		          // cannot find. Passed in rather than set afterwards because
		          // cast_manager_ is private and main has no other way in.
		          const std::vector<CastManager::CastDevice>& cast_devices = {},
		          // URL-fetch handlers. nullopt is "the config said nothing",
		          // which means the built-in table; an empty vector is "the
		          // config said no", which disables the feature. A plain vector
		          // could not express both.
		          const std::optional<std::vector<UrlHandler>>& url_handlers
		              = std::nullopt,
		          int url_fetch_timeout_s = 2 * 60 * 60);
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

		// Build and send one cast LOAD for `song`, starting at `offset`
		// seconds, with the caption track numbered `track_id` turned on (0 for
		// none).
		//
		// The single place that knows how a cast load is composed: the URL the
		// receiver will fetch, the castToken standing in for credentials the
		// television does not have, the contentType the stream will *actually*
		// carry (see cast_mime_for), and the side-loaded subtitle tracks. Three
		// endpoints need this — castLoad, castControl's IDLE recovery and
		// stream.view's cast redirect — and they had three copies of the URL
		// construction between them, which is two places for a new query
		// parameter to be forgotten.
		void cast_load_song(const httplib::Request& req,
		                    const MediaStore::SongInfo& song,
		                    int song_id, float offset, int track_id);

		// Probe each configured cast device once at startup and log the result,
		// so a wrong address is reported rather than only failing later.
		void probe_cast_devices_background();

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

		// ---- URL fetch ----
		//
		// Same shape as the portrait worker, and for the same reason: a fetch
		// takes minutes, so it cannot run on an httplib thread and the request
		// cannot wait for it.
		//
		// The queue lives here rather than inside UrlFetcher because the work
		// either side of the child process is gaindrive's — the uploads root,
		// the username, the depth-two normalisation and scan_dirs() — while
		// UrlFetcher knows the tool and nothing else. Owning the thread here is
		// also what makes it safe: it is joined in the destructor *body*, so it
		// cannot still be inside scan_dirs() when store_ is destroyed.
		//
		// Jobs are held in memory only. A batch is a directory and a scan; there
		// is no state a restart would need to repair, and a table would have to
		// be pruned by something. The cost is that a fetch in flight when the
		// server stops is lost, which is why a failed, timed-out or cancelled
		// job removes its batch directory rather than leaving a part file in the
		// uploads root for ever.
		struct FetchJob {
			std::string id;          // the batch uuid; also the id on the wire
			std::string batch;       // stored form, <root>/<user>/<uuid>
			std::string user;
			std::string url;
			std::string handler;
			bool        audio   = true;
			// What the user typed before pressing Fetch, each already
			// sanitised into a single path component, and empty when they
			// typed nothing. Applied by renaming the batch's two directory
			// levels after the tool exits and before the scan — see
			// apply_batch_names(). Unlike url and handler these are cleaned on
			// the way *in*, because they become directory names rather than
			// only wire strings.
			std::string artist, album;
			// queued | running | scanning | done | error | cancelled
			std::string state   = "queued";
			int         percent = 0;
			std::string detail;      // sanitised: never an absolute path
			std::string error;
			int         files   = 0;
			int64_t     started = 0, finished = 0;
			};
		void fetch_worker();
		// Everything between a producer finishing and the library being correct:
		// normalise what was written into <artist>/<album>/file, apply any names
		// the user typed by hand, then scan each artist directory in the batch.
		// Shared with /upload, which is the same steps around a different
		// producer — an archive rather than a fetched URL — and now passes the
		// same overrides. The defaults remain for a caller that has no names to
		// give. Catches — a contended database must not take the server with
		// it.
		void scan_batch(const std::string& rel_batch,
		                const std::filesystem::path& dest,
		                const std::string& fallback_artist,
		                const std::string& artist_override = "",
		                const std::string& album_override  = "");
		// Serialises the fold at the end of scan_batch, where a finished batch
		// is merged into the user's earlier ones. Two batches folding at once
		// would each move the other's contents away, and /upload detaches
		// scan_batch onto an HTTP thread, so two uploads really can arrive
		// together. Held only across the fold, never across the scan.
		std::mutex batch_fold_mu_;
		// Rewrites the batch's absolute path to its stored form. A tool's
		// progress line names the file it is writing, absolutely, and a root
		// path is never surfaced in an API response.
		std::string sanitise_detail(const std::string& line,
		                            const std::filesystem::path& dest,
		                            const std::string& rel_batch) const;

		UrlFetcher              url_fetcher_;
		std::thread             fetch_thread_;
		std::mutex              fetch_mu_;
		std::condition_variable fetch_cv_;
		std::deque<std::string> fetch_queue_;   // job ids, in submission order
		std::deque<FetchJob>    fetch_jobs_;    // queued, running and retained
		std::atomic<bool>       fetch_stop_{false};

		FolderWatcher   watcher_;
		httplib::Server server_;
	};

// Which peers' X-Forwarded-For header may be believed. Defaults to loopback,
// which is the reverse proxy the packaging installs. Free rather than a member
// because client_addr() is used by static helpers in gaindrive.cc — the access
// logger and check_auth's throttle — that have no GainDrive to ask.
//
// Believing the header from an untrusted peer is not merely a wrong log line:
// the login throttle keys on the result, so it would let a caller pick a fresh
// rate-limit bucket per request.
void gaindrive_set_trusted_proxies(std::vector<std::string> addrs);

#pragma once

// Normally supplied by CMake from the root VERSION file, or from the git tag
// for a release build. The fallback only exists so the tree still compiles if
// it is built by hand outside CMake.
#ifndef GAINDRIVE_VERSION
#define GAINDRIVE_VERSION "0.0-dev"
#endif

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <memory>
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

// Bounds on a request body, and on an image fetched for one. At file scope
// rather than inside GainDrive because the pre-request handler in apiweb.cc
// applies them, while the endpoints they describe live in apiedit.cc and
// apibrowse.cc.

// The ceiling on one upload archive as it streams through /upload, and
// therefore also httplib's global payload cap — the content-reader path
// enforces that cap on what it hands the receiver. Every other route is
// bounded far lower, before a byte of body is read, by the pre-request
// handler and MAX_SMALL_BODY_BYTES.
inline constexpr size_t MAX_REQUEST_BYTES = 4ull * 1024 * 1024 * 1024;

// The body bound for every route that is not /upload. Nothing else accepts a
// large body: the biggest legitimate one is a saveChapters file, which
// MAX_CHAPTERS keeps to a few tens of kilobytes.
inline constexpr size_t MAX_SMALL_BODY_BYTES = 1024 * 1024;

// The bound on a cover fetched by setCoverArt, which is a URL a person typed
// rather than one a provider returned, and so is bounded for the same reason
// as a portrait and one more: this one is reachable by any account allowed to
// edit the item. The redirect bound sits in netaddr.hh, because portrait_fetch
// needs the same one.
inline constexpr size_t MAX_COVER_BYTES = 16u * 1024 * 1024;

class CountingPool;

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
		          int url_fetch_timeout_s = 2 * 60 * 60,
		          // How wide the scan reads file metadata; see
		          // MediaStore::scan_jobs_. Appended, not inserted: every
		          // argument here is passed positionally.
		          int scan_jobs = 0);
		~GainDrive();
		// Binds and serves. Returns false without serving if the port could
		// not be acquired, so the caller can exit non-zero rather than treat a
		// doomed start as a clean shutdown.
		bool listen(const std::string& host, int port);

	private:
		// Route registration, split by subject; each is defined in its own
		// api*.cc and called once, in this order, from the constructor.
		//
		// The order is not cosmetic. httplib dispatches in registration
		// order and routes_fallback() registers `/rest/:endpoint`, a
		// wildcard that matches every Subsonic endpoint there is, so it has
		// to be registered after all of them or it answers the lot.
		void routes_web();       // server options, middleware, web assets
		void routes_system();    // ping, scan, users, settings
		void routes_browse();    // the library, its artwork and its texts
		void routes_playlist();  // play queue, playlists, stars, bookmarks
		void routes_stream();    // stream, download, video, captions, hls
		void routes_cast();      // the Chromecast session
		void routes_edit();      // tag edits, cover art, upload, move
		void routes_fetch();     // fetching from a URL
		void routes_fallback();  // the unknown-endpoint catch-all; last

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
		// Holds the transcode-cache entry a cast LOAD was warmed against, for
		// as long as that LOAD is the current one.  TranscodeCache::Entry is
		// an RAII in-use count and prune() skips in-use keys, so dropping it
		// after the warm would let the file be evicted between the warm and
		// the receiver's first GET — which is exactly the cold-entry wait the
		// warm exists to avoid.  Guarded by cast_warm_mu_ because the warm
		// runs on CastManager's load worker while a teardown can arrive on an
		// httplib thread.
		std::mutex      cast_warm_mu_;
		std::shared_ptr<const TranscodeCache::Entry> cast_warm_entry_;
		// What the last LOAD decided — reported to the client rather than
		// left for it to re-derive, for the reason cast_load_song() gives
		// about audioOnly: the ladder these come off lives in codecs.hh, and
		// a second copy of it in JavaScript is a copy that will drift.
		//
		// Held for castSession's snapshot as well as castLoad's reply, because
		// a page reload has no castLoad response to have read it from.
		struct CastStreamInfo {
			bool        audio_only = false;
			// Whether a picture is going to appear on the receiver, which is
			// **not** !audio_only.  The two came apart when a screenless
			// device gained the option of being sent the film anyway: there
			// the whole video goes out and the receiver still shows nothing.
			// A client keeps its own muted picture on this, never on
			// audio_only, which describes the bytes rather than the screen.
			bool        receiver_video = false;
			std::string mime;    // the contentType the LOAD declared
			std::string suffix;  // container the receiver actually receives
			int         bitrate = 0;  // kbps; 0 when not a fixed-rate encode
			std::string tier;    // direct | remux | encode
			};
		// Guarded, unlike last_cast_song_id_ and last_cast_offset_ beside it,
		// and the reason is the reason caption_ids is guarded: this holds three
		// std::strings, so a torn read is not a wrong number but undefined
		// behaviour.  It is written from httplib threads *and* from
		// CastManager's load worker — the fallback's prepare hook swaps it when
		// a refused film becomes its soundtrack — and read from the SSE thread
		// on every push.
		mutable std::mutex cast_stream_mu_;
		CastStreamInfo     last_cast_stream_;

		// Both take cast_stream_mu_, so neither is the plain accessor it
		// looks like; see the note above on why the lock is needed.
		CastStreamInfo cast_stream() const;
		void set_cast_stream(const CastStreamInfo& s);

		// The cast session's owner: the account *and* the client instance that
		// called startCast. Both halves are needed. The account alone is what
		// this replaced, and it is why casting from the web client and then
		// playing on the phone under the same login sent the track to the
		// television — `active_` is one process-global bool, so every
		// stream.view in the server was rerouted to whatever receiver anyone
		// had most recently picked.
		//
		// The client half is the `castController` parameter rather than the
		// Subsonic `c=` one, because `c=` names the *kind* of client: every
		// browser sends `gaindrive-web`, so two browsers of one user would go
		// on hijacking each other. startCast requires it, which is what makes
		// the "sent no id" bucket incapable of owning anything — otherwise two
		// installs of one app would collapse into a single identity and
		// reproduce the same bug one scale down.
		//
		// It is not a credential and authorises nothing: auth is still
		// u/t/s plus castRole, and this only breaks ties among one user's own
		// devices. Hence nothing to redact in the access logger.
		std::mutex      cast_owner_mu_;
		std::string     cast_owner_user_;
		std::string     cast_owner_controller_;

		// Bumped whenever the session is claimed or torn down, so a connection
		// belonging to a *previous* session can notice it has been displaced.
		// The same pattern as load_gen_ and cast_wd_gen_, and castEvents needs
		// it because a takeover is stop()-then-start(): `active_` is true
		// either side of a window the SSE thread spends blocked inside
		// wait_status(15000), so without a generation the old owner's stream
		// would quietly go on reporting the new owner's session.
		std::atomic<int> cast_session_gen_{0};

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

		// A credential-free grant to everything one track's playback needs, for
		// a receiver that has no account of its own.
		//
		// This is the other half of what CastManager's token does for the
		// server-driven cast, and it cannot be that token: CastManager holds a
		// *single* one bound to the live session, and these belong to clients
		// that hold the Cast control channel themselves — concurrent, and with
		// no session on this server at all.
		//
		// Scoped to one song, one account and a short life, because the point
		// of it is not to hand a receiver `u`/`t`/`s`: `t` is
		// md5(password + salt) and `s` is the salt, which together read the
		// whole library as that person for as long as the password stands.
		//
		// It covers three endpoints because a LOAD needs three things, and a
		// grant that covered only the audio would leave the password on the
		// television regardless — the sleeve travels in the LOAD's metadata
		// and the receiver fetches that too.
		struct StreamGrant
			{
			std::string                           user;
			int                                   song_id  = -1;
			// The song's own cover_art_id, resolved when the grant is minted
			// rather than named by the caller, so that asking for a grant can
			// never be a way to name somebody else's artwork.
			int                                   cover_id = -1;
			std::chrono::steady_clock::time_point expires;
			};
		std::mutex                                   grant_mu_;
		std::unordered_map<std::string, StreamGrant> grants_;

		// Mint a grant for `user` covering `song_id` and its cover art; empty
		// if it could not be.
		std::string mint_stream_grant(const std::string& user, int song_id,
		                              int cover_id);

		// The live grant a token names, or nullopt. Takes the lock and returns
		// a copy rather than a pointer into the table, so the three callers
		// below cannot hold a reference across an eviction.
		std::optional<StreamGrant> grant_lookup(const std::string& token);

		// The account a grant authorises for `song_id`, or empty when none
		// does — expired, for another song, or simply not a grant.
		std::string stream_grant_user(const std::string& token, int song_id);

		// True when a grant covers this cover art. Separate from the above
		// because a cover is addressed by its own id, not by the song's.
		bool grant_allows_cover(const std::string& token, int cover_id);

		// True when a grant covers a caption of its song.
		//
		// Any caption of it, deliberately unlike valid_caption_token(), which
		// scopes to the ids one LOAD declared. There is no LOAD here to
		// mirror — the client builds its own — and a subtitle of a song the
		// account may already read is not a wider reach than the song was.
		bool grant_allows_captions(const std::string& token, int song_id);

		// Refuse a cast request that did not come from the network this
		// server is on, and end any session the refused caller owns.
		//
		// Separate from the static check_cast_perm() rather than folded into
		// it for two reasons: it needs `this`, to reach cast_owned_by() and
		// cast_teardown(); and stopCast deliberately does not call it, so the
		// two are not one test. Refusing stopCast would leave the music
		// playing in the house with no way to end it from where the person
		// with the laptop is actually standing.
		bool check_cast_local(const httplib::Request& req, httplib::Response& res,
		                      bool use_json);

		// True when `req` is the client that owns the current cast session.
		// False when it sent no castController at all, which is what keeps
		// every client that does not speak the extension — the Android app, a
		// third-party Subsonic client, curl — out of cast mode entirely.
		bool cast_owned_by(const httplib::Request& req);

		// Record the owner of a session just started, and bump the generation.
		void cast_claim(const std::string& user, const std::string& controller);

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
		// Returns the description of what was sent — whether it was the
		// soundtrack alone (the session's device announced no video_out), the
		// contentType declared, and the container, bitrate and tier the
		// receiver will actually get.  castLoad and castSession report all of
		// it to the client, which draws it rather than working it out again
		// from the device list and the codec pair.
		CastStreamInfo cast_load_song(const httplib::Request& req,
		                              const MediaStore::SongInfo& song,
		                              int song_id, float offset, int track_id);

		// One LOAD carrying a film's soundtrack rather than the film.
		//
		// Extracted because it is now built twice: as the load itself, when the
		// receiver announced no screen, and as the **fallback** hung off a
		// video load a screenless receiver may refuse outright.  Two copies of
		// the format choice, the token minting and the cache warm would be two
		// copies of something that has to agree with Streamer::serve() exactly.
		//
		// `url` comes back empty when there is nothing to send — no token, or a
		// silent film with no audio stream to extract — and the caller decides
		// what that means.  `desc` is filled with what a client should be told.
		//
		// is_fallback does two extra things, and both are only knowable here:
		// it swaps last_cast_stream_ so castSession stops describing the
		// attempt that failed, and it records the refusal so the next play of
		// the same film on the same device does not repeat it.
		CastManager::LoadRequest
		    soundtrack_load(const std::string& base,
		                    const MediaStore::SongInfo& song, int song_id,
		                    float offset, bool is_fallback,
		                    CastStreamInfo& desc);

		// Codec pairs a device has already refused when handed the whole file,
		// held in client.settings under "cast_novideo:<device id>" beside the
		// "cast_video:<device id>" preference that allowed the attempt.
		//
		// Without it every play of an undecodable film pays the failed LOAD
		// again; with it the failure happens once, ever.  setCastDevicePref
		// clears the row, since changing your mind about a device is the
		// natural place to make it reconsider — and the only way a negative
		// that has gone stale (new firmware, a different device at the same
		// address) is ever forgotten.
		bool cast_video_refused(const std::string& device_id,
		                        const std::string& codec_pair);
		void cast_note_video_refused(const std::string& device_id,
		                             const std::string& codec_pair);

		// Probe each configured cast device once at startup and log the result,
		// so a wrong address is reported rather than only failing later.
		void probe_cast_devices_background();

		// ---- The online info resolver ----
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
		//
		// **Albums are the second kind of job rather than a second thread**,
		// which is why this section is no longer called "artist portraits".
		// getAlbumInfo ran its own two MusicBrainz requests plus Wikidata and
		// Wikipedia straight off the HTTP pool, up to 32 at a time; the whole
		// argument above applied to it word for word one field over. It shares
		// this queue because mb_pace() is one gate for the process, so a
		// second consumer would add no throughput while halving the courtesy
		// headroom LOOKUP_GAP exists to leave.
		enum class LookupKind { Artist, Album };
		struct LookupJob {
			LookupKind  kind = LookupKind::Artist;
			int         folder_id = 0;
			std::string path;
			// The artist's name, or the album's title. Only for the log and
			// for the providers; the album branch re-reads the row it needs.
			std::string name;
			};
		void lookup_worker();
		// One job, providers and all. Separate from the loop so the guard
		// there wraps a job rather than the thread -- see its definition.
		void lookup_run_job(const LookupJob& job);
		void lookup_seed();
		// Ask the worker to re-seed now rather than when its timer next
		// expires.  Called when a scan finishes, which is the only event that
		// can turn an empty seed into a full one.
		void lookup_wake();
		void lookup_request_front(LookupKind kind, int folder_id,
		                          const std::string& path,
		                          const std::string& name);
		// The other end of the queue, for a pass over the whole library:
		// nobody is waiting on these, so they must not get in front of the
		// artist a client is looking at right now. startInfoLookup is its only
		// caller. It answers whether it actually queued the job, so that
		// caller's counts are what was added rather than what it looked at.
		bool lookup_request_back(LookupKind kind, int folder_id,
		                         const std::string& path,
		                         const std::string& name);
		// Seeds the queue's back from artists_needing_bio() and
		// albums_needing_info(), and returns how many of each it added.
		// startInfoLookup's answer is that pair.
		struct LookupSeeded { int artists = 0; int albums = 0; };
		LookupSeeded lookup_seed_missing_info(bool artists, bool albums);
		// Whether this folder is queued for the resolver or being resolved
		// right now. It is what makes a *forced* re-lookup observable: both
		// the info cache and the artist_art status still describe the
		// previous answer until the worker replaces them, so a client polling
		// after `force` would otherwise be told on its very next request that
		// the work had finished — before it had started.
		//
		// One set for both kinds: an album folder's path and an artist
		// folder's path cannot be the same string.
		bool lookup_pending(const std::string& path);
		// getArtistInfo / getArtistInfo2. A member rather than a free function
		// because answering one now means *queueing* the lookup instead of
		// performing it, and the queue is ours. `key` is "artistInfo" or
		// "artistInfo2" — it names the XML element and the JSON key.
		void handle_artist_info(const httplib::Request& req,
		                        httplib::Response& res, const char* key);
		// getAlbumInfo / getAlbumInfo2, a member for the same reason and since
		// the same change. `key` is "albumInfo" or "albumInfo2".
		void handle_album_info(const httplib::Request& req,
		                       httplib::Response& res, const char* key);
		// Downloads one portrait URL and normalises it to something storable.
		// Empty on any failure; it never throws. Still named for the portrait
		// because that is all it is: an album has no image to fetch.
		MediaStore::ArtistArtRow portrait_fetch(const std::string& url);

		std::thread             lookup_thread_;
		std::mutex              lookup_mu_;
		std::condition_variable lookup_cv_;
		std::deque<LookupJob>   lookup_queue_;
		std::set<std::string>   lookup_queued_;
		std::atomic<bool>       lookup_stop_{false};
		// Set by lookup_wake(), cleared by the worker when it acts on it.
		// A plain bool under lookup_mu_ rather than an atomic, because it is
		// read inside the condition variable's predicate and so must be part
		// of what the lock protects — a notify that races the predicate is a
		// wake-up the worker sleeps straight through.
		bool                    lookup_reseed_ = false;

		// ---- URL fetch ----
		//
		// Same shape as the info resolver, and for the same reason: a fetch
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

		// How many fetches may be waiting at once. One worker runs the queue,
		// so this is a bound on how far behind a user can get the server, not
		// on throughput. Here rather than in apifetch.cc because
		// getServerStatus reports it beside the queue length.
		static constexpr size_t FETCH_QUEUE_MAX = 20;

		UrlFetcher              url_fetcher_;
		std::thread             fetch_thread_;
		std::mutex              fetch_mu_;
		std::condition_variable fetch_cv_;
		std::deque<std::string> fetch_queue_;   // job ids, in submission order
		std::deque<FetchJob>    fetch_jobs_;    // queued, running and retained
		std::atomic<bool>       fetch_stop_{false};

		FolderWatcher   watcher_;

		// Owned and deleted by server_; set by the task-queue factory in
		// apiweb.cc. Only read while the server is listening, so the pointer
		// is valid whenever a handler dereferences it.
		std::atomic<CountingPool*> http_pool_{nullptr};
		const std::chrono::steady_clock::time_point start_time_ =
			std::chrono::steady_clock::now();
		httplib::Server server_;
	};

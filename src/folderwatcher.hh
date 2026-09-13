#pragma once

#include <cstddef>
#include <string>
#include <atomic>
#include <mutex>
#include <utility>
#include <set>
#include <thread>
#include <unordered_map>
#include <vector>

class MediaStore;

class FolderWatcher {
	public:
		// Watches every root the store is configured with, the uploads root
		// included — but the two are watched for different things, and the
		// difference is load-bearing.
		//
		// A library root's events name an artist directory to rescan. An
		// uploads root's do not: they only ever set a flag, and the debounced
		// action is MediaStore::reconcile_uploads(), which prunes the rows of
		// batch directories that have gone and never indexes anything. That
		// asymmetry is what keeps the watcher unable to index a batch while a
		// fetch is still writing it — apply_batch_names()' plain fs::rename is
		// safe only because nothing under a live batch has rows yet.
		//
		// Personal batches still reach the DB through the explicit scan_dirs()
		// calls in scan_batch(), which know the <user>/<uuid>/<artist>/<album>
		// layout. Nothing here does.
		FolderWatcher(MediaStore& store, int debounce_ms = 2000);
		~FolderWatcher();

		void start();   // no-op where unsupported, or if already running
		void stop();    // stops watching and joins; safe to call repeatedly

#if defined(__APPLE__)
		// Entry point for the FSEvents C callback, which is a free function and
		// so cannot reach a private member. Public for that reason only — it
		// takes one coalesced batch of changed directory paths and is not
		// meant to be called from anywhere else.
		void handle_paths(size_t n, const char* const* paths);
#endif

	private:
		// The library root containing `path`, or "" if none does.
		std::string root_of(const std::string& path) const;
		// The depth-1 child of the owning root that contains (or is) `path` —
		// the artist (or category) directory a change belongs to. Returns ""
		// both when `path` lies outside every root and when it *is* a root,
		// which callers treat as "rescan that whole root".
		std::string artist_dir_for_path(const std::string& path) const;

		// True when `path` is the uploads root or sits under it. There is at
		// most one, so this is a prefix test rather than a lookup.
		bool is_uploads(const std::string& path) const;

		MediaStore&              store_;
		std::vector<std::string> roots_;   // every root, absolute
		std::string              uploads_root_;   // "" when none is configured
		int                      debounce_ms_;

#if defined(__linux__)
		void run();
		void add_watch(const std::string& path);
		void remove_watch(int wd);
		void add_watches_recursive(const std::string& root);

		int                                 inotify_fd_ = -1;
		int                                 pipe_fd_[2]      = {-1, -1};
		int                                 rewatch_pipe_[2] = {-1, -1};
		std::atomic<bool>                   scan_running_{false};
		std::thread                         thread_;
		std::unordered_map<int,std::string> wd_to_path_;
		std::set<std::string>               changed_artists_;
		// Set by a removal under the uploads root, taken with the set above by
		// the debounced rescan. Touched only on the watcher thread, as
		// changed_artists_ is, so it needs no lock of its own.
		bool                                uploads_dirty_ = false;
		std::mutex                          rewatches_mutex_;
		std::vector<std::string>            pending_rewatches_;
#elif defined(__APPLE__)
		// Runs on a detached thread: drains changed_artists_ and rescans until
		// nothing new is left.
		void drain_and_scan();
		// Maps a path FSEvents reported back onto the configured root prefix.
		std::string unresolve(const std::string& path) const;

		// Canonical root path -> configured root path, filled in by start().
		std::vector<std::pair<std::string,std::string>> canon_roots_;

		// FSEventStreamRef and dispatch_queue_t, held as void* so this header
		// does not drag CoreServices into every translation unit that includes
		// it — gaindrive.cc has no business seeing Carbon.
		void*                 stream_ = nullptr;
		void*                 queue_  = nullptr;
		// Both guarded by changed_mutex_. Clearing the flag under the same lock
		// that guards the set is what makes the scan handoff race-free.
		std::mutex            changed_mutex_;
		std::set<std::string> changed_artists_;
		bool                  uploads_dirty_ = false;
		bool                  scan_running_ = false;
#endif
	};

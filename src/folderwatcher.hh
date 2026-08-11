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
		// Watches every library root the store is configured with. The uploads
		// root is deliberately not watched: personal batches are scanned by
		// explicit scan_dirs() calls from the upload handler, which know the
		// batch layout, and watching them would also burn inotify slots.
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

		MediaStore&              store_;
		std::vector<std::string> roots_;   // library roots, absolute
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
		bool                  scan_running_ = false;
#endif
	};

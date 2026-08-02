#pragma once

#include <string>
#include <atomic>
#include <mutex>
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

		void start();   // no-op on non-Linux or if already running
		void stop();    // signals thread and joins; safe to call multiple times

	private:
#ifdef __linux__
		void run();
		void add_watch(const std::string& path);
		void remove_watch(int wd);
		void add_watches_recursive(const std::string& root);
		// Returns the depth-1 child of the owning root that contains (or is)
		// the directory watched by wd.  ev_name is the inotify event's name
		// field (may be null).  Returns "" if the path cannot be determined.
		std::string artist_dir_for(int wd, const char* ev_name) const;
		// The library root containing `path`, or "" if none does.
		std::string root_of(const std::string& path) const;

		MediaStore&                         store_;
		std::vector<std::string>            roots_;   // library roots, absolute
		int                                 debounce_ms_;
		int                                 inotify_fd_ = -1;
		int                                 pipe_fd_[2]     = {-1, -1};
		int                                 rewatch_pipe_[2] = {-1, -1};
		std::atomic<bool>                   scan_running_{false};
		std::thread                         thread_;
		std::unordered_map<int,std::string> wd_to_path_;
		std::set<std::string>               changed_artists_;
		std::mutex                          rewatches_mutex_;
		std::vector<std::string>            pending_rewatches_;
#else
		MediaStore&  store_;
		int          debounce_ms_;
#endif
	};

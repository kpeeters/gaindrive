#pragma once

#include <string>
#include <atomic>
#include <set>
#include <thread>
#include <unordered_map>

class MediaStore;

class FolderWatcher {
	public:
		FolderWatcher(MediaStore& store, const std::string& music_root, int debounce_ms = 2000);
		~FolderWatcher();

		void start();   // no-op on non-Linux or if already running
		void stop();    // signals thread and joins; safe to call multiple times

	private:
#ifdef __linux__
		void run();
		void add_watch(const std::string& path);
		void remove_watch(int wd);
		void add_watches_recursive(const std::string& root);
		// Returns the depth-1 child of music_root_ that contains (or is) the
		// directory watched by wd.  ev_name is the inotify event's name field
		// (may be null).  Returns "" if the path cannot be determined.
		std::string artist_dir_for(int wd, const char* ev_name) const;

		MediaStore&                         store_;
		std::string                         music_root_;
		int                                 debounce_ms_;
		int                                 inotify_fd_ = -1;
		int                                 pipe_fd_[2] = {-1, -1};
		std::atomic<bool>                   scan_running_{false};
		std::thread                         thread_;
		std::unordered_map<int,std::string> wd_to_path_;
		std::set<std::string>               changed_artists_;
#else
		MediaStore&  store_;
		std::string  music_root_;
		int          debounce_ms_;
#endif
	};

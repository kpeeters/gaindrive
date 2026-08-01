#include "folderwatcher.hh"
#include "mediastore.hh"
#include "stamp.hh"

#ifdef __linux__

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <filesystem>
#include <chrono>
#include <optional>
#include <iostream>

namespace fs = std::filesystem;

// Self-pipe wakeup.  A one-byte write to a pipe with room cannot fail for any
// reason worth handling except EINTR, but the result must still be consumed —
// write() is warn_unused_result, and a lost wakeup here would hang the join in
// stop() rather than fail visibly.
static void poke(int fd)
	{
	char b = 0;
	ssize_t n;
	do { n = write(fd, &b, 1); } while (n < 0 && errno == EINTR);
	}

// Events we care about on each watched directory.
static constexpr uint32_t WATCH_MASK =
	IN_CREATE      |   // new file or subdirectory
	IN_DELETE      |   // file or subdirectory removed
	IN_CLOSE_WRITE |   // file closed after write (once per copy, not per block)
	IN_MOVED_FROM  |   // rename / move source
	IN_MOVED_TO    |   // rename / move destination
	IN_DELETE_SELF |   // the watched directory itself was deleted
	IN_MOVE_SELF   ;   // the watched directory itself was moved

FolderWatcher::FolderWatcher(MediaStore& store,
                              const std::string& music_root,
                              int debounce_ms)
	: store_(store), music_root_(music_root), debounce_ms_(debounce_ms)
	{
	}

FolderWatcher::~FolderWatcher()
	{
	stop();
	}

void FolderWatcher::add_watch(const std::string& path)
	{
	int wd = inotify_add_watch(inotify_fd_, path.c_str(), WATCH_MASK);
	if (wd == -1) {
		if (errno == ENOSPC)
			std::cout << stamp()
			          << "FolderWatcher: inotify watch limit reached for " << path
			          << ". Raise with: sysctl fs.inotify.max_user_watches=524288"
			          << std::endl;
		else
			// EACCES is common when a directory is first created and its
			// permissions haven't been fully set yet; the rewatch after the
			// next rescan will pick it up.
			std::cout << stamp()
			          << "FolderWatcher: failed to watch " << path
			          << ": " << strerror(errno) << " (will retry after rescan)"
			          << std::endl;
		return;
		}
	wd_to_path_[wd] = path;
	}

void FolderWatcher::remove_watch(int wd)
	{
	inotify_rm_watch(inotify_fd_, wd);   // may already be auto-removed; ignore error
	wd_to_path_.erase(wd);
	}

std::string FolderWatcher::artist_dir_for(int wd, const char* ev_name) const
	{
	auto it = wd_to_path_.find(wd);
	if (it == wd_to_path_.end()) return "";

	fs::path dir(it->second);
	fs::path root(music_root_);

	// Event is on the root itself: the affected artist is root/ev_name.
	if (dir == root) {
		if (ev_name && ev_name[0] && std::string(ev_name) != ".users")
			return (root / ev_name).string();
		return "";
		}

	// Walk up to the depth-1 child of root (the artist dir).
	auto rel = dir.lexically_relative(root);
	auto depth1 = rel.begin()->string();
	// The personal-upload area is managed exclusively via explicit scan_dirs()
	// calls from the upload handler; treating .users as an artist dir would
	// re-parent the UUID batch folders and break personal-library browsing.
	if (depth1 == ".users") return "";
	return (root / *rel.begin()).string();
	}

void FolderWatcher::add_watches_recursive(const std::string& root)
	{
	// Never watch the personal-upload area; artist_dir_for() ignores events
	// there, and the scan_dirs() calls from the upload handler are authoritative.
	// Skipping also avoids exhausting inotify watch slots on large uploads.
	std::string users_abs = music_root_ + "/.users";
	if (root == users_abs
	    || root.compare(0, users_abs.size() + 1, users_abs + '/') == 0)
		return;

	add_watch(root);
	std::error_code ec;
	for (auto& entry : std::filesystem::recursive_directory_iterator(root,
	                       std::filesystem::directory_options::skip_permission_denied, ec)) {
		if (entry.is_directory(ec)
		    && entry.path().string() != users_abs
		    && entry.path().string().compare(0, users_abs.size() + 1,
		                                     users_abs + '/') != 0)
			add_watch(entry.path().string());
		}
	if (ec)
		std::cout << stamp() << "FolderWatcher: error walking " << root
		          << ": " << ec.message() << std::endl;
	}

void FolderWatcher::start()
	{
	if (thread_.joinable())
		return;   // already running

	inotify_fd_ = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (inotify_fd_ == -1) {
		std::cout << stamp() << "FolderWatcher: inotify_init1 failed: "
		          << strerror(errno) << std::endl;
		return;
		}

	if (pipe2(pipe_fd_, O_CLOEXEC | O_NONBLOCK) == -1) {
		std::cout << stamp() << "FolderWatcher: pipe2 failed: "
		          << strerror(errno) << std::endl;
		close(inotify_fd_);
		inotify_fd_ = -1;
		return;
		}

	if (pipe2(rewatch_pipe_, O_CLOEXEC | O_NONBLOCK) == -1) {
		std::cout << stamp() << "FolderWatcher: pipe2 (rewatch) failed: "
		          << strerror(errno) << std::endl;
		close(inotify_fd_); inotify_fd_ = -1;
		close(pipe_fd_[0]); close(pipe_fd_[1]);
		pipe_fd_[0] = pipe_fd_[1] = -1;
		return;
		}

	thread_ = std::thread(&FolderWatcher::run, this);
	}

void FolderWatcher::stop()
	{
	if (pipe_fd_[1] != -1) {
		// Wake the poll() in run() so the thread exits cleanly.
		poke(pipe_fd_[1]);
		}
	if (thread_.joinable())
		thread_.join();
	if (inotify_fd_      != -1) { close(inotify_fd_);       inotify_fd_       = -1; }
	if (pipe_fd_[0]     != -1) { close(pipe_fd_[0]);      pipe_fd_[0]       = -1; }
	if (pipe_fd_[1]     != -1) { close(pipe_fd_[1]);      pipe_fd_[1]       = -1; }
	if (rewatch_pipe_[0] != -1) { close(rewatch_pipe_[0]); rewatch_pipe_[0] = -1; }
	if (rewatch_pipe_[1] != -1) { close(rewatch_pipe_[1]); rewatch_pipe_[1] = -1; }
	}

void FolderWatcher::run()
	{
	// Build the inotify watch set on this thread rather than in start(), so
	// the GainDrive constructor — and therefore listen() — isn't blocked by
	// a multi-second recursive walk on large libraries.
	add_watches_recursive(music_root_);
	std::cout << stamp() << "FolderWatcher: watching " << wd_to_path_.size()
	          << " directories under " << music_root_ << std::endl;

	using Clock = std::chrono::steady_clock;
	std::optional<Clock::time_point> last_event;

	// Buffer sized to hold several events; aligned for inotify_event.
	alignas(struct inotify_event) char buf[4096];

	while (true) {
		// Compute poll timeout based on debounce state.
		int timeout_ms = -1;
		if (last_event.has_value()) {
			auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
				Clock::now() - *last_event).count();
			long remaining = debounce_ms_ - elapsed;
			if (remaining <= 0) {
				// Debounce period expired — start a rescan if none is running.
				if (!scan_running_.exchange(true)) {
					auto abs_dirs = std::move(changed_artists_);
					changed_artists_.clear();
					std::cout << stamp() << "FolderWatcher: rescanning "
					          << abs_dirs.size() << " artist director"
					          << (abs_dirs.size() == 1 ? "y" : "ies") << std::endl;
					// scan_dirs() expects music-root-relative paths; the
					// rewatch helper still needs absolute paths for
					// inotify_add_watch().
					std::string root_slash = music_root_;
					if (root_slash.empty() || root_slash.back() != '/')
						root_slash += '/';
					std::set<std::string> rel_dirs;
					for (auto& d : abs_dirs) {
						if (d == music_root_)
							rel_dirs.insert("");
						else if (d.size() > root_slash.size()
						         && d.compare(0, root_slash.size(), root_slash) == 0)
							rel_dirs.insert(d.substr(root_slash.size()));
						else
							rel_dirs.insert(d);   // outside music_root — shouldn't happen
						}
					std::thread([this, rel_dirs = std::move(rel_dirs),
					             abs_dirs = std::move(abs_dirs)]{
						store_.scan_dirs(rel_dirs);
						// Signal run() to re-watch the rescanned dirs so that
						// directories which were inaccessible at creation time
						// (transient EACCES) get a watch added now.
						{
						std::lock_guard<std::mutex> lk(rewatches_mutex_);
						for (auto& d : abs_dirs)
							pending_rewatches_.push_back(d);
						}
						poke(rewatch_pipe_[1]);
						scan_running_.store(false);
						}).detach();
					last_event.reset();
					}
				else {
					// Scan in progress; reset the timer so we retry after
					// another debounce period rather than spinning.
					std::cout << stamp()
					          << "FolderWatcher: rescan already running, will retry"
					          << std::endl;
					last_event = Clock::now();
					}
				// Fall through to poll with timeout -1 until next event.
				}
			else {
				timeout_ms = (int)remaining;
				}
			}

		struct pollfd fds[3];
		fds[0] = { inotify_fd_,       POLLIN, 0 };
		fds[1] = { pipe_fd_[0],       POLLIN, 0 };
		fds[2] = { rewatch_pipe_[0],  POLLIN, 0 };

		int r = poll(fds, 3, timeout_ms);
		if (r < 0) {
			if (errno == EINTR) continue;
			std::cout << stamp() << "FolderWatcher: poll error: "
			          << strerror(errno) << std::endl;
			break;
			}

		// Stop signal.
		if (fds[1].revents & POLLIN)
			break;

		// Rewatch signal: a scan just finished; add watches for any directories
		// that failed earlier due to transient EACCES.
		if (fds[2].revents & POLLIN) {
			char b;
			while (read(rewatch_pipe_[0], &b, 1) == 1) {}  // drain
			std::vector<std::string> todo;
			{
			std::lock_guard<std::mutex> lk(rewatches_mutex_);
			todo.swap(pending_rewatches_);
			}
			for (auto& path : todo)
				add_watches_recursive(path);
			}

		if (!(fds[0].revents & POLLIN))
			continue;   // timeout or spurious wakeup

		// Drain all pending inotify events.
		while (true) {
			ssize_t n = read(inotify_fd_, buf, sizeof(buf));
			if (n == -1) {
				if (errno == EAGAIN || errno == EWOULDBLOCK) break;
				std::cout << stamp() << "FolderWatcher: read error: "
				          << strerror(errno) << std::endl;
				break;
				}
			if (n == 0) break;

			for (char* p = buf; p < buf + n; ) {
				auto* ev = reinterpret_cast<struct inotify_event*>(p);
				p += sizeof(struct inotify_event) + ev->len;

				if (ev->mask & IN_IGNORED) {
					// Kernel auto-removed this watch (e.g. parent dir was
					// moved/deleted).  Clean up our map so the descriptor
					// can be reused without leaving a stale entry.
					wd_to_path_.erase(ev->wd);
					continue;
					}

				if (ev->mask & IN_Q_OVERFLOW) {
					std::cout << stamp()
					          << "FolderWatcher: inotify queue overflow; "
					             "falling back to full rescan"
					          << std::endl;
					// Can't know what changed; mark for full scan.
					changed_artists_.insert(music_root_);
					last_event = Clock::now();
					continue;
					}

				// Arm / reset the debounce timer and record the affected artist dir.
				last_event = Clock::now();
				{
				std::string artist = artist_dir_for(ev->wd,
				                                    ev->len > 0 ? ev->name : nullptr);
				if (!artist.empty())
					changed_artists_.insert(artist);
				}

				// Log the event.
				{
				auto it = wd_to_path_.find(ev->wd);
				std::string parent = (it != wd_to_path_.end()) ? it->second : "?";
				std::string name   = (ev->len > 0) ? ev->name : "";
				std::string full   = name.empty() ? parent : parent + "/" + name;
				const char* kind =
					(ev->mask & IN_CREATE)      ? "created"  :
					(ev->mask & IN_DELETE)       ? "deleted"  :
					(ev->mask & IN_CLOSE_WRITE)  ? "modified" :
					(ev->mask & IN_MOVED_FROM)   ? "moved out" :
					(ev->mask & IN_MOVED_TO)     ? "moved in"  :
					(ev->mask & IN_DELETE_SELF)  ? "deleted"  :
					(ev->mask & IN_MOVE_SELF)    ? "moved out" : "changed";
				std::cout << stamp() << "FolderWatcher: " << kind
				          << "  " << full << std::endl;
				}

				// When a new subdirectory appears, watch it immediately so
				// events inside it are captured before the next scan runs.
				// Use recursive watching in case it has disc subfolders.
				bool is_dir = ev->mask & IN_ISDIR;
				bool is_create = ev->mask & (IN_CREATE | IN_MOVED_TO);
				if (is_dir && is_create && ev->len > 0) {
					auto it = wd_to_path_.find(ev->wd);
					if (it != wd_to_path_.end())
						add_watches_recursive(it->second + "/" + ev->name);
					}

				// When the watched directory itself disappears, clean up.
				if (ev->mask & (IN_DELETE_SELF | IN_MOVE_SELF))
					remove_watch(ev->wd);
				}
			}
		}
	}

#else  // non-Linux stubs

FolderWatcher::FolderWatcher(MediaStore& store,
                              const std::string& music_root,
                              int debounce_ms)
	: store_(store), music_root_(music_root), debounce_ms_(debounce_ms)
	{
	}

FolderWatcher::~FolderWatcher() { }
void FolderWatcher::start() { }
void FolderWatcher::stop()  { }

#endif

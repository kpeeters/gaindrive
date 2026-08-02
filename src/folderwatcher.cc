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

FolderWatcher::FolderWatcher(MediaStore& store, int debounce_ms)
	: store_(store), debounce_ms_(debounce_ms)
	{
	for (const auto& r : store_.roots())
		if (r.type != "uploads") roots_.push_back(r.path);
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

std::string FolderWatcher::root_of(const std::string& path) const
	{
	for (const auto& r : roots_) {
		if (path == r) return r;
		if (path.size() > r.size() && path.compare(0, r.size(), r) == 0
		        && path[r.size()] == '/')
			return r;
		}
	return "";
	}

std::string FolderWatcher::artist_dir_for(int wd, const char* ev_name) const
	{
	auto it = wd_to_path_.find(wd);
	if (it == wd_to_path_.end()) return "";

	std::string owner = root_of(it->second);
	if (owner.empty()) return "";   // not under any library root

	fs::path dir(it->second);
	fs::path root(owner);

	// Event is on a root itself: the affected level-1 dir is root/ev_name.
	if (dir == root) {
		if (ev_name && ev_name[0]) return (root / ev_name).string();
		return "";
		}

	// Walk up to the depth-1 child of the owning root.
	auto rel = dir.lexically_relative(root);
	if (rel.empty()) return "";
	return (root / *rel.begin()).string();
	}

void FolderWatcher::add_watches_recursive(const std::string& root)
	{
	add_watch(root);
	std::error_code ec;
	// follow_directory_symlink so a root that is itself a symlink, or that
	// contains symlinked subtrees, still gets watched. Without it such a
	// library would silently never live-rescan.
	for (auto& entry : std::filesystem::recursive_directory_iterator(root,
	                       std::filesystem::directory_options::skip_permission_denied
	                     | std::filesystem::directory_options::follow_directory_symlink,
	                       ec)) {
		if (entry.is_directory(ec))
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
	for (const auto& r : roots_)
		add_watches_recursive(r);
	std::cout << stamp() << "FolderWatcher: watching " << wd_to_path_.size()
	          << " directories across " << roots_.size() << " root"
	          << (roots_.size() == 1 ? "" : "s") << std::endl;

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
					// scan_dirs() expects stored-form paths
					// ("<root>/<dir>"); the rewatch helper still needs
					// absolute ones for inotify_add_watch().  MediaStore owns
					// the mapping, so ask it rather than reimplementing the
					// prefix arithmetic here.
					std::set<std::string> rel_dirs;
					for (auto& d : abs_dirs)
						rel_dirs.insert(store_.rel_path(d));
					std::thread([this, rel_dirs = std::move(rel_dirs),
					             abs_dirs = std::move(abs_dirs)]{
						// Only the scan is guarded: an exception escaping this
						// thread would terminate the server, but the bookkeeping
						// below must run either way — leaving scan_running_ set
						// would stop the watcher ever scanning again.
						try { store_.scan_dirs(rel_dirs); }
						catch (const std::exception& e) {
							std::cout << stamp() << "FolderWatcher: rescan "
							          << "aborted: " << e.what() << std::endl;
							}
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
					// Can't know what changed; mark for full scan.  A bare root
					// path maps to a bare root name, which scan_dirs() treats
					// as "lost track" and escalates to a full scan.
					for (const auto& r : roots_)
						changed_artists_.insert(r);
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

FolderWatcher::FolderWatcher(MediaStore& store, int debounce_ms)
	: store_(store), debounce_ms_(debounce_ms)
	{
	}

FolderWatcher::~FolderWatcher() { }
void FolderWatcher::start() { }
void FolderWatcher::stop()  { }

#endif

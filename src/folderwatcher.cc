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
			std::cout << stamp()
			          << "FolderWatcher: failed to watch " << path
			          << ": " << strerror(errno) << std::endl;
		return;
		}
	wd_to_path_[wd] = path;
	}

void FolderWatcher::remove_watch(int wd)
	{
	inotify_rm_watch(inotify_fd_, wd);   // may already be auto-removed; ignore error
	wd_to_path_.erase(wd);
	}

void FolderWatcher::add_watches_recursive(const std::string& root)
	{
	add_watch(root);
	std::error_code ec;
	for (auto& entry : std::filesystem::recursive_directory_iterator(root,
	                       std::filesystem::directory_options::skip_permission_denied, ec)) {
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

	add_watches_recursive(music_root_);
	std::cout << stamp() << "FolderWatcher: watching " << wd_to_path_.size()
	          << " directories under " << music_root_ << std::endl;

	thread_ = std::thread(&FolderWatcher::run, this);
	}

void FolderWatcher::stop()
	{
	if (pipe_fd_[1] != -1) {
		// Wake the poll() in run() so the thread exits cleanly.
		char b = 0;
		write(pipe_fd_[1], &b, 1);
		}
	if (thread_.joinable())
		thread_.join();
	if (inotify_fd_ != -1) { close(inotify_fd_); inotify_fd_ = -1; }
	if (pipe_fd_[0] != -1) { close(pipe_fd_[0]); pipe_fd_[0] = -1; }
	if (pipe_fd_[1] != -1) { close(pipe_fd_[1]); pipe_fd_[1] = -1; }
	}

void FolderWatcher::run()
	{
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
					std::thread([this]{
						store_.scan();
						scan_running_.store(false);
						}).detach();
					}
				last_event.reset();
				// Fall through to poll with timeout -1 until next event.
				}
			else {
				timeout_ms = (int)remaining;
				}
			}

		struct pollfd fds[2];
		fds[0] = { inotify_fd_, POLLIN, 0 };
		fds[1] = { pipe_fd_[0], POLLIN, 0 };

		int r = poll(fds, 2, timeout_ms);
		if (r < 0) {
			if (errno == EINTR) continue;
			std::cout << stamp() << "FolderWatcher: poll error: "
			          << strerror(errno) << std::endl;
			break;
			}

		// Stop signal via pipe.
		if (fds[1].revents & POLLIN)
			break;

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

				if (ev->mask & IN_IGNORED)
					continue;   // watch auto-removed, nothing to do

				if (ev->mask & IN_Q_OVERFLOW) {
					std::cout << stamp()
					          << "FolderWatcher: inotify queue overflow; "
					             "some events may have been missed"
					          << std::endl;
					// Still arm the debounce so we rescan.
					}

				// Arm / reset the debounce timer.
				last_event = Clock::now();

				// When a new subdirectory appears, watch it immediately so
				// events inside it are captured before the next scan runs.
				bool is_dir = ev->mask & IN_ISDIR;
				bool is_create = ev->mask & (IN_CREATE | IN_MOVED_TO);
				if (is_dir && is_create && ev->len > 0) {
					auto it = wd_to_path_.find(ev->wd);
					if (it != wd_to_path_.end())
						add_watch(it->second + "/" + ev->name);
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

#include "folderwatcher.hh"
#include "mediastore.hh"
#include "stamp.hh"

#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

// ---- Shared across platforms ----------------------------------------------

FolderWatcher::FolderWatcher(MediaStore& store, int debounce_ms)
	: store_(store), debounce_ms_(debounce_ms)
	{
	for (const auto& r : store_.roots()) {
		roots_.push_back(r.path);
		// At most one, and remembered rather than re-derived: every event has
		// to be told apart from a library one before it is turned into an
		// artist directory, and the uploads layout has two extra levels that
		// artist_dir_for_path() knows nothing about.
		if (r.type == "uploads") uploads_root_ = r.path;
		}
	}

FolderWatcher::~FolderWatcher()
	{
	stop();
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

bool FolderWatcher::is_uploads(const std::string& path) const
	{
	if (uploads_root_.empty()) return false;
	if (path == uploads_root_) return true;
	return path.size() > uploads_root_.size()
	       && path.compare(0, uploads_root_.size(), uploads_root_) == 0
	       && path[uploads_root_.size()] == '/';
	}

std::string FolderWatcher::artist_dir_for_path(const std::string& path) const
	{
	std::string owner = root_of(path);
	if (owner.empty()) return "";   // not under any library root

	fs::path p(path);
	fs::path root(owner);
	if (p == root) return "";       // the root itself; caller decides what to do

	// Walk up to the depth-1 child of the owning root.
	auto rel = p.lexically_relative(root);
	if (rel.empty()) return "";
	return (root / *rel.begin()).string();
	}

#ifdef __linux__

#include <sys/inotify.h>
#include <poll.h>
#include <unistd.h>
#include <fcntl.h>
#include <cerrno>
#include <cstring>
#include <chrono>
#include <optional>
#include <system_error>

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

void FolderWatcher::add_watch(const std::string& path)
	{
	int wd = inotify_add_watch(inotify_fd_, path.c_str(), WATCH_MASK);
	if (wd == -1) {
		// A directory that has just been deleted is the ordinary case here,
		// not a failure: every removal ends with a rescan of the folder that
		// went away, and the rewatch afterwards asks for a watch on a path
		// that is gone by definition. Reporting it — twice, with the walk
		// below — made a normal `rmdir` look like something had broken.
		if (errno == ENOENT)
			return;
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
	// Same reason as the ENOENT above: a rewatch of a directory that has just
	// been removed is expected, and its walk cannot start.
	if (ec && ec != std::errc::no_such_file_or_directory)
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
					// Taken with the set, and for the same reason: whatever
					// arrives after this point belongs to the next pass.
					bool uploads = uploads_dirty_;
					uploads_dirty_ = false;
					if (!abs_dirs.empty())
						std::cout << stamp() << "FolderWatcher: rescanning "
						          << abs_dirs.size() << " artist director"
						          << (abs_dirs.size() == 1 ? "y" : "ies")
						          << std::endl;
					if (uploads)
						std::cout << stamp()
						          << "FolderWatcher: reconciling uploads"
						          << std::endl;
					// scan_dirs() expects stored-form paths
					// ("<root>/<dir>"); the rewatch helper still needs
					// absolute ones for inotify_add_watch().  MediaStore owns
					// the mapping, so ask it rather than reimplementing the
					// prefix arithmetic here.
					std::set<std::string> rel_dirs;
					for (auto& d : abs_dirs)
						rel_dirs.insert(store_.rel_path(d));
					std::thread([this, uploads, rel_dirs = std::move(rel_dirs),
					             abs_dirs = std::move(abs_dirs)]{
						// Only the scan is guarded: an exception escaping this
						// thread would terminate the server, but the bookkeeping
						// below must run either way — leaving scan_running_ set
						// would stop the watcher ever scanning again.
						try {
							// The set is empty whenever the pass was an uploads
							// removal alone.  scan_dirs() would survive that,
							// but it would take a ScanGuard and print a totals
							// line for a scan of nothing.
							if (!rel_dirs.empty()) store_.scan_dirs(rel_dirs);
							if (uploads) store_.reconcile_uploads();
							}
						catch (const std::exception& e) {
							std::cout << stamp() << "FolderWatcher: rescan "
							          << "aborted: " << e.what() << std::endl;
							}
						catch (...) {
							std::cout << stamp() << "FolderWatcher: rescan "
							             "aborted: unknown exception" << std::endl;
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
					//
					// Not the uploads root: that name would escalate the same
					// way, so an overflow of uploads events alone would rescan
					// the entire library.  The uploads half of "we lost track"
					// is exactly what reconcile_uploads() answers.
					for (const auto& r : roots_) {
						if (is_uploads(r)) uploads_dirty_ = true;
						else               changed_artists_.insert(r);
						}
					last_event = Clock::now();
					continue;
					}

				// Record the affected artist dir, and arm the debounce timer
				// only if something was recorded. The full path of the changed
				// entry is what identifies it: when the watch is on a root,
				// parent + "/" + name IS the artist directory; deeper down, the
				// first component below the root is the same either way.
				{
				auto it = wd_to_path_.find(ev->wd);
				std::string parent = (it != wd_to_path_.end()) ? it->second : "";
				std::string name   = (ev->len > 0) ? ev->name : "";
				if (!parent.empty()) {
					std::string full = name.empty() ? parent : parent + "/" + name;
					if (is_uploads(full)) {
						// **Removals only, and never an artist directory.** An
						// uploads batch is <user>/<uuid>/<artist>/<album>, which
						// artist_dir_for_path() would read as <user>; and a
						// write here is a fetch in progress, whose batch
						// scan_batch() will index itself once it is complete.
						// Indexing it early would leave apply_batch_names()
						// renaming directories that already have rows.
						//
						// So the only question asked of the uploads root is
						// "has something gone", and the only answer is
						// reconcile_uploads().
						if (ev->mask & (IN_DELETE | IN_MOVED_FROM
						                | IN_DELETE_SELF | IN_MOVE_SELF)) {
							uploads_dirty_ = true;
							last_event = Clock::now();
							}
						}
					else {
						std::string artist = artist_dir_for_path(full);
						if (!artist.empty()) {
							changed_artists_.insert(artist);
							last_event = Clock::now();
							}
						}
					}
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

#elif defined(__APPLE__)

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

// FSEvents watches a path RECURSIVELY from a single stream, so unlike the
// inotify branch there is no per-directory registration: no startup walk, no
// watch map, and no rewatch pass after a scan.
//
// kqueue would have been the portable-to-BSD choice and was rejected on cost.
// EVFILT_VNODE needs one open fd per watched directory, and this watcher covers
// every directory at every depth — roots, artists, albums, disc subfolders — so
// a 3,000-album library means several thousand permanently-held descriptors.
// inotify pays one fd total for the same job. On macOS that difference is not
// merely wasteful:
//   * CastManager's discovery loop uses select(), and FD_SETSIZE is 1024. Once
//     the watcher holds more descriptors than that, sockets opened afterwards
//     get numbers past the end of an fd_set and FD_SET() corrupts the stack —
//     a bug that only appears above a library-size threshold.
//   * macOS ships a soft RLIMIT_NOFILE of 256, shared with httplib's workers,
//     SQLite handles, ffmpeg pipes and transcode-cache files.
//   * Open descriptors pin the volume, so a library on an external drive could
//     not be ejected while gaindrive was running.

static void fsevents_cb(ConstFSEventStreamRef, void* info, size_t n,
                        void* paths, const FSEventStreamEventFlags*,
                        const FSEventStreamEventId*)
	{
	// Flags are deliberately ignored. The only one that would change our
	// behaviour is kFSEventStreamEventFlagMustScanSubDirs (the daemon dropped
	// events, as IN_Q_OVERFLOW does on Linux), and it already reports the
	// subtree's directory — which maps to the same artist directory any
	// ordinary event under it would, so the rescan covers it either way.
	static_cast<FolderWatcher*>(info)->handle_paths(n, static_cast<char**>(paths));
	}

// FSEvents reports fully RESOLVED paths, while MediaStore::rel_path() — which
// drain_and_scan() has to call to get stored-form paths — keys off the
// CONFIGURED root path. Those differ whenever a root is reached through a
// symlink, and on macOS that is not exotic: /tmp and /var are symlinks into
// /private on every install. Without this translation root_of() would match
// nothing, every event would be discarded, and live rescan would do exactly
// nothing while logging no error at all.
std::string FolderWatcher::unresolve(const std::string& path) const
	{
	for (const auto& [canon, cfg] : canon_roots_) {
		if (path == canon) return cfg;
		if (path.size() > canon.size() && path.compare(0, canon.size(), canon) == 0
		        && path[canon.size()] == '/')
			return cfg + path.substr(canon.size());
		}
	return path;
	}

void FolderWatcher::handle_paths(size_t n, const char* const* paths)
	{
	bool spawn = false;
		{
		std::lock_guard<std::mutex> lk(changed_mutex_);
		for (size_t i = 0; i < n; i++) {
			std::string path = paths[i];
			// FSEvents reports directories with a trailing slash.
			while (path.size() > 1 && path.back() == '/') path.pop_back();
			path = unresolve(path);

			// The uploads root is watched for removals and nothing else — see
			// the ctor comment.  FSEvents carries no entry name and this branch
			// deliberately ignores the flags, so a removal cannot be told from
			// a write here; running the reconcile on both is harmless, because
			// it is one stat per indexed batch directory and it never indexes
			// anything.  What it must not do is reach artist_dir_for_path(),
			// which would read <uploads>/<user> as an artist directory, or the
			// bare-root fallback below, which would escalate to a full scan of
			// the whole library.
			if (is_uploads(path)) {
				std::cout << stamp() << "FolderWatcher: changed  " << path
				          << std::endl;
				uploads_dirty_ = true;
				continue;
				}

			std::string artist = artist_dir_for_path(path);
			if (artist.empty()) {
				// Either outside every root, or the root itself changed. Unlike
				// inotify, FSEvents carries no entry name, so we cannot tell
				// which artist appeared directly under a root — record the bare
				// root, which scan_dirs() reads as "lost track" and escalates to
				// a full scan of it. Rare in practice: a new artist directory
				// gets files written into it, and those events name it.
				std::string owner = root_of(path);
				if (owner.empty()) continue;
				artist = owner;
				}
			std::cout << stamp() << "FolderWatcher: changed  " << path << std::endl;
			changed_artists_.insert(artist);
			}
		if (changed_artists_.empty() && !uploads_dirty_) return;
		// One scan thread at a time. It loops until nothing new is left, so a
		// batch delivered mid-scan is picked up rather than dropped.
		if (!scan_running_) { scan_running_ = true; spawn = true; }
		}

	if (spawn)
		std::thread(&FolderWatcher::drain_and_scan, this).detach();
	}

void FolderWatcher::drain_and_scan()
	{
	while (true) {
		std::set<std::string> abs_dirs;
		bool uploads = false;
			{
			std::lock_guard<std::mutex> lk(changed_mutex_);
			abs_dirs.swap(changed_artists_);
			// Taken with the set, and for the same reason.
			uploads = uploads_dirty_;
			uploads_dirty_ = false;
			// Clearing the flag under the lock that guards the set closes the
			// handoff race: a batch arriving after this point sees scan_running_
			// false and starts a new thread, and one arriving before it is
			// already in abs_dirs.
			if (abs_dirs.empty() && !uploads) { scan_running_ = false; return; }
			}

		if (!abs_dirs.empty())
			std::cout << stamp() << "FolderWatcher: rescanning "
			          << abs_dirs.size() << " artist director"
			          << (abs_dirs.size() == 1 ? "y" : "ies") << std::endl;
		if (uploads)
			std::cout << stamp() << "FolderWatcher: reconciling uploads"
			          << std::endl;

		// This is a detached thread touching the DB: an escaping exception
		// calls std::terminate and takes the server down with it. Both arms log
		// — a silent catch turns a dead watcher into a mystery.
		try {
			// scan_dirs() expects stored-form paths ("<root>/<dir>"); MediaStore
			// owns that mapping, so ask it rather than reimplementing the prefix
			// arithmetic here.
			std::set<std::string> rel_dirs;
			for (auto& d : abs_dirs)
				rel_dirs.insert(store_.rel_path(d));
			// The set is empty whenever the pass was an uploads removal alone.
			if (!rel_dirs.empty()) store_.scan_dirs(rel_dirs);
			if (uploads) store_.reconcile_uploads();
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "FolderWatcher: rescan aborted: "
			          << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "FolderWatcher: rescan aborted: "
			             "unknown exception" << std::endl;
			}
		}
	}

void FolderWatcher::start()
	{
	if (stream_ || roots_.empty())
		return;   // already running, or nothing to watch

	// Resolve each root once so unresolve() can map FSEvents' reported paths
	// back. Done here rather than in the constructor because it touches the
	// filesystem, and a root that does not exist yet should not fail construction.
	canon_roots_.clear();
	for (const auto& r : roots_) {
		std::error_code ec;
		auto canon = fs::weakly_canonical(fs::path(r), ec);
		if (!ec && canon.string() != r)
			canon_roots_.emplace_back(canon.string(), r);
		}

	CFMutableArrayRef cf_paths =
		CFArrayCreateMutable(nullptr, (CFIndex)roots_.size(), &kCFTypeArrayCallBacks);
	if (!cf_paths) return;
	for (const auto& r : roots_) {
		CFStringRef s = CFStringCreateWithCString(nullptr, r.c_str(),
		                                          kCFStringEncodingUTF8);
		if (s) { CFArrayAppendValue(cf_paths, s); CFRelease(s); }
		}

	FSEventStreamContext ctx = { 0, this, nullptr, nullptr, nullptr };

	// The stream's latency IS the debounce: FSEvents coalesces everything inside
	// the window into one callback, which is what the inotify branch assembles
	// by hand out of poll() timeouts. kFSEventStreamCreateFlagNoDefer is
	// deliberately NOT set — without it the callback fires at the END of the
	// window, so a long file copy produces periodic batches instead of one
	// callback per write. kFSEventStreamCreateFlagWatchRoot additionally reports
	// a root that is itself moved or deleted.
	stream_ = FSEventStreamCreate(nullptr, fsevents_cb, &ctx, cf_paths,
	                              kFSEventStreamEventIdSinceNow,
	                              debounce_ms_ / 1000.0,
	                              kFSEventStreamCreateFlagWatchRoot);
	CFRelease(cf_paths);
	if (!stream_) {
		std::cout << stamp() << "FolderWatcher: FSEventStreamCreate failed"
		          << std::endl;
		return;
		}

	queue_ = dispatch_queue_create("gaindrive.folderwatcher", DISPATCH_QUEUE_SERIAL);
	FSEventStreamSetDispatchQueue((FSEventStreamRef)stream_, (dispatch_queue_t)queue_);

	if (!FSEventStreamStart((FSEventStreamRef)stream_)) {
		std::cout << stamp() << "FolderWatcher: FSEventStreamStart failed"
		          << std::endl;
		// Not stop(): FSEventStreamStop() must not be called on a stream that
		// never started, so unwind by hand instead.
		FSEventStreamInvalidate((FSEventStreamRef)stream_);
		FSEventStreamRelease((FSEventStreamRef)stream_);
		stream_ = nullptr;
		dispatch_release((dispatch_queue_t)queue_);
		queue_ = nullptr;
		return;
		}

	std::cout << stamp() << "FolderWatcher: watching " << roots_.size()
	          << " root" << (roots_.size() == 1 ? "" : "s")
	          << " recursively via FSEvents" << std::endl;
	}

void FolderWatcher::stop()
	{
	if (stream_) {
		auto s = (FSEventStreamRef)stream_;
		FSEventStreamStop(s);
		FSEventStreamInvalidate(s);
		FSEventStreamRelease(s);
		stream_ = nullptr;
		}
	if (queue_) {
		// Stop() guarantees no further callback is scheduled, but one may still
		// be running. Draining the serial queue before we return keeps a
		// callback from touching a half-destroyed FolderWatcher.
		// dispatch_sync_f rather than a block literal so this stays plain C++.
		dispatch_sync_f((dispatch_queue_t)queue_, nullptr, [](void*){});
		dispatch_release((dispatch_queue_t)queue_);
		queue_ = nullptr;
		}
	}

#else  // no live rescan on this platform

void FolderWatcher::start() { }
void FolderWatcher::stop()  { }

#endif

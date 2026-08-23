#include "transcodecache.hh"
#include "stamp.hh"
#include "proc.hh"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iostream>

#include <reproc++/reproc.hpp>

namespace fs = std::filesystem;

// A transcode that has not finished in this long is not going to.  The slot it
// holds is worth more than the hope.
static constexpr reproc::milliseconds FFMPEG_TIMEOUT(5 * 60 * 1000);
// How long a request waits for a free ffmpeg slot before giving up and letting
// the caller stream instead.  Long enough to ride out a burst, short enough
// that a client is not left staring at nothing.
static constexpr auto SLOT_TIMEOUT = std::chrono::seconds(20);
// Prune below this fraction of the cap rather than exactly to it, so a steady
// stream of requests does not trigger a delete on every single one.
static constexpr double PRUNE_TARGET = 0.9;

// ---- Entry ------------------------------------------------------------

TranscodeCache::Entry::Entry(TranscodeCache& owner, std::string key,
                             fs::path path, int64_t size, bool hit)
	: owner_(owner), key_(std::move(key)), path_(std::move(path)), size_(size),
	  hit_(hit)
	{
	}

TranscodeCache::Entry::~Entry()
	{
	owner_.release(key_);
	}

// ---- TranscodeCache ---------------------------------------------------

TranscodeCache::TranscodeCache(const fs::path& dir, int64_t cap_bytes, int jobs)
	: dir_(dir), cap_bytes_(cap_bytes), jobs_(std::max(1, jobs))
	{
	if (cap_bytes <= 0) {
		std::cout << stamp() << "transcode cache: disabled (zero size)"
		          << std::endl;
		return;
		}
	std::error_code ec;
	fs::create_directories(dir_, ec);
	if (ec || !fs::is_directory(dir_, ec)) {
		std::cout << stamp() << "transcode cache: disabled, cannot use "
		          << dir_ << ": " << ec.message() << std::endl;
		return;
		}
	// Probe writability now rather than discovering it on the first request,
	// when the only symptom would be that transcodes are mysteriously slow.
	fs::path probe = dir_ / ".writable";
	{
	std::ofstream f(probe);
	if (!f) {
		std::cout << stamp() << "transcode cache: disabled, "
		          << dir_ << " is not writable" << std::endl;
		return;
		}
	}
	fs::remove(probe, ec);

	// Half-written files from a crash: the rename is the only publish, so
	// anything still named .part is known garbage.
	int stale = 0;
	for (const auto& e : fs::directory_iterator(dir_, ec))
		if (e.path().extension() == ".part" && fs::remove(e.path(), ec))
			stale++;
	if (stale)
		std::cout << stamp() << "transcode cache: removed " << stale
		          << " stale .part file(s)" << std::endl;

	enabled_ = true;
	std::cout << stamp() << "transcode cache: " << dir_
	          << " cap=" << (cap_bytes_ / (1024 * 1024)) << "MB"
	          << " jobs=" << jobs_ << std::endl;
	}

std::shared_ptr<const TranscodeCache::Entry> TranscodeCache::get_or_build(
	const std::string& key, const std::string& ext,
	const std::vector<std::string>& argv,
	const std::string& out_placeholder)
	{
	if (!enabled_) return {};

	fs::path final = dir_ / (key + ext);
	std::error_code ec;

	std::unique_lock<std::mutex> lock(mu_);

	// Someone else may be building this exact entry; wait for them rather than
	// running a second ffmpeg over the same input.
	while (building_.count(key)) {
		if (cv_.wait_for(lock, SLOT_TIMEOUT) == std::cv_status::timeout)
			break;
		}

	if (fs::exists(final, ec)) {
		int64_t size = static_cast<int64_t>(fs::file_size(final, ec));
		if (!ec && size > 0) {
			// Touch so prune()'s LRU ordering reflects use, not creation.
			fs::last_write_time(final, fs::file_time_type::clock::now(), ec);
			in_use_[key]++;
			return std::make_shared<Entry>(*this, key, final, size, true);
			}
		}

	// Still being built after the wait above timed out.  Two ffmpegs writing
	// the same .part file would corrupt it, so let this request stream instead
	// of racing the builder.
	if (building_.count(key)) {
		std::cout << stamp() << "transcode cache: " << key
		          << " still building, streaming instead" << std::endl;
		return {};
		}

	// Claim the build, then wait for a free ffmpeg slot.
	building_.insert(key);
	bool got_slot = cv_.wait_for(lock, SLOT_TIMEOUT,
		[this]{ return running_ < jobs_; });
	if (!got_slot) {
		building_.erase(key);
		cv_.notify_all();
		std::cout << stamp() << "transcode cache: no free slot for " << key
		          << ", streaming instead" << std::endl;
		return {};
		}
	running_++;
	lock.unlock();

	fs::path part = dir_ / (key + ext + ".part");
	std::vector<std::string> cmd = argv;
	for (auto& a : cmd)
		if (a == out_placeholder) a = part.string();

	bool ok = run_ffmpeg(cmd, part);

	lock.lock();
	running_--;
	building_.erase(key);

	std::shared_ptr<const Entry> entry;
	if (ok) {
		// The rename is the only way a file becomes visible under its final
		// name, so a reader can never observe a partial transcode.
		fs::rename(part, final, ec);
		if (ec) {
			std::cout << stamp() << "transcode cache: rename failed for "
			          << key << ": " << ec.message() << std::endl;
			fs::remove(part, ec);
			ok = false;
			}
		else {
			int64_t size = static_cast<int64_t>(fs::file_size(final, ec));
			in_use_[key]++;
			entry = std::make_shared<Entry>(*this, key, final, size, false);
			}
		}
	else
		fs::remove(part, ec);

	cv_.notify_all();
	lock.unlock();

	if (entry) prune();
	return entry;
	}

bool TranscodeCache::run_ffmpeg(const std::vector<std::string>& argv,
                                const fs::path& part)
	{
	{
	std::string line;
	for (const auto& a : argv) { line += ' '; line += a; }
	std::cout << stamp() << "transcode cache: ffmpeg:" << line << std::endl;
	}

	auto t0 = std::chrono::steady_clock::now();

	reproc::process proc;
	reproc::options opts;
	// stderr to a temp file, not a pipe: nothing here drains a pipe, and a
	// chatty ffmpeg would block forever once the pipe buffer filled.
	std::unique_ptr<FILE, int(*)(FILE*)> errf(std::tmpfile(), &std::fclose);
	if (errf) {
		opts.redirect.err.type = reproc::redirect::type::file_;
		opts.redirect.err.file = errf.get();
		}
	else
		opts.redirect.err.type = reproc::redirect::type::discard;
	// Nothing reads stdout — ffmpeg writes the file itself.
	opts.redirect.out.type = reproc::redirect::type::discard;

	if (auto ec = proc.start(argv, opts)) {
		std::cout << stamp() << "transcode cache: ffmpeg launch failed: "
		          << ec.message() << std::endl;
		return false;
		}

	auto [status, ec] = proc.wait(FFMPEG_TIMEOUT);
	if (ec) {
		std::cout << stamp() << "transcode cache: ffmpeg wait failed ("
		          << ec.message() << "), killing" << std::endl;
		proc.kill();
		proc.wait(reproc::infinite);
		return false;
		}

	std::error_code fec;
	int64_t size = fs::exists(part, fec)
	    ? static_cast<int64_t>(fs::file_size(part, fec)) : 0;

	if (status != 0 || size <= 0) {
		std::cout << stamp() << "transcode cache: ffmpeg failed status="
		          << status << " bytes=" << size << std::endl;
		if (errf) {
			// The reason ffmpeg gives is on its last line or two; without this
			// a failed transcode is indistinguishable from a missing file.
			std::string tail = stderr_tail(errf.get());
			if (!tail.empty())
				std::cout << stamp() << "transcode cache: ffmpeg stderr: "
				          << tail << std::endl;
			}
		return false;
		}

	auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
	    std::chrono::steady_clock::now() - t0).count();
	std::cout << stamp() << "transcode cache: built " << part.stem().string()
	          << " " << size << " bytes in " << ms << " ms" << std::endl;
	return true;
	}

void TranscodeCache::release(const std::string& key)
	{
	std::lock_guard<std::mutex> lock(mu_);
	auto it = in_use_.find(key);
	if (it != in_use_.end() && --it->second <= 0)
		in_use_.erase(it);
	cv_.notify_all();
	}

void TranscodeCache::prune()
	{
	struct Item {
		fs::path              path;
		int64_t               size;
		fs::file_time_type    mtime;
		std::string           key;
		};

	std::error_code ec;
	std::vector<Item> items;
	int64_t total = 0;
	for (const auto& e : fs::directory_iterator(dir_, ec)) {
		if (!e.is_regular_file(ec)) continue;
		if (e.path().extension() == ".part") continue;
		int64_t size = static_cast<int64_t>(e.file_size(ec));
		if (ec) continue;
		total += size;
		items.push_back({ e.path(), size, e.last_write_time(ec),
		                  e.path().stem().string() });
		}
	if (total <= cap_bytes_) return;

	std::sort(items.begin(), items.end(),
		[](const Item& a, const Item& b){ return a.mtime < b.mtime; });

	int64_t target = static_cast<int64_t>(cap_bytes_ * PRUNE_TARGET);
	int     removed = 0;
	int64_t freed   = 0;
	for (const auto& item : items) {
		if (total <= target) break;
		{
		// Never delete a file a response is still reading.  On Linux the
		// unlinked file would survive for an open fd, but serve_direct reopens
		// by path for each chunk, so it really would break.
		std::lock_guard<std::mutex> lock(mu_);
		if (in_use_.count(item.key)) continue;
		}
		if (fs::remove(item.path, ec)) {
			total -= item.size;
			freed += item.size;
			removed++;
			}
		}
	if (removed)
		std::cout << stamp() << "transcode cache: pruned " << removed
		          << " entries, freed " << (freed / 1024) << " kB, now "
		          << (total / (1024 * 1024)) << " MB" << std::endl;
	}

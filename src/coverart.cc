#include "coverart.hh"

#include <algorithm>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#include <reproc++/drain.hpp>
#include <reproc++/reproc.hpp>

#include "imagescale.hh"
#include "mediastore.hh"
#include "stamp.hh"

namespace {

// Nearly every size gaindrive's own clients ask for is a rung. The web client
// asks in *device* pixels, so each of its boxes contributes two: a 48px
// player thumbnail, an 80px grid cell, an 80px and a 120px artist portrait
// and a 320px hero give 64/80/128/320 on a 1x screen and 96/160/640 on a 2x
// one, the 2x portrait's 240 being the one that rounds — on to 256, which is
// also its mediaSession artwork (an OS hint rather than a box, so it is not
// doubled). Android adds 144/288/512 and iOS 144/288/800. The rest fill the
// gaps closely enough that rounding up is never a visible loss.
constexpr int LADDER[] = {32,  64,  80,  96,  128, 144, 160, 192, 256,
                          288, 320, 400, 512, 640, 800, 1024, 1600};

// A source no larger than this is served as it stands. Beyond the top rung
// there is nothing useful left to do: the client asked for something bigger
// than the largest thumbnail we keep, so it wants the picture itself.
constexpr int LADDER_MAX = 1600;

constexpr reproc::milliseconds FFMPEG_TIMEOUT(30 * 1000);

// Last resort for an image stb refuses. It is the same fork this whole change
// exists to remove, but the result is stored like any other thumbnail, so it
// happens once per (image, size) in the lifetime of the library rather than
// once per request. Serving the original unscaled instead would mean shipping
// a multi-megabyte scan to an 80px grid cell, on every request, for ever.
std::optional<std::string> scale_with_ffmpeg(const std::string& path, int size)
	{
	// The short edge becomes `size` and the long one follows, matching
	// imagescale's Fit::Short — the two paths produce bytes under the same
	// cache key, so a rung has to mean the same thing in both.
	//
	// force_original_aspect_ratio=increase says that in one word and is not
	// usable: it enlarges a source smaller than the box, and never upscaling
	// is the other half of the contract. So the orientation is branched on by
	// hand and each edge clamped to the source's own. -2 rather than -1 keeps
	// the derived edge even, which the mjpeg encoder wants.
	//
	// LONG_EDGE_LIMIT is deliberately not mirrored here. Expressing it costs
	// a nested min in both branches, and this path is reached only for an
	// image stb cannot decode at all — so a wider-than-4:1 one of those is a
	// case that has never occurred rather than one being tolerated.
	const std::string px = std::to_string(size);
	std::vector<std::string> args = {
		"ffmpeg", "-v", "quiet", "-i", path,
		"-vf", "scale=w='if(gt(iw,ih),-2,min(iw," + px + "))'"
		       ":h='if(gt(iw,ih),min(ih," + px + "),-2)'",
		"-frames:v", "1", "-f", "mjpeg", "pipe:1"
		};

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	opts.deadline          = FFMPEG_TIMEOUT;

	if (proc.start(args, opts)) return std::nullopt;

	std::string          out;
	reproc::sink::string sink(out);
	// drain() checks the error code itself, which is the point: reading by
	// hand needs `n == 0 || err`, because reproc wraps a negative return into
	// a size_t and a plain `n == 0` test lets a huge length reach write().
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);

	if (ec || wec || status != 0 || out.empty()) return std::nullopt;
	return out;
	}

std::string cache_key(const std::string& key, int size, int64_t stamp)
	{
	return key + '\x1f' + std::to_string(size) + '\x1f' + std::to_string(stamp);
	}

}   // namespace

CoverArtCache::CoverArtCache(MediaStore& store, std::size_t mem_bytes,
                              int decode_jobs)
	: store_(store),
	  mem_cap_(mem_bytes),
	  // Decoding is bounded well below the 32-slot HTTP pool on purpose.
	  // Thirty-two simultaneous decodes of 3000x3000 JPEGs is well over a
	  // gigabyte of transient RGB buffers on a machine that may also be
	  // scanning, and it is not faster: past a handful the work is already
	  // CPU-bound. This is what makes the memory ceiling of the feature a
	  // number that can be stated.
	  jobs_(decode_jobs > 0
	            ? decode_jobs
	            : std::max(2u, std::min(4u, std::thread::hardware_concurrency())))
	{
	}

int CoverArtCache::ladder_size(int requested)
	{
	if (requested <= 0)          return 0;
	if (requested > LADDER_MAX)  return 0;
	for (int r : LADDER)
		if (requested <= r) return r;
	return 0;
	}

CoverArtCache::Stats CoverArtCache::stats()
	{
	Stats s{};
	{
	std::lock_guard<std::mutex> lock(mem_mu_);
	s.mem_used = mem_used_;
	s.entries  = lru_.size();
	}
	{
	std::lock_guard<std::mutex> lock(build_mu_);
	s.building = running_;
	}
	// Set once in the constructor, safe to read unlocked.
	s.mem_cap = mem_cap_;
	s.jobs    = jobs_;
	return s;
	}

std::optional<CoverArtCache::Result> CoverArtCache::mem_get(const std::string& ckey)
	{
	std::lock_guard<std::mutex> lk(mem_mu_);
	auto it = index_.find(ckey);
	if (it == index_.end()) return std::nullopt;
	lru_.splice(lru_.begin(), lru_, it->second);
	return it->second->val;
	}

void CoverArtCache::mem_put(const std::string& ckey, const Result& v)
	{
	if (v.bytes.size() > mem_cap_ / 4) return;   // never let one entry dominate

	std::lock_guard<std::mutex> lk(mem_mu_);
	if (auto it = index_.find(ckey); it != index_.end()) {
		mem_used_ -= it->second->val.bytes.size();
		lru_.erase(it->second);
		index_.erase(it);
		}
	lru_.push_front(Entry{ckey, v});
	index_[ckey] = lru_.begin();
	mem_used_   += v.bytes.size();

	while (mem_used_ > mem_cap_ && !lru_.empty()) {
		auto& back = lru_.back();
		mem_used_ -= back.val.bytes.size();
		index_.erase(back.ckey);
		lru_.pop_back();
		}
	}

void CoverArtCache::invalidate(const std::string& key)
	{
	std::lock_guard<std::mutex> lk(mem_mu_);
	// Entries are keyed by path *and* stamp, so a changed source usually
	// misses on its own. This exists for the case where it does not: an
	// upload replacing a cover twice inside one filesystem timestamp tick,
	// which is exactly when a person is watching to see the new image.
	const std::string prefix = key + '\x1f';
	for (auto it = lru_.begin(); it != lru_.end(); ) {
		if (it->ckey.compare(0, prefix.size(), prefix) == 0) {
			mem_used_ -= it->val.bytes.size();
			index_.erase(it->ckey);
			it = lru_.erase(it);
			}
		else ++it;
		}
	}

std::optional<CoverArtCache::Result> CoverArtCache::scaled(const Source& src,
                                                            int size)
	{
	if (size <= 0 || src.key.empty()) return std::nullopt;

	const std::string ckey = cache_key(src.key, size, src.stamp);
	if (auto hit = mem_get(ckey)) return hit;

	// The stored row is compared against the source's stamp here rather than
	// in the query: only the caller knows what the stamp is now, so only the
	// caller can call a disagreement a miss.
	if (auto row = store_.get_cover_thumb(src.key, size);
	    row && row->source_stamp == src.stamp) {
		if (row->status == "unscalable") return std::nullopt;
		Result r{row->mime, row->bytes, row->width, row->height};
		mem_put(ckey, r);
		return r;
		}

	// Single flight. Without it a page that shows the same image twice, or
	// two clients opening the same album, decode it once each for nothing.
	std::shared_ptr<Pending> mine, theirs;
	{
	std::unique_lock<std::mutex> lk(build_mu_);
	if (auto it = building_.find(ckey); it != building_.end()) {
		theirs = it->second;
		}
	else {
		mine = std::make_shared<Pending>();
		building_[ckey] = mine;
		}
	}

	if (theirs) {
		std::unique_lock<std::mutex> lk(build_mu_);
		theirs->cv.wait(lk, [&] { return theirs->done; });
		if (theirs->none) return std::nullopt;
		return theirs->val;
		}

	// From here on `mine` must be published and erased whatever happens, or
	// every later request for this key waits for ever.
	std::optional<Result> out;
	try {
		// The slot is released by a destructor, not by a statement at the end
		// of the try: build() reads the database and SQLiteCpp throws, and a
		// slot leaked once per exception would wedge the cache for good after
		// `jobs_` of them.
		struct Slot
			{
			CoverArtCache& c;
			explicit Slot(CoverArtCache& owner) : c(owner)
				{
				std::unique_lock<std::mutex> lk(c.build_mu_);
				c.slot_cv_.wait(lk, [&] { return c.running_ < c.jobs_; });
				++c.running_;
				}
			~Slot()
				{
				{
				std::lock_guard<std::mutex> lk(c.build_mu_);
				--c.running_;
				}
				c.slot_cv_.notify_one();
				}
			} slot(*this);

		out = build(src, size);
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "cover art: " << src.key << ": " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "cover art: " << src.key
		          << ": unknown error while scaling" << std::endl;
		}

	{
	std::lock_guard<std::mutex> lk(build_mu_);
	mine->done = true;
	mine->none = !out.has_value();
	if (out) mine->val = *out;
	building_.erase(ckey);
	}
	mine->cv.notify_all();

	if (out) mem_put(ckey, *out);
	return out;
	}

std::optional<CoverArtCache::Result> CoverArtCache::build(const Source& src,
                                                           int size)
	{
	std::string raw;
	if (src.kind == Source::Kind::Blob) {
		raw = src.bytes;
		}
	else {
		std::ifstream f(src.abs_path, std::ios::binary);
		if (!f) {
			std::cout << stamp() << "cover art: cannot open " << src.abs_path
			          << std::endl;
			return std::nullopt;
			}
		raw.assign(std::istreambuf_iterator<char>(f),
		           std::istreambuf_iterator<char>());
		}
	if (raw.empty()) return std::nullopt;

	// Fit::Short, because every surface that asks for a size crops the result
	// to a square: the album row, the hero, the player bar and both artist
	// portraits in the web client, and the same shapes on Android and iOS.
	// The long edge overshoots and the crop throws it away, which is the point
	// — fitting the long edge instead leaves the *client* upscaling a poster
	// to fill its cell, however sharp what we sent it was.
	auto s = imagescale::scale_to_fit(raw, size, imagescale::Fit::Short);

	// Already small enough. Not stored: a copy of the source under a thumbnail
	// key would duplicate the file in the database to save a stat and a header
	// parse. The caller serves the original.
	if (s.ok && s.from_source) return std::nullopt;

	if (s.ok) {
		Result r{s.mime, std::move(s.bytes), s.width, s.height};
		store_.store_cover_thumb(src.key, size, src.stamp, "ok", r.mime,
		                         r.width, r.height, r.bytes);
		return r;
		}

	// stb could not read it. That is rare — it handles progressive JPEG,
	// 16-bit PNG and Adobe CMYK — but a portrait fetched from a provider can
	// be a format the scanner would never have indexed.
	if (src.kind == Source::Kind::File) {
		if (auto jpg = scale_with_ffmpeg(src.abs_path, size)) {
			auto dims = imagescale::probe(*jpg);
			Result r{"image/jpeg", std::move(*jpg),
			         dims ? dims->width : 0, dims ? dims->height : 0};
			std::cout << stamp() << "cover art: " << src.key
			          << ": stb could not decode it (" << s.error
			          << "), used ffmpeg" << std::endl;
			store_.store_cover_thumb(src.key, size, src.stamp, "ok", r.mime,
			                         r.width, r.height, r.bytes);
			return r;
			}
		}

	// Record the failure, or every request for this image would try again for
	// ever. A negative result is a result — the same reasoning that puts an
	// 'unmatched' row in video_meta.
	std::cout << stamp() << "cover art: " << src.key << ": cannot scale ("
	          << s.error << "); serving it at full size" << std::endl;
	store_.store_cover_thumb(src.key, size, src.stamp, "unscalable", "", 0, 0,
	                         std::string());
	return std::nullopt;
	}

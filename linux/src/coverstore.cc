#include "coverstore.hh"
#include "apiclient.hh"
#include "md5.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <glibmm/main.h>
#include <glibmm/miscutils.h>
#include <gdk/gdk.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <vector>

namespace {
	// Enough for a few screens of album rows at both sizes.
	const size_t memory_entries = 400;
	// The phones' limit too. Trimmed to 90% so that a full cache is not
	// trimmed again on every write.
	const std::uintmax_t disk_limit = std::uintmax_t(128) << 20;
	// The web client's numbers: fifteen seconds apart, giving up after ten
	// minutes, long enough for any backlog of lookups a person waits on.
	const unsigned retry_seconds = 15;
	const unsigned max_tries     = 40;

	std::filesystem::path cache_dir()
		{
		return std::filesystem::path(Glib::get_user_cache_dir()) / "gaindrive-player" / "covers";
		}

	// An entry is the server's ETag on the first line, then the image. One
	// file rather than a sidecar, so that trim() cannot part the two.
	bool read_entry(const std::filesystem::path& p, std::string& etag, std::string& data)
		{
		std::ifstream in(p, std::ios::binary);
		if(!in || !std::getline(in, etag)) return false;
		std::ostringstream ss;
		ss << in.rdbuf();
		data = ss.str();
		return !data.empty();
		}

	// The centred square of a texture, the way a record sleeve is shown: a
	// poster or a photo is cropped top and bottom or left and right rather
	// than drawn smaller with bars beside it. Takes ownership of `tex`.
	GdkTexture *square(GdkTexture *tex)
		{
		if(!tex) return tex;
		int w = gdk_texture_get_width(tex), h = gdk_texture_get_height(tex);
		if(w==h) return tex;
		int side = std::min(w, h);
		gsize stride = gsize(w)*4;
		std::vector<guchar> pixels(stride*h);
		gdk_texture_download(tex, pixels.data(), stride);
		g_object_unref(tex);
		// Bytes of the full image, read from the square's first pixel with the
		// full image's stride: no copy of the square is needed.
		gsize offset = gsize((h-side)/2)*stride + gsize((w-side)/2)*4;
		GBytes *bytes = g_bytes_new(pixels.data(), pixels.size());
		GBytes *sub   = g_bytes_new_from_bytes(bytes, offset, pixels.size()-offset);
		// gdk_texture_download's format.
		GdkTexture *out = gdk_memory_texture_new(side, side, GDK_MEMORY_DEFAULT, sub, stride);
		g_bytes_unref(sub);
		g_bytes_unref(bytes);
		return out;
		}

	bool write_entry(const std::filesystem::path& p, const std::string& etag, const std::string& data)
		{
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		auto tmp = p;
		tmp += ".tmp";
		{
		std::ofstream out(tmp, std::ios::binary);
		// A header cannot hold a newline, so none can end up in the etag.
		out << etag << '\n' << data;
		if(!out) return false;
		}
		std::filesystem::rename(tmp, p, ec);
		return !ec;
		}

	// Deletes the least recently used files until the cache is at most
	// `target` bytes, and returns what is left. "Used" is the mtime, which a
	// read moves forward: access times are not reliable under relatime.
	std::uintmax_t trim(std::uintmax_t target)
		{
		struct File {
			std::filesystem::path           path;
			std::filesystem::file_time_type when;
			std::uintmax_t                  size;
		};
		std::vector<File> files;
		std::uintmax_t total = 0;
		std::error_code ec;
		for(std::filesystem::directory_iterator it(cache_dir(), ec), end; !ec && it!=end; it.increment(ec)) {
			std::error_code fec;
			if(!it->is_regular_file(fec)) continue;
			auto size = it->file_size(fec);
			if(fec) continue;
			auto when = it->last_write_time(fec);
			if(fec) continue;
			total += size;
			files.push_back({it->path(), when, size});
			}
		if(total<=target) return total;
		std::sort(files.begin(), files.end(), [](const File& a, const File& b) {
			return a.when<b.when;
			});
		for(const auto& f: files) {
			if(total<=target) break;
			// A reader racing this just fetches the cover again.
			if(std::filesystem::remove(f.path, ec)) total -= f.size;
			}
		std::cout << stamp() << "cover cache trimmed to " << (total>>20) << " MB" << std::endl;
		return total;
		}

	// Called from the pool after each write. The directory is measured once,
	// on the first write of a run, and then only counted.
	void note_written(std::uintmax_t size)
		{
		static std::mutex     mtx;
		static bool           measured = false;
		static std::uintmax_t bytes    = 0;
		std::lock_guard<std::mutex> lock(mtx);
		if(!measured) {
			bytes    = trim(disk_limit);
			measured = true;
			}
		else
			bytes += size;
		if(bytes>disk_limit)
			bytes = trim(disk_limit/10*9);
		}
}

CoverStore::CoverStore(WorkPool& pool)
	: pool_(pool)
	{
	}

void CoverStore::set_api(std::shared_ptr<ApiClient> api)
	{
	api_ = std::move(api);
	memory_.clear();
	ages_.clear();
	pending_.clear();
	missing_.clear();
	++epoch_;
	}

Glib::RefPtr<Gdk::Texture> CoverStore::get(const std::string& cover_id, int px, std::string& key,
                                           int index)
	{
	key.clear();
	if(!api_ || cover_id.empty()) return {};

	const auto& s = api_->session();
	key = s.server_key + "|" + cover_id + "|" + std::to_string(px);
	if(index>0) key += "|" + std::to_string(index);

	auto it = memory_.find(key);
	if(it!=memory_.end()) {
		ages_.splice(ages_.begin(), ages_, it->second.age);
		return it->second.texture;
		}
	auto miss = missing_.find(key);
	if(miss!=missing_.end()) {
		if(std::chrono::steady_clock::now()<miss->second) return {};
		missing_.erase(miss);
		}
	if(pending_.count(key)) return {};
	pending_.insert(key);

	// .cover, not the .img of the entries before ETags: those are never read,
	// and trim() clears them out as the oldest files.
	auto file = cache_dir() / (md5_hex(key) + ".cover");
	Params p = {{"id", cover_id}, {"size", std::to_string(px)}};
	if(index>0) p.push_back({"index", std::to_string(index)});
	auto url  = api_->url("getCoverArt", p);
	pool_.submit([this, key, file, url, epoch=epoch_]() {
		// Always asked, never trusted: the server's ETag covers the file and
		// its size, and a rescan can hand a cover id to another album, so a
		// stored copy may be another album's art. A 304 costs no image.
		std::string etag, data;
		bool stored = read_entry(file, etag, data);
		auto f = ApiClient::get_if_changed(url, stored ? etag : "");
		std::error_code ec;
		if(f.status==200) {
			data = std::move(f.body);
			if(write_entry(file, f.etag, data))
				note_written(data.size());
			}
		else if(f.status!=304 && f.status!=0) {
			// Gone, or not there yet; either way the stored copy is not it.
			if(stored) std::filesystem::remove(file, ec);
			data.clear();
			}
		// A 304, or no answer at all: what is stored is the best there is.
		else if(stored)
			// Marks it as recently used, for trim().
			std::filesystem::last_write_time(file, std::filesystem::file_time_type::clock::now(), ec);
		// Not fetched at all, as opposed to fetched and undecodable.
		bool failed    = data.empty();
		int  keep_miss = f.miss_max_age;
		// Decoded here rather than on the main loop; only the C object is
		// made off the main thread, the C++ wrapper is made on it.
		GdkTexture *tex = nullptr;
		if(!data.empty()) {
			GBytes *bytes = g_bytes_new(data.data(), data.size());
			GError *err = nullptr;
			tex = square(gdk_texture_new_from_bytes(bytes, &err));
			g_bytes_unref(bytes);
			if(err) {
				std::cout << stamp() << "cover " << key << ": " << err->message << std::endl;
				g_error_free(err);
				// A cached file that does not decode would otherwise be served
				// for ever.
				std::filesystem::remove(file, ec);
				}
			}
		on_ui([this, key, tex, epoch, failed, keep_miss]() {
			auto texture = Glib::wrap(tex);
			// A logout in between: the key may now mean another account.
			if(epoch!=epoch_) return;
			pending_.erase(key);
			if(!texture) {
				if(keep_miss>0)
					missing_[key] = std::chrono::steady_clock::now() + std::chrono::seconds(keep_miss);
				else if(failed)
					signal_later.emit(key);
				return;
				}
			remember(key, texture);
			signal_ready.emit(key, texture);
			});
		});
	return {};
	}

void CoverStore::remember(const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex)
	{
	ages_.push_front(key);
	memory_[key] = Entry{tex, ages_.begin()};
	while(memory_.size()>memory_entries) {
		memory_.erase(ages_.back());
		ages_.pop_back();
		}
	}

CoverImage::CoverImage(CoverStore& store, int css_px)
	: store_(store), css_px_(css_px)
	{
	set_pixel_size(css_px);
	set_halign(Gtk::Align::CENTER);
	set_valign(Gtk::Align::CENTER);
	// Clips the cropped image to the rounded corners.
	set_overflow(Gtk::Overflow::HIDDEN);
	add_css_class("cover");
	// mem_fun rather than a lambda: only then does sigc disconnect the slot
	// when this widget is destroyed, which a ListView does as it shrinks.
	store_.signal_ready.connect(sigc::mem_fun(*this, &CoverImage::ready));
	store_.signal_later.connect(sigc::mem_fun(*this, &CoverImage::later));
	}

CoverImage::~CoverImage()
	{
	retry_.disconnect();
	}

void CoverImage::ready(const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex)
	{
	if(key==wanted_)
		set(tex);
	}

void CoverImage::show_cover(const std::string& cover_id, int index)
	{
	retry_.disconnect();
	cover_id_ = cover_id;
	index_    = index;
	tries_    = 0;
	fetch();
	}

void CoverImage::fetch()
	{
	// Device pixels, at 1x or 2x only: each distinct size is another thumbnail
	// the server keeps, so fractional scales are rounded up rather than
	// followed. The same rule as the web client's coverPx().
	int px = css_px_ * (get_scale_factor()>1 ? 2 : 1);
	auto tex = store_.get(cover_id_, px, wanted_, index_);
	if(tex) set(tex);
	else    clear();
	}

void CoverImage::later(const std::string& key)
	{
	// Several images showing the same cover each keep their own count; the
	// store fetches it once whichever asks first.
	if(key!=wanted_ || retry_.connected() || tries_>=max_tries) return;
	++tries_;
	retry_ = Glib::signal_timeout().connect_seconds([this]() {
		// Not on screen: its pane was switched away, so nobody is waiting.
		if(get_mapped()) fetch();
		return false;
		}, retry_seconds);
	}

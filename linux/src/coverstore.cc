#include "coverstore.hh"
#include "apiclient.hh"
#include "md5.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <glibmm/miscutils.h>
#include <gdk/gdk.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <vector>

namespace {
	// Enough for a few screens of album rows at both sizes.
	const size_t memory_entries = 400;

	std::filesystem::path cache_dir()
		{
		return std::filesystem::path(Glib::get_user_cache_dir()) / "gaindrive-player" / "covers";
		}

	bool read_file(const std::filesystem::path& p, std::string& out)
		{
		std::ifstream in(p, std::ios::binary);
		if(!in) return false;
		std::ostringstream ss;
		ss << in.rdbuf();
		out = ss.str();
		return !out.empty();
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

	void write_file(const std::filesystem::path& p, const std::string& data)
		{
		std::error_code ec;
		std::filesystem::create_directories(p.parent_path(), ec);
		auto tmp = p;
		tmp += ".tmp";
		{
		std::ofstream out(tmp, std::ios::binary);
		out << data;
		if(!out) return;
		}
		std::filesystem::rename(tmp, p, ec);
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
	if(pending_.count(key)) return {};
	pending_.insert(key);

	auto file = cache_dir() / (md5_hex(key) + ".img");
	Params p = {{"id", cover_id}, {"size", std::to_string(px)}};
	if(index>0) p.push_back({"index", std::to_string(index)});
	auto url  = api_->url("getCoverArt", p);
	pool_.submit([this, key, file, url, epoch=epoch_]() {
		std::string data, error;
		if(!read_file(file, data)) {
			if(ApiClient::get_bytes(url, data, error))
				write_file(file, data);
			else
				data.clear();
			}
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
				std::error_code ec;
				std::filesystem::remove(file, ec);
				}
			}
		on_ui([this, key, tex, epoch]() {
			auto texture = Glib::wrap(tex);
			// A logout in between: the key may now mean another account.
			if(epoch!=epoch_) return;
			pending_.erase(key);
			if(!texture) return;
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
	}

void CoverImage::ready(const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex)
	{
	if(key==wanted_)
		set(tex);
	}

void CoverImage::show_cover(const std::string& cover_id, int index)
	{
	// Device pixels, at 1x or 2x only: each distinct size is another thumbnail
	// the server keeps, so fractional scales are rounded up rather than
	// followed. The same rule as the web client's coverPx().
	int px = css_px_ * (get_scale_factor()>1 ? 2 : 1);
	auto tex = store_.get(cover_id, px, wanted_, index);
	if(tex) set(tex);
	else    clear();
	}

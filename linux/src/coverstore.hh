#pragma once

#include <gdkmm/texture.h>
#include <gtkmm/image.h>
#include <sigc++/signal.h>

#include <chrono>
#include <list>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>

class ApiClient;
class WorkPool;

// Cover art from getCoverArt, cropped to its centred square: a memory LRU of decoded textures in front of a
// disk cache in ~/.cache/gaindrive-player/covers, capped at 128 MB as on the
// phones. Main thread only; fetching and decoding happen on the pool.
class CoverStore {
	public:
		CoverStore();
		~CoverStore();

		// Clears the memory cache too: covers are keyed by server, but a new
		// login may be the same server with a different account.
		void set_api(std::shared_ptr<ApiClient> api);

		// The texture if it is in memory. Otherwise null, a fetch is started,
		// and signal_ready fires with the same key once it has landed.
		// `index` picks one of an album's further images (getAlbumImages).
		// With `fetch` false, only what is in memory; nothing is started.
		Glib::RefPtr<Gdk::Texture> get(const std::string& cover_id, int px, std::string& key,
		                               int index=0, bool fetch=true);

		sigc::signal<void(const std::string& key, const Glib::RefPtr<Gdk::Texture>&)> signal_ready;
		// A fetch that failed but may work later: a portrait or online cover
		// the server is still looking up (a no-store 404), or no connection.
		sigc::signal<void(const std::string& key)> signal_later;

	private:
		void remember(const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex);

		struct Entry {
			Glib::RefPtr<Gdk::Texture>       texture;
			std::list<std::string>::iterator age;
		};

		// Its own threads, apart from the API calls': a screenful of covers
		// queued ahead of getAlbum kept a reopened album's tracks waiting.
		std::unique_ptr<WorkPool>              pool_;
		std::shared_ptr<ApiClient>             api_;
		std::unordered_map<std::string, Entry> memory_;
		std::list<std::string>                 ages_;      // most recent first
		std::set<std::string>                  pending_;
		// Covers the server says do not exist, until the time it says that
		// answer may be kept. Without this every scroll past an artist with
		// no portrait would ask again.
		std::map<std::string, std::chrono::steady_clock::time_point> missing_;
		unsigned                               epoch_=0;   // bumped by set_api
};

// A square cover of a fixed size, which fills itself in when the image lands.
// An Image rather than a Picture: an Image draws at its pixel size whatever
// the paintable, where a Picture asks for the paintable's own size. The store
// has already cut the texture square, so nothing is letterboxed.
// A ListView recycles its row widgets, so the picture remembers which cover it
// was last asked for and ignores any other that arrives.
// A cover that is not there yet is asked for again every 15 seconds for up to
// ten minutes while it is on screen, as the web client does.
// Inside a scrolled view, nothing is fetched until the image is within its
// visible area: a ListView keeps up to 200 rows bound, not just those shown,
// and an artist's 200 albums then all fetched their covers at once, ahead of
// the album actually opened.
class CoverImage : public Gtk::Image {
	public:
		CoverImage(CoverStore& store, int css_px);
		~CoverImage() override;

		void show_cover(const std::string& cover_id, int index=0);

	private:
		void fetch();
		// Waits for the image to scroll into view, then fetches.
		void watch();
		void stop_watching();
		bool on_screen() const;
		void ready(const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex);
		void later(const std::string& key);

		CoverStore&      store_;
		int              css_px_;
		std::string      cover_id_, wanted_;
		int              index_=0;
		unsigned         tries_=0;
		sigc::connection retry_;
		Gtk::Widget     *scrolled_=nullptr;
		sigc::connection scroll_watch_, size_watch_, map_watch_;
		guint            tick_=0;
};

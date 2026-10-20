#pragma once

#include "model.hh"

#include <giomm/liststore.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/overlay.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/searchentry.h>
#include <gtkmm/singleselection.h>

#include <map>
#include <functional>
#include <memory>
#include <set>

class ApiClient;
class CoverStore;
class CoverImage;
class InfoBlock;
struct ApiResult;
struct AppSettings;
struct ServerConfig;

// One row widget for every pane: a heading, or an item with an optional
// cover, a leading number, a title over a subtitle, a trailing note and up to
// three buttons. Unused parts are hidden; a ListView recycles these between
// row kinds.
class RowWidget : public Gtk::Box {
	public:
		RowWidget(CoverStore *covers, int cover_px);
		~RowWidget() override;

		void show_heading(const std::string& text);
		void show_item(const std::string& cover_id, const std::string& lead,
		               const std::string& title, const std::string& subtitle,
		               const std::string& trail);
		// The list button either offers "add to playlist" or, inside a
		// playlist the user owns, "remove from it".
		enum class ListButton { none, add, remove };
		void show_actions(bool star, bool starred, ListButton list, bool queue);
		void set_playing(bool on);
		// Shows the pane's own widget in place of the row's parts, taking
		// it from whichever row had it; drop_extra gives it back.
		void show_extra(Gtk::Widget& w);
		void drop_extra();

		// The row currently bound here.
		Glib::RefPtr<ListRow> row;
		Gtk::Button           star, list, queue;
		ListButton            list_mode=ListButton::none;
		// The pane's set of live row widgets, which this one leaves when it
		// is destroyed. GTK does not promise the child is still reachable
		// from the factory's teardown, so that cannot do the bookkeeping.
		std::shared_ptr<std::set<RowWidget*>> registry;

	private:
		Gtk::Label  heading_;
		Gtk::Box    item_;
		Gtk::Widget *extra_=nullptr;
		CoverImage *cover_=nullptr;
		Gtk::Label  lead_, title_, subtitle_, trail_;
		Gtk::Box    text_;
};

// A titled, scrolling list of ListRows, as on the web client's pane strip.
// The signals are what every pane may ask of the window.
class Pane : public Gtk::Box {
	public:
		// `boxed` puts boxes above and below the list, scrolling with it. It
		// costs the list its virtualisation, so only short lists use it.
		Pane(CoverStore& covers, int cover_px, bool boxed);

		virtual void clear();
		// A star set or cleared anywhere: rows showing that item follow.
		virtual void star_changed(ListRow::Kind kind, const ItemRef& ref, bool on);
		void set_playing(const ItemRef& song);
		void unselect();
		// Reselects the first row `match` accepts, as a click would, once
		// there are rows: now if there are, else with the next list that
		// arrives. For reopening the last view; `done` says whether it was
		// found.
		void restore(std::function<bool(const ListRow&)> match,
		             std::function<void(bool found)> done={});
		void cancel_restore();
		bool restoring() const;
		// "← label" before the title, when the pane is the leftmost of a
		// narrow strip; empty hides it.
		void set_back(const std::string& label);
		sigc::signal<void()>                                  signal_back;

		sigc::signal<void(const ApiResult&)>                  signal_failed;
		sigc::signal<void(const std::vector<Song>&, size_t)>  signal_play;
		sigc::signal<void(const Song&)>                       signal_enqueue;
		sigc::signal<void(ListRow::Kind, const ItemRef&, bool)> signal_star;
		sigc::signal<void(const Song&, Gtk::Widget&)>         signal_pick_playlist;
		// Open an album in the track pane, and play `play_id` from it if set.
		sigc::signal<void(const std::string& album_id, const std::string& play_id)> signal_open_album;
		sigc::signal<void(const Artist&)>                     signal_open_artist;

	protected:
		void set_title(const std::string& t);
		void set_rows(const std::vector<Glib::RefPtr<ListRow>>& rows);
		// Shown in place of an empty list; empty text hides it.
		void set_empty(const std::string& text);
		virtual void bind(RowWidget& w, const ListRow& row) = 0;
		virtual void activated(const ListRow&) {}
		virtual void remove_from_list(const ListRow&) {}
		// Whether the row stands for what plays now; by default the song row
		// of the current track.
		virtual bool is_playing(const ListRow& row) const;
		void rebind_all();
		void report(const ApiResult& r);

		// The usual song row outside an album: title, "artist · album".
		void bind_song_hit(RowWidget& w, const Song& s, const std::string& trail);
		void bind_album_hit(RowWidget& w, const Album& a);

		CoverStore&                           covers_;
		Gtk::Box                              header_;
		Gtk::Button                           back_;
		Gtk::Label                            title_;
		Gtk::Box                              top_, bottom_;
		unsigned                              gen_=0;

	private:
		std::shared_ptr<std::set<RowWidget*>> widgets_;
		Gtk::Label                            empty_;
		Gtk::ScrolledWindow                   scroll_;
		Gtk::Box                              scroll_box_;
		Glib::RefPtr<Gio::ListStore<ListRow>> store_;
		Glib::RefPtr<Gtk::SingleSelection>    selection_;
		Gtk::ListView                         list_;
		ItemRef                               playing_;
		std::function<bool(const ListRow&)>   restore_match_;
		std::function<void(bool)>             restore_done_;

		void apply_restore();
};

// ---- Library: artists, an artist's albums, an album's tracks ---------------

class ArtistPane : public Pane {
	public:
		explicit ArtistPane(CoverStore& covers);

		void load(std::shared_ptr<ApiClient> api);
		bool loaded() const;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		void load_artists(std::shared_ptr<ApiClient> api, unsigned gen, bool categories,
		                  bool artists, std::vector<Glib::RefPtr<ListRow>> rows);

		// Whether the server sorts its roots by contentType (gaindrive does).
		bool typed_=false;
		bool loaded_=false;
};

class AlbumPane : public Pane {
	public:
		AlbumPane(CoverStore& covers, AppSettings& settings);
		~AlbumPane() override;

		void show_artist(std::shared_ptr<ApiClient> api, const Artist& artist);
		const std::string& artist_id() const;
		void clear() override;

		sigc::signal<void(const Album&)> signal_album;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		void render();
		void label_sort();
		std::string section() const;

		AppSettings&               settings_;
		Gtk::Button                sort_;
		std::unique_ptr<InfoBlock> bio_;
		std::vector<Album>         albums_;
		std::string                artist_id_;
		bool                       category_=false;
};

class TrackPane : public Pane {
	public:
		explicit TrackPane(CoverStore& covers);
		~TrackPane() override;

		// Everything else about the album comes from getAlbum.
		void show_album(std::shared_ptr<ApiClient> api, const std::string& album_id,
		                const std::string& play_id="");
		void clear() override;

		// Marks a chapter of `film` as playing; -1 for none. 0-based, as
		// the film's own chapter list counts.
		void set_chapter(const ItemRef& film, int index);

		// The album's own details, once getAlbum has answered.
		sigc::signal<void(const Album&)> signal_loaded;
		// A chapter row was picked: play songs[index] from `start`.
		sigc::signal<void(const std::vector<Song>&, size_t, double start)> signal_play_chapter;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;
		bool is_playing(const ListRow& row) const override;

	private:
		struct Chapter {
			double      start=0;
			int         duration=0;
			std::string name;
		};
		void show_rows();

		void show_image(int index);
		void load_texts(std::shared_ptr<ApiClient> api, const std::string& album_id);
		void show_text(size_t index);

		std::vector<Song>           songs_;
		// getAlbumChapters, by film id: from the scan's index, so a film
		// whose markers are only inside its container has none here.
		std::map<std::string, std::vector<Chapter>> chapters_;
		ItemRef                     chapter_film_;
		int                         chapter_=-1;
		Album                       album_;
		std::shared_ptr<ApiClient>  api_;

		Gtk::Overlay                hero_box_;
		std::unique_ptr<CoverImage> hero_;
		Gtk::Button                 img_prev_, img_next_;
		int                         image_=0, images_=1;
		std::unique_ptr<InfoBlock>  notes_;

		// Liner notes: the .txt files in the album's folder.
		Gtk::Box                    texts_, texts_nav_;
		Gtk::Button                 text_prev_, text_next_;
		Gtk::Label                  text_name_, text_body_;
		std::vector<std::string>    text_files_;
		size_t                      text_=0;
};

// ---- The other sidebar sections --------------------------------------------

// Playlists, then starred albums and tracks, as on the web client.
class PlaylistsPane : public Pane {
	public:
		explicit PlaylistsPane(CoverStore& covers);

		void load(std::shared_ptr<ApiClient> api);
		void star_changed(ListRow::Kind kind, const ItemRef& ref, bool on) override;

		sigc::signal<void(const Playlist&)> signal_playlist;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		std::shared_ptr<ApiClient> api_;
};

// One playlist's tracks, in the middle pane.
class PlaylistPane : public Pane {
	public:
		explicit PlaylistPane(CoverStore& covers);

		void show_playlist(std::shared_ptr<ApiClient> api, const Playlist& pl);
		void reload();

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;
		void remove_from_list(const ListRow& row) override;

	private:
		std::shared_ptr<ApiClient> api_;
		Playlist                   playlist_;
		std::vector<Song>          songs_;
		bool                       owner_=false;
};

class RecentsPane : public Pane {
	public:
		explicit RecentsPane(CoverStore& covers);

		void load(std::shared_ptr<ApiClient> api);

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;
};

class SearchPane : public Pane {
	public:
		explicit SearchPane(CoverStore& covers);

		void set_api(std::shared_ptr<ApiClient> api);
		void focus_entry();
		void clear() override;
		// The search text; setting it runs the search, as typing does.
		std::string query() const;
		void        set_query(const std::string& q);

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		void run();

		Gtk::SearchEntry           entry_;
		std::shared_ptr<ApiClient> api_;
};

class SettingsPane : public Pane {
	public:
		SettingsPane(CoverStore& covers, AppSettings& settings,
		             const std::vector<ServerConfig>& servers);

		void load();

		sigc::signal<void(const std::string& key)> signal_category;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		AppSettings&                     settings_;
		const std::vector<ServerConfig>& servers_;
};

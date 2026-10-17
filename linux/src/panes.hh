#pragma once

#include "model.hh"

#include <giomm/liststore.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listview.h>
#include <gtkmm/scrolledwindow.h>
#include <gtkmm/singleselection.h>

#include <map>
#include <memory>

class ApiClient;
class CoverStore;
class CoverImage;
struct ApiResult;
namespace Gtk { class ListItem; }

// One row widget for all three panes: a heading, or an item with an optional
// cover, a leading number, a title over a subtitle, and a trailing note.
// Unused parts are hidden; a ListView recycles these between row kinds.
class RowWidget : public Gtk::Box {
	public:
		RowWidget(CoverStore *covers, int cover_px);

		void show_heading(const std::string& text);
		void show_item(const std::string& cover_id, const std::string& lead,
		               const std::string& title, const std::string& subtitle,
		               const std::string& trail);

	private:
		Gtk::Label  heading_;
		Gtk::Box    item_;
		CoverImage *cover_=nullptr;
		Gtk::Label  lead_, title_, subtitle_, trail_;
		Gtk::Box    text_;
};

// A titled, scrolling list of ListRows, as on the web client's pane strip.
class Pane : public Gtk::Box {
	public:
		Pane(CoverStore& covers, int cover_px, bool hero);

		void clear();

		// Raised for any failed request, for the window to report, or to go
		// back to the login screen when the credentials were refused.
		sigc::signal<void(const ApiResult&)> signal_failed;

	protected:
		void set_title(const std::string& t);
		void set_rows(const std::vector<Glib::RefPtr<ListRow>>& rows);
		virtual void bind(RowWidget& w, const ListRow& row) = 0;
		virtual void activated(const ListRow&) {}

		CoverStore&                           covers_;
		Gtk::Box                              header_;
		Gtk::Label                            title_;
		// Above the list, scrolling with it; only the track pane has one.
		std::unique_ptr<CoverImage>           hero_;
		// Bumped by each load, so the answer to a superseded one is dropped.
		unsigned                              gen_=0;

	private:
		Gtk::ScrolledWindow                   scroll_;
		Gtk::Box                              scroll_box_;
		Glib::RefPtr<Gio::ListStore<ListRow>> store_;
		Glib::RefPtr<Gtk::SingleSelection>    selection_;
		Gtk::ListView                         list_;
};

class ArtistPane : public Pane {
	public:
		explicit ArtistPane(CoverStore& covers);

		void load(std::shared_ptr<ApiClient> api);

		sigc::signal<void(const Artist&)> signal_artist;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		void load_artists(std::shared_ptr<ApiClient> api, unsigned gen, bool categories,
		                  bool artists, std::vector<Glib::RefPtr<ListRow>> rows);

		// Whether the server sorts its roots by contentType (gaindrive does).
		bool typed_=false;
};

class AlbumPane : public Pane {
	public:
		explicit AlbumPane(CoverStore& covers);

		void show_artist(std::shared_ptr<ApiClient> api, const Artist& artist);

		sigc::signal<void(const Album&)> signal_album;

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		void render();
		void label_sort();

		Gtk::Button        sort_;
		std::vector<Album> albums_;
		bool               category_=false;
		// Per section, as the web client keeps it: a category of radio shows
		// reads best by name, an artist's records by year.
		std::map<bool, bool> by_name_;
};

class TrackPane : public Pane {
	public:
		explicit TrackPane(CoverStore& covers);

		void show_album(std::shared_ptr<ApiClient> api, const Album& album);

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
};

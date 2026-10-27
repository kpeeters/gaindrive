#pragma once

#include "model.hh"

#include <gdkmm/texture.h>
#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>

#include <memory>
#include <string>
#include <vector>

class ApiClient;

// The track pane's edit mode, as on the web client: the album's title and
// artist, its cover, and each track's number, title, disc and year. The pane
// places the widgets and hides its own listing; this fills them and saves.
class AlbumEditor {
	public:
		AlbumEditor();
		~AlbumEditor();

		// `numbers` is what the listing shows before each song, so that a
		// number left alone is not sent, even where it was only counted.
		void start(std::shared_ptr<ApiClient> api, const Album& album,
		           const std::vector<Song>& songs, const std::vector<std::string>& numbers);
		// Leaves edit mode without saving; answers still on their way are
		// ignored.
		void stop();
		bool editing() const;

		Gtk::Box        fields{Gtk::Orientation::HORIZONTAL, 6};  // album and artist
		Gtk::Box        buttons{Gtk::Orientation::HORIZONTAL, 6}; // Save and Cancel
		Gtk::MenuButton pencil;                                   // over the cover
		Gtk::Grid       tracks;                                   // in the listing's place

		// A cover picked, to show until Save; null to show the album's own.
		sigc::signal<void(const Glib::RefPtr<Gdk::Texture>&)> signal_preview;
		sigc::signal<void()> signal_cancelled;

		struct Saved {
			// What changed shows elsewhere or reorders the listing, so the
			// album has to come back from the server: `album` and `artist`
			// are where it now is, new ids after a rename.
			bool                reload=false;
			bool                cover=false;
			ItemRef             album, artist;
			// Otherwise the songs, with their new titles.
			std::vector<Song>   songs;
		};
		sigc::signal<void(const Saved&)> signal_saved;

	private:
		struct Row {
			size_t      song;
			Gtk::Entry *num, *title, *disc, *year;
			std::string num0, title0, disc0, year0;
		};
		void add_year_all(int grid_row, int disc);
		void choose_file();
		void apply_url();
		void save();
		void save_track(size_t from);
		void rename();
		void finish(const nlohmann::json& moved);
		void show_errors(const std::string& heading, const std::string& text);

		std::shared_ptr<ApiClient> api_;
		Album                      album_;
		std::vector<Song>          songs_;
		std::vector<Row>           rows_;
		Gtk::Entry                 name_, artist_;
		std::string                name0_, artist0_;
		Gtk::Button                save_, cancel_;
		bool                       editing_=false;
		unsigned                   gen_=0;

		// The cover dialog's contents, as the web's: a file, or a URL.
		Gtk::Popover               cover_pop_;
		Gtk::Box                   cover_box_{Gtk::Orientation::VERTICAL, 8};
		Gtk::Button                choose_, apply_;
		Gtk::Label                 file_name_, url_label_, cover_note_;
		Gtk::Entry                 url_;
		std::string                cover_file_, cover_url_;

		// Filled while saving.
		std::vector<std::string>   errors_;
		bool                       order_changed_=false, year_changed_=false, cover_changed_=false;
};

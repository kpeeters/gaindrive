#pragma once

#include "coverstore.hh"
#include "loginview.hh"
#include "panes.hh"
#include "panestrip.hh"
#include "playback.hh"
#include "playerbar.hh"
#include "serverconfig.hh"
#include "settings.hh"
#include "settingspage.hh"
#include "trackinfo.hh"
#include "videoview.hh"
#include "workpool.hh"

#include <memory>

#include <gtkmm/applicationwindow.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/overlay.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>
#include <gtkmm/togglebutton.h>

#include <memory>
#include <vector>

class ApiClient;
struct ApiResult;
struct Session;

class Mpris;

class MainWindow : public Gtk::ApplicationWindow {
	public:
		MainWindow();
		~MainWindow() override;

	private:
		void restore_size();
		void save_size();
		void show_login(const std::string& error);
		void login(const std::string& url, const std::string& user, const std::string& password);
		void logged_in(const ApiResult& r, const Session& s);
		void start_session(const Session& s);
		// Back to the login view, forgetting nothing on disk.
		void end_session(const std::string& error);
		void remove_server(const std::string& key);
		void failed(const ApiResult& r);
		void notify(const std::string& text);

		// The sidebar's sections, by their stack names.
		void show_section(const std::string& name);
		void open_artist(const Artist& a);
		void open_album(const std::string& album_id, const std::string& play_id);
		void album_loaded(const Album& a);
		void play(const std::vector<Song>& songs, size_t index);
		void star(ListRow::Kind kind, const ItemRef& ref, bool on);
		void close_video();
		void install_tune_closers();
		void close_cards();
		void build_tabs();
		void set_narrow(bool narrow);
		// One pane back in a narrow strip.
		void back();
		// The last view, kept in view.json and reopened at login.
		void save_view();
		void restore_view();
		// Shows pane `depth` and puts the back links right.
		void go(int depth);
		void update_back();
		void fullscreen_changed();
		std::vector<Pane*> all_panes();

		// First, so that it is destroyed last: its threads are joined while
		// everything they might still touch is alive.
		WorkPool                   pool_;
		AppSettings                settings_;
		CoverStore                 covers_;
		std::shared_ptr<ApiClient> api_;
		std::vector<ServerConfig>  servers_;
		Playback                   playback_;

		Gtk::HeaderBar             header_;
		Gtk::Stack                 stack_;
		LoginView                  login_;

		// Says when the window is narrow enough for the phone-like layout.
		NarrowBox                  browse_;
		Gtk::Revealer              notice_;
		Gtk::Label                 notice_text_;
		sigc::connection           notice_timer_;
		// main_ with the volume and equaliser card floating over it.
		Gtk::Overlay               body_;
		Gtk::Box                   main_;
		Gtk::Box                   sidebar_;
		Gtk::ListBox               nav_;
		Gtk::ScrolledWindow        queue_scroll_;
		QueueView                  queue_;

		// The panes, or a film in their place.
		Gtk::Stack                 content_;
		VideoView                  video_;
		Gtk::Stack                 left_, middle_;
		ArtistPane                 artists_;
		PlaylistsPane              playlists_;
		RecentsPane                recents_;
		SearchPane                 search_;
		SettingsPane               settings_pane_;
		AlbumPane                  albums_;
		PlaylistPane               playlist_;
		SettingsPage               settings_page_;
		TrackPane                  tracks_;
		// Three columns, as on the web client: a section, what was picked in
		// it, and an album's tracks; fewer in a smaller window. After the
		// panes, so it goes first and unparents them while they still exist.
		PaneStrip                  strip_;
		PlayerBar                  bar_;
		// Floats over the panes beside the equaliser's card; one at a time.
		TrackInfo                  info_card_;
		// The sections, below the bar, in place of the sidebar when narrow.
		Gtk::Box                   tabs_;
		std::vector<Gtk::ToggleButton*> tab_buttons_;
		bool                       narrow_=false;
		// What the track pane's back link names: the album's artist.
		std::string                tracks_back_;
		// What is open, as reopened at the next start: each item by id and
		// name, since a rescan can hand an id to something else.
		struct View {
			std::string section="library";
			std::string artist_id, artist_name;
			std::string playlist_id, playlist_name;
			std::string settings;
			std::string album_id, album_title;
		};
		View                       view_;
		// Set while an album reopens by id alone: the title it must have.
		std::string                expect_album_;
		// While restore_view() runs, before its restores are queued.
		bool                       restoring_view_=false;
		// Last, so it goes first, before what it reports on.
		std::unique_ptr<Mpris>     mpris_;
};

#pragma once

#include "coverstore.hh"
#include "loginview.hh"
#include "panes.hh"
#include "playback.hh"
#include "playerbar.hh"
#include "serverconfig.hh"
#include "settings.hh"
#include "settingspage.hh"
#include "videoview.hh"
#include "workpool.hh"

#include <gtkmm/applicationwindow.h>
#include <gtkmm/headerbar.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/revealer.h>
#include <gtkmm/stack.h>

#include <memory>
#include <vector>

class ApiClient;
struct ApiResult;
struct Session;

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

		Gtk::Box                   browse_;
		Gtk::Revealer              notice_;
		Gtk::Label                 notice_text_;
		sigc::connection           notice_timer_;
		Gtk::Box                   main_;
		Gtk::Box                   sidebar_;
		Gtk::ListBox               nav_;
		Gtk::ScrolledWindow        queue_scroll_;
		QueueView                  queue_;

		// The panes, or a film in their place.
		Gtk::Stack                 content_;
		VideoView                  video_;
		// Three columns, as on the web client: a section, what was picked in
		// it, and an album's tracks.
		Gtk::Box                   panes_;
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
		PlayerBar                  bar_;
};

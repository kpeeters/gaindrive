#pragma once

#include "coverstore.hh"
#include "loginview.hh"
#include "panes.hh"
#include "serverconfig.hh"
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
		void logout();
		void failed(const ApiResult& r);
		void notify(const std::string& text);

		// First, so that it is destroyed last: its threads are joined while
		// everything they might still touch is alive.
		WorkPool                   pool_;
		CoverStore                 covers_;
		std::shared_ptr<ApiClient> api_;
		std::vector<ServerConfig>  servers_;

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
		Gtk::Button                logout_;
		Gtk::Box                   panes_;
		ArtistPane                 artists_;
		AlbumPane                  albums_;
		TrackPane                  tracks_;
};

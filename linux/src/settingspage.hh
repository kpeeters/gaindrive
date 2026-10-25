#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>

#include <memory>
#include <vector>

class Clients;
struct AppSettings;
struct ServerConfig;

// One settings category, in the middle pane.
class SettingsPage : public Gtk::Box {
	public:
		SettingsPage(AppSettings& settings, const std::vector<ServerConfig>& servers,
		             const Clients& clients);

		void show(const std::string& key);
		void clear();
		// As Pane::set_back.
		void set_back(const std::string& label);
		sigc::signal<void()> signal_back;

		// A setting changed, so summaries elsewhere are out of date.
		sigc::signal<void()> signal_changed;
		// The theme setting changed and wants applying.
		sigc::signal<void()> signal_theme;
		// The Remove button of a server, by its key.
		sigc::signal<void(const std::string&)> signal_remove_server;
		// "Log in again" on a server whose login was refused, by its key.
		sigc::signal<void(const std::string&)> signal_login_server;
		sigc::signal<void()>                   signal_add_server;

	private:
		void show_playback();
		void show_appearance();
		Gtk::Box* add_row(const char *label, Gtk::Widget& w);
		void show_servers();
		void show_about();
		Gtk::Label& add_label(const std::string& text, const char *css=nullptr);

		AppSettings&                     settings_;
		const std::vector<ServerConfig>& servers_;
		const Clients&                   clients_;
		Gtk::Box                         header_, content_;
		Gtk::Button                      back_;
		Gtk::Label                       title_;
};

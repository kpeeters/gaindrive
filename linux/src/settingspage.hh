#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>
#include <gtkmm/scrolledwindow.h>

#include <functional>

#include <memory>
#include <vector>

class Clients;
namespace Gtk { class ListBox; }
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
		// A server's name was edited, by its key; empty for the host.
		sigc::signal<void(const std::string&, const std::string&)> signal_rename_server;
		// Album merging was turned on or off, so the lists are out of date.
		sigc::signal<void()>                   signal_merge;

	private:
		void show_playback();
		void show_appearance();
		// A GNOME preference group: a bordered list, with a title above it
		// when given one.
		Gtk::ListBox* add_group(const char *title);
		// A row of `group`: title, dim subtitle, `suffix` on the right.
		// Returns the row's box.
		Gtk::Box* add_row(Gtk::ListBox *group, const std::string& title,
		                  const std::string& subtitle, Gtk::Widget *suffix);
		void add_switch_row(Gtk::ListBox *group, const std::string& title,
		                    const std::string& subtitle, bool on,
		                    std::function<void(bool)> changed);
		// Small dim text under a group.
		void add_hint(const std::string& text);
		void show_servers();
		void show_about();

		AppSettings&                     settings_;
		const std::vector<ServerConfig>& servers_;
		const Clients&                   clients_;
		Gtk::Box                         header_, content_;
		Gtk::ScrolledWindow              scroll_;
		Gtk::Button                      back_;
		Gtk::Label                       title_;
};

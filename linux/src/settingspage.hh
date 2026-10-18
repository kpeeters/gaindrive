#pragma once

#include <gtkmm/box.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>

#include <memory>
#include <vector>

class ApiClient;
struct AppSettings;
struct ServerConfig;

// One settings category, in the middle pane.
class SettingsPage : public Gtk::Box {
	public:
		SettingsPage(AppSettings& settings, const std::vector<ServerConfig>& servers);

		void show(const std::string& key, std::shared_ptr<ApiClient> api);
		void clear();

		// A setting changed, so summaries elsewhere are out of date.
		sigc::signal<void()> signal_changed;
		// The Remove button of a server, by its key.
		sigc::signal<void(const std::string&)> signal_remove_server;

	private:
		void show_playback();
		void show_servers(std::shared_ptr<ApiClient> api);
		void show_about(std::shared_ptr<ApiClient> api);
		Gtk::Label& add_label(const std::string& text, const char *css=nullptr);

		AppSettings&                     settings_;
		const std::vector<ServerConfig>& servers_;
		Gtk::Box                         header_, content_;
		Gtk::Label                       title_;
};

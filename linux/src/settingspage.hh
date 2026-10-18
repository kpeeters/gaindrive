#pragma once

#include <gtkmm/box.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/label.h>

#include <memory>

class ApiClient;
struct AppSettings;

// One settings category, in the middle pane.
class SettingsPage : public Gtk::Box {
	public:
		explicit SettingsPage(AppSettings& settings);

		void show(const std::string& key, std::shared_ptr<ApiClient> api);
		void clear();

		// A setting changed, so summaries elsewhere are out of date.
		sigc::signal<void()> signal_changed;

	private:
		void show_playback();
		void show_about(std::shared_ptr<ApiClient> api);
		Gtk::Label& add_label(const std::string& text, const char *css=nullptr);

		AppSettings& settings_;
		Gtk::Box     header_, content_;
		Gtk::Label   title_;
};

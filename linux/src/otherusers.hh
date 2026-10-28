#pragma once

#include "model.hh"

#include <gtkmm/box.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

class ApiClient;

// What everyone else is listening to, from getNowPlaying, at the bottom of
// the sidebar as on the web client.
class OtherUsers : public Gtk::Box {
	public:
		OtherUsers();
		~OtherUsers() override;

		// Each server with the name to show beside its users, empty when
		// there is only one. None stops the polling and hides the list.
		void set_servers(std::vector<std::pair<std::shared_ptr<ApiClient>, std::string>> servers);

		sigc::signal<void(const ItemRef& album, const std::string& song_id)> signal_open_album;

	private:
		struct Entry {
			std::string user, title, artist, song_id;
			ItemRef     album;
		};
		void poll();
		void rebuild();

		std::vector<std::pair<std::shared_ptr<ApiClient>, std::string>> servers_;
		// Per server, so one that does not answer keeps what it showed.
		std::vector<std::vector<Entry>> shown_;
		// Servers without getNowPlaying (Bandcamp), left out of later polls.
		std::vector<bool>               lacking_;
		std::vector<Entry>              rows_;
		Gtk::Label                      heading_;
		Gtk::ListBox                    list_;
		sigc::connection                timer_;
		unsigned                        gen_=0;
};

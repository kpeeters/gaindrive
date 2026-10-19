#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/label.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

class ApiClient;
class CoverImage;
class CoverStore;
class Playback;
class WorkPool;

// The web client's track info, as a card the window floats above the player
// bar beside the equaliser's: what the file is, how it is reaching the
// speaker, and a link to it for the web client. "Start at" is the position
// when the card opened, as there, so the label and the link agree.
class TrackInfo : public Gtk::Box {
	public:
		TrackInfo(CoverStore& covers, Playback& playback, WorkPool& pool);
		~TrackInfo() override;

		void set_api(std::shared_ptr<ApiClient> api);
		// Fills the card in for what plays now.
		void refresh();
		// The id of the track it shows, so a queue change alone need not
		// refresh it.
		const std::string& shown() const;
		void set_narrow(bool narrow);

		// The close button in its corner.
		sigc::signal<void()> signal_close;

	private:
		using Rows = std::vector<std::pair<std::string, std::string>>;
		void fill(Gtk::Grid& grid, const Rows& rows);
		void update_link();

		Playback&                   playback_;
		WorkPool&                   pool_;
		std::shared_ptr<ApiClient>  api_;
		std::unique_ptr<CoverImage> cover_;

		Gtk::Box         head_, head_text_, share_row_, starts_;
		Gtk::Label       title_, sub_;
		Gtk::Button      close_, copy_;
		Gtk::Grid        file_, playback_grid_;
		Gtk::Entry       link_;
		Gtk::CheckButton at_, at_chapter_;

		std::string      shown_, base_;
		int              at_seconds_=0;
		double           chapter_start_=0;
		// Bumped by each refresh, so replies about an earlier track drop.
		unsigned         gen_=0;
		// Replies that come back after the card has gone are dropped too.
		std::shared_ptr<bool> alive_;
		sigc::connection copied_;
};

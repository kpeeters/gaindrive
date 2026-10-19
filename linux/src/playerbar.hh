#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/scale.h>

#include <chrono>
#include <memory>

struct AppSettings;
class CastButton;
class TuneButton;
class TunePanel;
class CoverImage;
class CoverStore;
class Playback;

// The bar along the bottom, as on the web client: cover, title over artist,
// previous / play / next, a seek slider and the time.
class PlayerBar : public Gtk::Box {
	public:
		PlayerBar(CoverStore& covers, Playback& playback, AppSettings& settings);
		~PlayerBar() override;

		// Below the narrow-window width: the seek bar on a second row.
		void set_narrow(bool narrow);

		// The volume and equaliser card, for the window to float above the
		// bar, and the button that opens it.
		TunePanel&  tune_panel();
		TuneButton& tune_button();

		// The cover was clicked: open the playing album.
		sigc::signal<void()> signal_cover;

	private:
		void track_changed();
		void state_changed(bool playing);
		bool tick();

		Playback&                   playback_;
		Gtk::Box                    row_, row2_;
		std::unique_ptr<CoverImage> cover_;
		Gtk::Box                    info_;
		Gtk::Label                  title_, artist_;
		Gtk::Button                 prev_, play_, next_;
		Gtk::Scale                  seek_;
		Gtk::Label                  time_;
		std::unique_ptr<CastButton> cast_;
		std::unique_ptr<TuneButton> tune_;
		sigc::connection            timer_;
		// The slider is not moved under a hand that is dragging it.
		std::chrono::steady_clock::time_point user_moved_;
};

// "Next tracks" in the sidebar: what follows the current track. A click jumps
// there, the button removes the entry.
class QueueView : public Gtk::Box {
	public:
		explicit QueueView(Playback& playback);

	private:
		void rebuild();

		Playback&    playback_;
		Gtk::Label   heading_;
		Gtk::ListBox list_;
};

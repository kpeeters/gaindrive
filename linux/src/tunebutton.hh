#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/togglebutton.h>

#include <memory>

struct AppSettings;
class Playback;
class WiimPanel;

// The receiver's volume in single steps, and a WiiM's equaliser: a card the
// window floats above the player bar, as the web client's equaliser panel.
// Not a popover: GTK leaves a popover unable to close on an outside click
// once a drop-down inside it has opened its list (GTK issues 4369, 4529).
class TunePanel : public Gtk::Box {
	public:
		TunePanel(Playback& playback, AppSettings& settings);
		~TunePanel() override;

		// Reads the WiiM afresh; called each time the panel opens.
		void opened();

	private:
		void session_changed();
		void volume_changed();

		Playback&    playback_;
		Gtk::Box     volume_;
		Gtk::Label   heading_, level_, fixed_;
		Gtk::Button  down_, up_;
		std::unique_ptr<WiimPanel> wiim_;
};

// Beside the cast button, as the web client's equaliser button: opens and
// closes the TunePanel. Shown only while casting, until there is a local
// equaliser to put in the panel too.
class TuneButton : public Gtk::ToggleButton {
	public:
		TuneButton(Playback& playback, AppSettings& settings);
		~TuneButton() override;

		// For the window to place; this button only shows and hides it.
		TunePanel& panel();

	private:
		Playback&                  playback_;
		std::unique_ptr<TunePanel> panel_;
};

#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/togglebutton.h>

#include <memory>

struct AppSettings;
class LocalEqPanel;
class Playback;
class WiimPanel;

// A card the window floats above the player bar, as the web client's
// equaliser panel: the local equaliser, or while casting the receiver's
// volume in single steps and a WiiM's equaliser.
// Not a popover: GTK leaves a popover unable to close on an outside click
// once a drop-down inside it has opened its list (GTK issues 4369, 4529).
class TunePanel : public Gtk::Box {
	public:
		TunePanel(Playback& playback, AppSettings& settings);
		~TunePanel() override;

		// Reads the WiiM afresh; called each time the panel opens.
		void opened();

		// Below the narrow-window width.
		void set_narrow(bool narrow);

		// The local equaliser was switched on or off.
		sigc::signal<void(bool on)> signal_eq_switched;

	private:
		void session_changed();
		void volume_changed();

		Playback&    playback_;
		// What casting shows, in place of local_.
		Gtk::Box     cast_;
		Gtk::Box     volume_;
		Gtk::Label   heading_, level_, fixed_;
		Gtk::Button  down_, up_;
		std::unique_ptr<WiimPanel>    wiim_;
		std::unique_ptr<LocalEqPanel> local_;
};

// Beside the cast button, as the web client's equaliser button: opens and
// closes the TunePanel, and is tinted while the local equaliser is on.
class TuneButton : public Gtk::ToggleButton {
	public:
		TuneButton(Playback& playback, AppSettings& settings);
		~TuneButton() override;

		// For the window to place; this button only shows and hides it.
		TunePanel& panel();

	private:
		void update();

		Playback&                  playback_;
		AppSettings&               settings_;
		std::unique_ptr<TunePanel> panel_;
};

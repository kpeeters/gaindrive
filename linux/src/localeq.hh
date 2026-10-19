#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/switch.h>

#include <string>
#include <vector>

struct AppSettings;
class Playback;

// The equaliser for playback here, as the web client's: ten bands of whole
// dB, automatic headroom, built-in presets and curves saved under a name.
// It applies as the faders move; there is no device to wait for.
class LocalEqPanel : public Gtk::Box {
	public:
		LocalEqPanel(Playback& playback, AppSettings& settings);
		~LocalEqPanel() override;

		// Switched on or off, for the button that opens the panel.
		sigc::signal<void(bool on)> signal_switched;

		// Below the narrow-window width.
		void set_narrow(bool narrow);

	private:
		void render();
		void show_preset();
		void choose_preset();
		void faders_moved();
		void apply();
		void save_slot();
		void delete_slot();
		std::string matching_slot() const;

		Playback&                  playback_;
		AppSettings&               settings_;
		// What the menu lists, in order: "Custom", built-ins, saved.
		std::vector<std::string>   entries_;
		// Set while the widgets are filled in from the settings, so their
		// change signals are not taken for the user's.
		bool                       syncing_=false;

		Gtk::Box                   top_, bands_, buttons_, save_row_;
		// The headroom taken off, or a message; beside the title, so the
		// panel's height does not change with it.
		Gtk::Label                 title_, status_;
		Gtk::Switch                on_;
		Gtk::DropDown              presets_;
		std::vector<Gtk::Scale*>   faders_;
		std::vector<Gtk::Label*>   readouts_;
		Gtk::Button                save_, delete_, save_go_;
		Gtk::Entry                 save_name_;
		// The settings file is written once a drag has rested.
		sigc::connection           save_timer_;
};

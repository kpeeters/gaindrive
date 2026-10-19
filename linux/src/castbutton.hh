#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/popover.h>
#include <gtkmm/spinner.h>

class Playback;

// The cast button in the player bar, as on the web client. Its popover lists
// the receivers on the network, and while casting offers a way back. The
// receiver's volume and equaliser are behind TuneButton.
class CastButton : public Gtk::MenuButton {
	public:
		explicit CastButton(Playback& playback);

	private:
		void opened();
		void rebuild();
		void session_changed();

		Playback&    playback_;
		Gtk::Popover popover_;
		Gtk::Box     box_, searching_;
		Gtk::Label   heading_, searching_text_;
		Gtk::Spinner spinner_;
		Gtk::ListBox list_;
		Gtk::Button  stop_;
};

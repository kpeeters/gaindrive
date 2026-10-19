#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/scale.h>
#include <gtkmm/switch.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

struct AppSettings;
class WorkPool;

// A WiiM's own equaliser, spoken to directly over its LinkPlay API as the
// Android client does (the web client needs the server to relay it). The
// switch, the device's presets, its ten bands, and curves saved here per
// receiver. Every command is followed by a fresh read of the device, which
// then decides what is shown.
class WiimPanel : public Gtk::Box {
	public:
		explicit WiimPanel(AppSettings& settings);
		~WiimPanel() override;

		// Reads the device afresh; called each time the cast popover opens.
		void open(const std::string& device_id, const std::string& address);
		// The session ended: whatever is still on its way is dropped.
		void close();

	private:
		struct State {
			bool             on=false;
			std::string      preset;
			std::vector<int> bands;   // empty when the device gave none
		};

		// Runs `cmd` against the device, then reads its state. `preset` is
		// what the menu should name afterwards; empty keeps the current one.
		void run(std::function<bool(const std::string& address)> cmd, std::string preset);
		void render();
		void show_preset();
		void choose_preset();
		void commit_bands();
		void save_slot();
		void delete_slot();
		std::string matching_slot() const;

		AppSettings&               settings_;
		std::unique_ptr<WorkPool>  pool_;
		std::string                device_id_, address_;
		unsigned                   gen_=0;
		State                      state_;
		bool                       have_state_=false;
		std::vector<std::string>   device_presets_;
		// What the menu lists, in order: "Custom", device presets, saved.
		std::vector<std::string>   entries_;
		// Set while the widgets are filled in from state_, so their change
		// signals are not taken for the user's.
		bool                       syncing_=false;

		Gtk::Box                   top_, bands_, save_row_, buttons_;
		// Beside the title rather than on a line of its own, so that a
		// message coming and going does not change the popover's height.
		Gtk::Label                 title_, status_;
		Gtk::Switch                on_;
		Gtk::DropDown              presets_;
		std::vector<Gtk::Scale*>   faders_;
		std::vector<Gtk::Label*>   readouts_;
		Gtk::Button                save_, delete_, save_go_;
		Gtk::Entry                 save_name_;
		sigc::connection           commit_timer_;
};

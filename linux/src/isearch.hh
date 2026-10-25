#pragma once

#include <gdkmm/enums.h>
#include <gtkmm/box.h>
#include <gtkmm/label.h>

class Pane;

// The web client's Ctrl+S / Ctrl+R, emacs style: typing walks the cursor
// to the next row of one pane whose text holds what was typed, without
// leaving the listing as the Search section would. This is its minibuffer,
// and the mode itself.
class ISearch : public Gtk::Box {
	public:
		ISearch();

		bool on() const;
		void start(Pane& pane, bool back);
		// `restore` puts the cursor back where the search began.
		void end(bool restore);
		// The mode's whole keyboard; true when it claimed the key. One it
		// does not claim ends the search and then does its own job.
		bool key(guint keyval, Gdk::ModifierType state);

	private:
		void step(int from);
		void draw();
		// From the cursor, or from the end the search runs from without one.
		int  from_cursor(int offset) const;

		Pane          *pane_=nullptr;
		Glib::ustring  text_;
		int            origin_=-1, cursor_=-1;
		// Sticky, as in emacs: typing carries on the way it was last sent.
		bool           back_=false, wrapped_=false, failing_=false;
		Gtk::Label     label_, text_label_, note_, hint_;
};

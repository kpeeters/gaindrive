#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/listbox.h>
#include <gtkmm/menubutton.h>
#include <gtkmm/overlay.h>
#include <gtkmm/picture.h>
#include <gtkmm/popover.h>
#include <gtkmm/revealer.h>
#include <sigc++/connection.h>

#include <memory>
#include <string>
#include <vector>

class ApiClient;
class Playback;
struct Song;

// A film as it plays: the picture, subtitles, chapters, and controls that get
// out of the way. Playback owns what plays, so the player bar keeps working
// alongside.
class VideoView : public Gtk::Overlay {
	public:
		struct Cue {
			double      start=0, end=0;
			std::string text;
		};

		explicit VideoView(Playback& playback);
		~VideoView() override;

		// Fetches this film's captions and chapters; again only for another
		// film.
		void show_film(const Song& film, std::shared_ptr<ApiClient> api);
		void set_fullscreen(bool on);

		sigc::signal<void()> signal_close;
		// Asks the window to toggle fullscreen.
		sigc::signal<void()> signal_fullscreen;
		// The chapter now playing changed: its 0-based index in this
		// film's list, or -1 for none.
		sigc::signal<void(int)> signal_chapter;

	private:
		struct Chapter {
			double      start=0;
			std::string name;
		};

		void select_caption(int row);
		void fill_chapters();
		// The chapter `seconds` falls in, or -1 before the first.
		int  chapter_at(double seconds) const;
		void chapter_prev();
		void chapter_next();
		void update_chapter();
		bool tick();
		// Shows the controls and starts the clock that hides them again.
		void poke();
		bool hide_controls();
		bool key(guint keyval, guint keycode, Gdk::ModifierType state);

		Playback&                  playback_;
		std::shared_ptr<ApiClient> api_;
		std::string                film_id_;
		// Bumped per film, so an answer for the previous one is dropped.
		unsigned                   gen_=0;
		bool                       fullscreen_=false;

		Gtk::Picture   picture_;
		Gtk::Label     missing_;
		Gtk::Label     caption_;
		Gtk::Revealer  top_, bottom_;
		Gtk::Box       bar_;
		Gtk::Button    close_, back_, play_, forward_, full_;
		Gtk::Button    chapter_prev_, chapter_next_;
		Gtk::Label     chapter_now_;
		Gtk::MenuButton chapters_button_, captions_button_;
		Gtk::Popover   chapters_popover_, captions_popover_;
		Gtk::ListBox   chapters_list_, captions_list_;

		std::vector<Chapter>     chapters_;
		int                      chapter_=-1;
		// getVideoInfo's caption ids, in menu order after "Off".
		std::vector<std::string> caption_ids_;
		std::vector<Cue>         cues_;

		sigc::connection timer_, hide_;
};

// The cues of a WebVTT file, tags stripped; what getCaptions sends.
std::vector<VideoView::Cue> parse_vtt(const std::string& text);

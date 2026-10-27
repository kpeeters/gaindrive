#pragma once

#include "panes.hh"

#include <gtkmm/dropdown.h>
#include <gtkmm/entry.h>
#include <gtkmm/grid.h>
#include <gtkmm/listbox.h>
#include <gtkmm/progressbar.h>
#include <gtkmm/togglebutton.h>
#include <gtkmm/window.h>

#include <atomic>
#include <map>
#include <memory>
#include <string>
#include <vector>

struct AppSettings;

// An entry offering, in a popover under it, those of its names that hold
// what was typed. GTK's EntryCompletion did this, and is deprecated.
class NameEntry : public Gtk::Box {
	public:
		NameEntry();
		~NameEntry() override;

		Gtk::Entry& entry();
		void set_names(std::vector<std::string> names);

	private:
		void suggest();

		Gtk::Entry               entry_;
		Gtk::Popover             popover_;
		Gtk::ListBox             list_;
		std::vector<std::string> names_;
		// While a suggestion is being put into the entry.
		bool                     picking_=false;
};

// The uploads listing, as on the web client: the uploading account's own
// batches, or everybody's for an admin, under the form that adds to them.
// Always on one server, the first gaindrive server that takes this
// account's uploads.
class UploadsPane : public Pane {
	public:
		UploadsPane(CoverStore& covers, AppSettings& settings);
		~UploadsPane() override;

		// Null for none.
		void set_server(std::shared_ptr<ApiClient> api, bool admin);
		// The listing again, and the fetch jobs watched while it shows.
		void load();
		// The section was left: nothing is polled for a listing not shown.
		void leave();

	protected:
		void bind(RowWidget& w, const ListRow& row) override;
		void activated(const ListRow& row) override;

	private:
		// The getArtists `personal` this account's listing is asked with.
		std::string scope() const;
		void load_server_info();
		void choose_archive();
		void start_upload(const std::string& path);
		void watch_upload(int files);
		void fetch();
		void watch_fetches();
		void show_jobs(const ApiResult& r);
		void recheck();
		void check_existing();
		void sync_busy();

		AppSettings&                       settings_;
		std::shared_ptr<ApiClient>         api_;
		bool                               admin_=false;
		bool                               shown_=false;
		// Bumped when the server changes, so replies about the last one are
		// dropped.
		unsigned                           server_gen_=0;

		Gtk::Box                           form_{Gtk::Orientation::VERTICAL, 6};
		Gtk::Label                         intro_, names_title_, names_hint_, add_title_;
		Gtk::Grid                          names_card_, add_card_;
		NameEntry                          artist_;
		Gtk::Entry                         album_;
		Gtk::Label                         dupe_;
		Gtk::Box                           archive_row_{Gtk::Orientation::HORIZONTAL, 6};
		Gtk::Label                         archive_types_;
		Gtk::Button                        choose_, cancel_;
		Gtk::Label                         url_label_;
		Gtk::Box                           url_row_{Gtk::Orientation::HORIZONTAL, 6};
		Gtk::Entry                         url_;
		Gtk::Box                           mode_;
		Gtk::ToggleButton                  audio_, video_;
		Gtk::Button                        fetch_;
		Gtk::ProgressBar                   progress_;
		Gtk::Label                         status_;
		Gtk::Box                           jobs_{Gtk::Orientation::VERTICAL, 8};

		// The upload running, if any: setting it stops it.
		std::shared_ptr<std::atomic<bool>> cancel_flag_;
		bool                               uploading_=false;
		// Of the listing as last shown: name and album count per artist,
		// since a second upload usually adds an album to an artist already
		// listed.
		std::string                        signature_;
		sigc::connection                   upload_poll_, fetch_poll_, dupe_timer_;
		// The states last seen, by job id, so a job is seen reaching "done"
		// once.
		std::map<std::string, std::string> job_states_;

		// Every top-level name the server knows, by its casefolded form,
		// with the slice it is in ("staging" for the uploads).
		struct Known {
			std::string name, id, label;
		};
		std::map<std::string, Known>       known_;
		unsigned                           dupe_gen_=0;
};

// Where an upload goes in the shared library: a root, and an artist or a
// category in it. Two controls rather than a browser, as on the web: an
// album sits one level down, always.
class PromoteDialog : public Gtk::Window {
	public:
		PromoteDialog(Gtk::Window& parent, std::shared_ptr<ApiClient> api,
		              const Album& album, const std::string& artist);
		~PromoteDialog() override;

		// moveAlbum's answer; the dialog has closed.
		sigc::signal<void(const ApiResult&)> signal_moved;

	private:
		struct Root {
			std::string id, name, type;
		};
		void root_changed();
		void sync_go();
		void move();

		std::shared_ptr<ApiClient> api_;
		Album                      album_;
		std::string                artist_;
		std::vector<Root>          roots_;
		Gtk::Box                   box_{Gtk::Orientation::VERTICAL, 10};
		Gtk::Label                 what_, folder_label_, note_;
		Gtk::DropDown              root_;
		NameEntry                  folder_;
		Gtk::Box                   buttons_{Gtk::Orientation::HORIZONTAL, 6};
		Gtk::Button                cancel_, go_;
		// Replies arrive on the main loop, possibly after the dialog went.
		std::shared_ptr<bool>      alive_;
		unsigned                   names_gen_=0;
};

// Asks, then deletes the upload `album` from its server; `done` gets the
// server's answer, and is not called when the user says no.
void confirm_delete_upload(Gtk::Window& parent, std::shared_ptr<ApiClient> api,
                           const Album& album, std::function<void(const ApiResult&)> done);

#include "uploads.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "settings.hh"
#include "stamp.hh"

#include <giomm/liststore.h>
#include <glibmm/main.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/grid.h>
#include <gtkmm/root.h>
#include <gtkmm/stringlist.h>

#include <algorithm>
#include <iostream>
#include <set>

namespace {
	const char *archive_types[] = { ".zip", ".tar", ".tar.gz", ".tgz", ".7z" };

	std::string fold(const std::string& s)
		{
		return Glib::ustring(s).casefold().raw();
		}

	std::string trimmed(const std::string& s)
		{
		auto a = s.find_first_not_of(" \t");
		if(a==std::string::npos) return "";
		auto b = s.find_last_not_of(" \t");
		return s.substr(a, b-a+1);
		}

	bool flag(const nlohmann::json& j, const std::string& key)
		{
		const auto& v = jsub(j, key);
		return v.is_boolean() && v.get<bool>();
		}

	std::string curly(const std::string& s)
		{
		return "“" + s + "”";
		}

	void hint(Gtk::Label& l, const std::string& text)
		{
		l.set_text(text);
		l.set_wrap(true);
		l.set_xalign(0);
		l.add_css_class("dim-label");
		}

	// A status line takes no room while it has nothing to say.
	void note(Gtk::Label& l, const std::string& text)
		{
		l.set_text(text);
		l.set_visible(!text.empty());
		}

	void title(Gtk::Label& l, const std::string& text)
		{
		l.set_text(text);
		l.set_xalign(0);
		l.add_css_class("settings-group-title");
		}

	Gtk::Label& field_label(const std::string& text)
		{
		auto l = Gtk::make_managed<Gtk::Label>(text);
		l->set_xalign(0);
		return *l;
		}

	void card(Gtk::Grid& g)
		{
		g.set_row_spacing(10);
		g.set_column_spacing(12);
		g.add_css_class("settings-group");
		g.add_css_class("upload-card");
		}

	// A clear icon inside the field while it holds anything.
	void clearable(Gtk::Entry& e)
		{
		auto sync = [&e]() {
			if(e.get_text().empty()) e.unset_icon(Gtk::Entry::IconPosition::SECONDARY);
			else e.set_icon_from_icon_name("gd-close-symbolic", Gtk::Entry::IconPosition::SECONDARY);
			};
		e.set_icon_tooltip_text("Clear", Gtk::Entry::IconPosition::SECONDARY);
		e.signal_changed().connect(sync);
		e.signal_icon_release().connect([&e](Gtk::Entry::IconPosition) { e.set_text(""); });
		sync();
		}
}

// ---- NameEntry ----------------------------------------------------------

NameEntry::NameEntry()
	: Gtk::Box(Gtk::Orientation::VERTICAL)
	{
	entry_.set_hexpand(true);
	append(entry_);
	// Not focusable and not autohiding, so typing carries on in the entry
	// while the suggestions show.
	popover_.set_autohide(false);
	popover_.set_has_arrow(false);
	popover_.set_can_focus(false);
	popover_.set_position(Gtk::PositionType::BOTTOM);
	list_.set_can_focus(false);
	list_.set_selection_mode(Gtk::SelectionMode::NONE);
	popover_.set_child(list_);
	popover_.set_parent(*this);
	list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		auto label = dynamic_cast<Gtk::Label*>(row->get_child());
		if(!label) return;
		picking_ = true;
		entry_.set_text(label->get_text());
		entry_.set_position(-1);
		picking_ = false;
		popover_.popdown();
		entry_.grab_focus_without_selecting();
		});
	entry_.signal_changed().connect(sigc::mem_fun(*this, &NameEntry::suggest));
	entry_.signal_activate().connect([this]() { popover_.popdown(); });
	auto focus = Gtk::EventControllerFocus::create();
	focus->signal_leave().connect([this]() { popover_.popdown(); });
	entry_.add_controller(focus);
	}

NameEntry::~NameEntry()
	{
	popover_.unparent();
	}

Gtk::Entry& NameEntry::entry()
	{
	return entry_;
	}

void NameEntry::set_names(std::vector<std::string> names)
	{
	names_ = std::move(names);
	popover_.popdown();
	}

void NameEntry::suggest()
	{
	if(picking_) return;
	auto typed = fold(trimmed(entry_.get_text()));
	// Only for typing: not for a name filled in from settings or a default.
	// The focus is on the entry's text widget, inside it.
	auto root  = get_root();
	auto focus = root ? root->get_focus() : nullptr;
	bool typing = focus && (focus==&entry_ || focus->is_ancestor(entry_));
	std::vector<std::string> starts, holds;
	if(!typed.empty() && typing)
		for(const auto& n: names_) {
			auto f = fold(n);
			if(f==typed) continue;
			auto at = f.find(typed);
			if(at==0)                      starts.push_back(n);
			else if(at!=std::string::npos) holds.push_back(n);
			}
	starts.insert(starts.end(), holds.begin(), holds.end());
	if(starts.size()>8) starts.resize(8);
	while(auto row = list_.get_row_at_index(0))
		list_.remove(*row);
	if(starts.empty()) {
		popover_.popdown();
		return;
		}
	for(const auto& n: starts) {
		auto label = Gtk::make_managed<Gtk::Label>(n);
		label->set_xalign(0);
		label->set_margin(4);
		list_.append(*label);
		if(auto row = label->get_parent()) row->set_focusable(false);
		}
	popover_.popup();
	}

// ---- UploadsPane --------------------------------------------------------

UploadsPane::UploadsPane(CoverStore& covers, AppSettings& settings)
	: Pane(covers, 0, true),
	  settings_(settings)
	{
	// One server, so nothing to tell apart.
	row_badges_ = false;
	form_.set_margin(12);
	form_.set_margin_bottom(6);

	// Says where an upload goes, since "Uploads" reads like the library
	// itself: until an admin moves it, only this account sees it.
	hint(intro_, "Uploads stay private to you until an admin moves them into the shared library.");

	// Two cards as on the settings page: what it is filed as, then the two
	// ways of sending it.
	title(names_title_, "File as");
	artist_.entry().set_placeholder_text("Artist or category");
	album_.set_placeholder_text("Album or name");
	artist_.set_hexpand(true);
	album_.set_hexpand(true);
	artist_.entry().set_text(settings_.upload_artist);
	album_.set_text(settings_.upload_album);
	clearable(artist_.entry());
	clearable(album_);
	card(names_card_);
	names_card_.attach(field_label("Artist"), 0, 0);
	names_card_.attach(artist_, 1, 0);
	names_card_.attach(field_label("Album"), 0, 1);
	names_card_.attach(album_, 1, 1);
	hint(names_hint_, "Leave either blank to use what the archive or the page says.");
	names_hint_.add_css_class("settings-hint");
	hint(dupe_, "");
	dupe_.remove_css_class("dim-label");
	dupe_.add_css_class("settings-hint");
	dupe_.add_css_class("upload-note");
	dupe_.set_visible(false);

	title(add_title_, "Add");
	hint(archive_types_, "zip, tar, tar.gz, tgz or 7z");
	archive_types_.set_hexpand(true);
	choose_.set_label("Choose file\u2026");
	cancel_.set_label("Cancel");
	cancel_.set_visible(false);
	archive_row_.append(archive_types_);
	archive_row_.append(cancel_);
	archive_row_.append(choose_);
	url_.set_placeholder_text("https://\u2026");
	url_.set_hexpand(true);
	clearable(url_);
	audio_.set_label("Audio");
	video_.set_label("Video");
	video_.set_group(audio_);
	(settings_.fetch_mode=="video" ? video_ : audio_).set_active(true);
	mode_.add_css_class("linked");
	mode_.add_css_class("upload-mode");
	mode_.append(audio_);
	mode_.append(video_);
	fetch_.set_label("Fetch");
	url_row_.append(url_);
	url_row_.append(mode_);
	url_row_.append(fetch_);
	url_label_.set_text("URL");
	url_label_.set_xalign(0);
	card(add_card_);
	add_card_.attach(field_label("Archive"), 0, 0);
	add_card_.attach(archive_row_, 1, 0);
	add_card_.attach(url_label_, 0, 1);
	add_card_.attach(url_row_, 1, 1);
	add_card_.attach(progress_, 0, 2, 2, 1);
	url_label_.set_visible(false);
	url_row_.set_visible(false);
	progress_.set_visible(false);
	hint(status_, "");
	status_.add_css_class("settings-hint");
	status_.set_visible(false);
	jobs_.set_margin_top(6);

	form_.append(intro_);
	form_.append(names_title_);
	form_.append(names_card_);
	form_.append(names_hint_);
	form_.append(dupe_);
	form_.append(add_title_);
	form_.append(add_card_);
	form_.append(status_);
	form_.append(jobs_);
	top_.append(form_);

	artist_.entry().signal_changed().connect(sigc::mem_fun(*this, &UploadsPane::recheck));
	album_.signal_changed().connect(sigc::mem_fun(*this, &UploadsPane::recheck));
	choose_.signal_clicked().connect(sigc::mem_fun(*this, &UploadsPane::choose_archive));
	cancel_.signal_clicked().connect([this]() {
		if(cancel_flag_) *cancel_flag_ = true;
		note(status_, "Cancelling\u2026");
		});
	fetch_.signal_clicked().connect(sigc::mem_fun(*this, &UploadsPane::fetch));
	// Enter fetches from any of the three, artist then album then Enter
	// being the natural way to fill it in; from the names only when there
	// is a URL to fetch.
	url_.signal_activate().connect(sigc::mem_fun(*this, &UploadsPane::fetch));
	for(auto e: {&artist_.entry(), &album_})
		e->signal_activate().connect([this]() {
			if(url_row_.get_visible() && !trimmed(url_.get_text()).empty()) fetch();
			});
	audio_.signal_toggled().connect([this]() {
		settings_.fetch_mode = audio_.get_active() ? "audio" : "video";
		settings_.save();
		});
	}

UploadsPane::~UploadsPane()
	{
	if(cancel_flag_) *cancel_flag_ = true;
	upload_poll_.disconnect();
	fetch_poll_.disconnect();
	dupe_timer_.disconnect();
	}

std::string UploadsPane::scope() const
	{
	// An admin is the only account that can promote, so an admin sees
	// everybody's, grouped by owner; otherwise a user's upload is stranded
	// where nobody who can act on it sees it.
	return admin_ ? "*" : "true";
	}

void UploadsPane::set_server(std::shared_ptr<ApiClient> api, bool admin)
	{
	if(api==api_ && admin==admin_) return;
	++server_gen_;
	if(cancel_flag_) *cancel_flag_ = true;
	upload_poll_.disconnect();
	fetch_poll_.disconnect();
	api_   = std::move(api);
	admin_ = admin;
	uploading_ = false;
	sync_busy();
	progress_.set_visible(false);
	note(status_, "");
	known_.clear();
	artist_.set_names({});
	job_states_.clear();
	while(auto child = jobs_.get_first_child())
		jobs_.remove(*child);
	url_label_.set_visible(false);
	url_row_.set_visible(false);
	clear();
	if(api_) load_server_info();
	}

void UploadsPane::load_server_info()
	{
	unsigned gen = server_gen_;
	// Asked once per server: what it can fetch from a URL. None is the
	// feature being unavailable, and then no URL row is offered at all.
	api_->call("getUrlHandlers", {}, [this, gen](const ApiResult& r) {
		if(gen!=server_gen_) return;
		const auto& hs = jsub(jsub(r.body, "urlHandlers"), "urlHandler");
		bool can_audio = false, can_video = false;
		for(size_t i=0; i<hs.size(); ++i) {
			can_audio |= flag(jidx(hs, i), "audio");
			can_video |= flag(jidx(hs, i), "video");
			}
		bool any = can_audio || can_video;
		url_label_.set_visible(any);
		url_row_.set_visible(any);
		// A choice only when there is one to make.
		mode_.set_visible(can_audio && can_video);
		if(!can_audio) video_.set_active(true);
		if(!can_video) audio_.set_active(true);
		if(any && shown_) watch_fetches();
		});

	// The names to complete against: every slice of the library, not only
	// the one this will be filed into, and the uploads, the likeliest
	// duplicate of all and the one no library listing shows.
	api_->call("getMusicFolders", {}, [this, gen](const ApiResult& r) {
		if(gen!=server_gen_) return;
		std::set<std::string> types;
		const auto& folders = jsub(jsub(r.body, "musicFolders"), "musicFolder");
		for(size_t i=0; i<folders.size(); ++i) {
			auto t = jstr(jidx(folders, i), "contentType");
			if(!t.empty()) types.insert(t);
			}
		ServerCalls calls;
		std::vector<std::string> labels;
		for(const auto& t: types) {
			calls.push_back({api_, {{"contentType", t}}});
			labels.push_back(t);
			}
		// A server naming no kinds still has one library.
		if(types.empty()) {
			calls.push_back({api_, {}});
			labels.push_back("the library");
			}
		calls.push_back({api_, {{"personal", "true"}}});
		labels.push_back("staging");
		call_each(calls, "getArtists", [this, gen, labels](const std::vector<ApiResult>& rs) {
			if(gen!=server_gen_) return;
			known_.clear();
			std::vector<std::string> names;
			for(size_t k=0; k<rs.size(); ++k) {
				// A slice that fails is left out: this is completion, and
				// half a list is worth more than an error.
				const auto& index = jsub(jsub(rs[k].body, "artists"), "index");
				for(size_t i=0; i<index.size(); ++i) {
					const auto& arr = jsub(jidx(index, i), "artist");
					for(size_t a=0; a<arr.size(); ++a) {
						auto name = jstr(jidx(arr, a), "name");
						auto key  = fold(name);
						// The first slice wins, as everywhere a merge happens.
						if(name.empty() || known_.count(key)) continue;
						known_[key] = {name, jstr(jidx(arr, a), "id"), labels[k]};
						names.push_back(name);
						}
					}
				}
			artist_.set_names(std::move(names));
			// The names arrive filled in from last time, and a stale one is
			// what the note is there to show.
			check_existing();
			});
		});
	}

void UploadsPane::load()
	{
	shown_ = true;
	clear();
	set_title("Uploads");
	if(!api_) return;
	unsigned gen = gen_;
	api_->call("getArtists", {{"personal", scope()}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		// The buckets are first letters, or for an admin the owners.
		std::vector<Glib::RefPtr<ListRow>> rows;
		signature_.clear();
		const auto& index = jsub(jsub(r.body, "artists"), "index");
		for(size_t i=0; i<index.size(); ++i) {
			const auto& bucket = jidx(index, i);
			const auto& arr    = jsub(bucket, "artist");
			if(!arr.empty())
				rows.push_back(ListRow::heading(jstr(bucket, "name")));
			for(size_t k=0; k<arr.size(); ++k) {
				auto a = parse_artist(r.server, jidx(arr, k), false);
				signature_ += a.name + ":" + std::to_string(a.album_count) + "|";
				rows.push_back(ListRow::of(a));
				}
			}
		set_empty(rows.empty() ? "Nothing uploaded yet." : "");
		set_rows(rows);
		});
	if(url_row_.get_visible()) watch_fetches();
	}

void UploadsPane::leave()
	{
	shown_ = false;
	upload_poll_.disconnect();
	fetch_poll_.disconnect();
	}

void UploadsPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::heading)
		w.show_heading(row.text);
	else
		w.show_item({}, "", row.artist.name, "", plural(row.artist.album_count, "album", "albums"));
	}

void UploadsPane::activated(const ListRow& row)
	{
	signal_open_artist.emit(row.artist);
	}

void UploadsPane::sync_busy()
	{
	choose_.set_sensitive(!uploading_ && api_);
	cancel_.set_visible(uploading_);
	}

void UploadsPane::choose_archive()
	{
	auto win = dynamic_cast<Gtk::Window*>(get_root());
	if(!win || !api_) return;
	auto filter = Gtk::FileFilter::create();
	filter->set_name("Archives");
	// Without the dot; case does not matter to a suffix.
	for(auto t: archive_types)
		filter->add_suffix(t+1);
	auto filters = Gio::ListStore<Gtk::FileFilter>::create();
	filters->append(filter);
	auto dialog = Gtk::FileDialog::create();
	dialog->set_title("Upload a music archive");
	dialog->set_filters(filters);
	dialog->set_default_filter(filter);
	dialog->open(*win, [this, dialog](Glib::RefPtr<Gio::AsyncResult>& res) {
		Glib::RefPtr<Gio::File> file;
		try {
			file = dialog->open_finish(res);
			}
		catch(const Glib::Error& e) {
			// Dismissed, which is nothing to report.
			std::cout << stamp() << "Upload file dialog: " << e.what() << std::endl;
			return;
			}
		if(!file) return;
		auto path = file->get_path();
		if(path.empty()) {
			note(status_, "Error: only a file on this computer can be uploaded.");
			return;
			}
		start_upload(path);
		});
	}

void UploadsPane::start_upload(const std::string& path)
	{
	if(!api_ || uploading_) return;
	// The filter is only a suggestion. Checked here, because a refusal sent
	// mid-body reaches us through a proxy as a bare 502, and only after the
	// whole file has gone up.
	auto lower = fold(path);
	bool ok = false;
	for(auto t: archive_types)
		ok |= lower.ends_with(t);
	if(!ok) {
		note(status_, "Error: Unsupported file type. Use zip, tar, tar.gz, tgz, or 7z.");
		return;
		}
	// An untouched field sends nothing at all rather than an empty value,
	// which the server is entitled to treat as a mistake.
	Params p;
	auto a = trimmed(artist_.entry().get_text());
	auto b = trimmed(album_.get_text());
	if(!a.empty()) p.push_back({"artist", a});
	if(!b.empty()) p.push_back({"album", b});

	upload_poll_.disconnect();
	auto flag = std::make_shared<std::atomic<bool>>(false);
	cancel_flag_ = flag;
	uploading_   = true;
	sync_busy();
	progress_.set_fraction(0);
	progress_.set_visible(true);
	note(status_, "");
	std::cout << stamp() << "Uploading " << path << std::endl;
	api_->upload(path, p, [this, flag](int pct) {
		if(flag!=cancel_flag_) return;
		progress_.set_fraction(pct/100.0);
		// Past here the server is extracting, which nothing can stop.
		if(pct>=100) {
			cancel_.set_visible(false);
			note(status_, "Extracting on the server…");
			}
		}, flag, [this, flag](const ApiResult& r) {
		if(flag!=cancel_flag_) return;
		uploading_ = false;
		cancel_flag_.reset();
		sync_busy();
		progress_.set_visible(false);
		if(!r.ok) {
			note(status_, "Error: " + r.error);
			return;
			}
		int files = jint(r.body, "files");
		note(status_, "Extracted " + std::to_string(files) + " file(s); scanning…");
		// The album goes with the upload: the next archive is almost never
		// the same album, and a leftover name would file it under this one.
		// The artist stays, as on the web client.
		album_.set_text("");
		settings_.upload_album.clear();
		settings_.save();
		watch_upload(files);
		});
	}

void UploadsPane::watch_upload(int files)
	{
	// The server answers before it has scanned, so there is nothing new
	// to show yet: the listing is asked again until it changes.
	auto tries = std::make_shared<int>(0);
	unsigned gen = server_gen_;
	upload_poll_.disconnect();
	upload_poll_ = Glib::signal_timeout().connect([this, tries, files, gen]() {
		if(gen!=server_gen_ || !api_) return false;
		api_->call("getArtists", {{"personal", scope()}}, [this, gen](const ApiResult& r) {
			// A blip does not end the wait.
			if(gen!=server_gen_ || !r.ok || !upload_poll_.connected()) return;
			std::string sig;
			const auto& index = jsub(jsub(r.body, "artists"), "index");
			for(size_t i=0; i<index.size(); ++i) {
				const auto& arr = jsub(jidx(index, i), "artist");
				for(size_t k=0; k<arr.size(); ++k)
					sig += jstr(jidx(arr, k), "name") + ":" + std::to_string(jint(jidx(arr, k), "albumCount")) + "|";
				}
			if(sig==signature_) return;
			upload_poll_.disconnect();
			note(status_, "");
			load();
			});
		if(++*tries>=10) {
			note(status_, "Extracted " + std::to_string(files) + " file(s); the server is still scanning them.");
			return false;
			}
		return true;
		}, 2000);
	}

void UploadsPane::fetch()
	{
	if(!api_) return;
	auto url = trimmed(url_.get_text());
	if(url.empty()) {
		note(status_, "No URL entered.");
		return;
		}
	Params p = {{"url", url}, {"mode", video_.get_active() ? "video" : "audio"}};
	auto a = trimmed(artist_.entry().get_text());
	auto b = trimmed(album_.get_text());
	if(!a.empty()) p.push_back({"artist", a});
	if(!b.empty()) p.push_back({"album", b});
	fetch_.set_sensitive(false);
	note(status_, "");
	unsigned gen = server_gen_;
	api_->call("fetchUrl", p, [this, gen](const ApiResult& r) {
		if(gen!=server_gen_) return;
		fetch_.set_sensitive(true);
		if(!r.ok) {
			// Refused credentials still log out, as everywhere else.
			if(r.code==40) report(r);
			note(status_, "Error: " + r.error);
			return;
			}
		// Only the URL: the names are meant to carry over to the next
		// fetch, six tracks off one concert being typed once.
		url_.set_text("");
		watch_fetches();
		});
	}

void UploadsPane::watch_fetches()
	{
	if(!api_) return;
	unsigned gen = server_gen_;
	auto tick = [this, gen]() {
		if(gen!=server_gen_ || !shown_) return false;
		api_->call("getFetchJobs", {}, [this, gen](const ApiResult& r) {
			// A blip does not end the wait.
			if(gen!=server_gen_ || !shown_ || !r.ok) return;
			show_jobs(r);
			});
		return true;
		};
	fetch_poll_.disconnect();
	fetch_poll_ = Glib::signal_timeout().connect(tick, 2000);
	// Now as well, so a fetch running since before shows at once.
	tick();
	}

void UploadsPane::show_jobs(const ApiResult& r)
	{
	while(auto child = jobs_.get_first_child())
		jobs_.remove(*child);
	bool live = false, finished = false;
	const auto& jobs = jsub(jsub(r.body, "fetchJobs"), "fetchJob");
	for(size_t i=0; i<jobs.size(); ++i) {
		const auto& j = jidx(jobs, i);
		auto id    = jstr(j, "id");
		auto state = jstr(j, "state");
		if(state=="queued" || state=="running" || state=="scanning")
			live = true;
		// Only a job seen unfinished before: one already done when the
		// listing was drawn is in it.
		auto seen = job_states_.find(id);
		if(state=="done" && seen!=job_states_.end() && seen->second!="done")
			finished = true;
		job_states_[id] = state;

		auto row  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
		row->add_css_class("settings-group");
		row->add_css_class("upload-card");
		auto head = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 6);
		auto artist = jstr(j, "artist"), album = jstr(j, "album");
		// The typed names when there were any, so a queued job says what it
		// will be filed as rather than only which tool does it.
		auto name = Gtk::make_managed<Gtk::Label>(
			!artist.empty() || !album.empty()
				? (artist.empty() ? "…" : artist) + " · " + (album.empty() ? "…" : album)
				: jstr(j, "handler") + " · " + jstr(j, "mode"));
		name->set_xalign(0);
		name->set_hexpand(true);
		name->set_ellipsize(Pango::EllipsizeMode::END);
		head->append(*name);
		std::string shown = state;
		if(state=="done")       shown = "done - " + std::to_string(jint(j, "files")) + " file(s)";
		else if(state=="error") shown = jstr(j, "error").empty() ? "error" : jstr(j, "error");
		auto st = Gtk::make_managed<Gtk::Label>(shown);
		st->add_css_class("dim-label");
		st->set_ellipsize(Pango::EllipsizeMode::END);
		head->append(*st);
		if(state=="queued" || state=="running") {
			auto cancel = Gtk::make_managed<Gtk::Button>("Cancel");
			cancel->add_css_class("flat");
			cancel->signal_clicked().connect([this, cancel, id]() {
				cancel->set_sensitive(false);
				if(api_) api_->call("cancelFetch", {{"id", id}}, [this](const ApiResult& r) {
					if(!r.ok) report(r);
					});
				});
			head->append(*cancel);
			}
		row->append(*head);
		if(state=="running" || state=="scanning") {
			auto bar = Gtk::make_managed<Gtk::ProgressBar>();
			bar->set_fraction(std::clamp(jint(j, "percent"), 0, 100)/100.0);
			row->append(*bar);
			}
		auto detail = jstr(j, "detail");
		if(!detail.empty()) {
			auto d = Gtk::make_managed<Gtk::Label>();
			hint(*d, detail);
			row->append(*d);
			}
		jobs_.append(*row);
		}
	// The server scans before it says "done", so the listing is right by
	// now. Redrawn whatever is open beside it: unlike the web's, this
	// listing does not slide back into view when it is redrawn.
	if(finished) {
		load();
		return;
		}
	if(!live) fetch_poll_.disconnect();
	}

void UploadsPane::recheck()
	{
	// Debounced, as the search box is: every keystroke would otherwise be a
	// request. Kept for next time at the same pace.
	dupe_timer_.disconnect();
	dupe_timer_ = Glib::signal_timeout().connect([this]() {
		settings_.upload_artist = trimmed(artist_.entry().get_text());
		settings_.upload_album  = trimmed(album_.get_text());
		settings_.save();
		check_existing();
		return false;
		}, 400);
	}

void UploadsPane::check_existing()
	{
	// Advisory and never blocking: the batch is a fresh directory, and
	// whether a promote will collide is not known until an admin picks a
	// root. The strongest signal only; it makes the weaker ones moot.
	unsigned gen = ++dupe_gen_;
	auto artist = trimmed(artist_.entry().get_text());
	auto album  = trimmed(album_.get_text());
	note(dupe_, "");
	if(!api_) return;
	auto it = artist.empty() ? known_.end() : known_.find(fold(artist));
	if(it!=known_.end()) {
		Known hit = it->second;
		std::string where = hit.label=="staging" ? "already in your uploads, not yet promoted"
		                                         : "already in " + hit.label;
		note(dupe_, curly(hit.name) + " is " + where + ".");
		if(album.empty()) return;
		// That artist's own albums, which is the precise question.
		api_->call("getArtist", {{"id", hit.id}}, [this, gen, hit, where, album](const ApiResult& r) {
			if(gen!=dupe_gen_) return;
			const auto& arr = jsub(jsub(r.body, "artist"), "album");
			for(size_t i=0; i<arr.size(); ++i) {
				auto a = parse_album(r.server, jidx(arr, i));
				if(fold(a.title)==fold(album)) {
					note(dupe_, "You already have " + curly(a.title) + " under " + hit.name + " - " + where + ".");
					return;
					}
				}
			});
		return;
		}
	// Nothing by name, which is when it is worth searching: the same record
	// under a spelling nobody would type. On the album, the distinctive
	// string.
	if(Glib::ustring(album).size()<3) return;
	api_->call("search3", {{"query", album}, {"artistCount", "0"}, {"albumCount", "3"}, {"songCount", "0"}},
	           [this, gen](const ApiResult& r) {
		if(gen!=dupe_gen_) return;
		const auto& found = jsub(jsub(r.body, "searchResult3"), "album");
		if(found.empty()) return;
		auto first = parse_album(r.server, jidx(found, 0));
		std::string text = "Possibly already there: " + curly(first.title);
		if(!first.artist.empty()) text += " by " + first.artist;
		if(found.size()>1) text += " and " + std::to_string(found.size()-1) + " more";
		note(dupe_, text + ".");
		});
	}

// ---- PromoteDialog ------------------------------------------------------

PromoteDialog::PromoteDialog(Gtk::Window& parent, std::shared_ptr<ApiClient> api,
                             const Album& album, const std::string& artist)
	: api_(std::move(api)),
	  album_(album),
	  artist_(artist),
	  alive_(std::make_shared<bool>(true))
	{
	set_title("Move to shared library");
	set_transient_for(parent);
	set_modal(true);
	set_hide_on_close(true);
	set_default_size(440, -1);
	box_.set_margin(16);
	what_.set_text(curly(album_.title) + " is moved out of your uploads and onto the server's disk "
	               "in the shared library, where everyone with an account can see it. This cannot "
	               "be undone from here.");
	what_.set_wrap(true);
	what_.set_xalign(0);
	what_.set_max_width_chars(50);
	box_.append(what_);
	auto root_label = Gtk::make_managed<Gtk::Label>("Library:");
	root_label->set_xalign(0);
	box_.append(*root_label);
	box_.append(root_);
	folder_label_.set_xalign(0);
	box_.append(folder_label_);
	box_.append(folder_);
	hint(note_, "");
	box_.append(note_);
	cancel_.set_label("Cancel");
	go_.set_label("Move");
	go_.add_css_class("suggested-action");
	go_.set_sensitive(false);
	buttons_.set_halign(Gtk::Align::END);
	buttons_.append(cancel_);
	buttons_.append(go_);
	box_.append(buttons_);
	set_child(box_);

	cancel_.signal_clicked().connect([this]() { close(); });
	go_.signal_clicked().connect(sigc::mem_fun(*this, &PromoteDialog::move));
	folder_.entry().signal_changed().connect(sigc::mem_fun(*this, &PromoteDialog::sync_go));
	folder_.entry().signal_activate().connect([this]() { if(go_.get_sensitive()) move(); });
	root_.property_selected().signal_changed().connect(sigc::mem_fun(*this, &PromoteDialog::root_changed));

	std::weak_ptr<bool> alive = alive_;
	api_->call("getMusicFolders", {}, [this, alive](const ApiResult& r) {
		if(alive.expired()) return;
		if(!r.ok) {
			note_.set_text("Error: " + r.error);
			return;
			}
		const auto& folders = jsub(jsub(r.body, "musicFolders"), "musicFolder");
		std::vector<Glib::ustring> labels;
		for(size_t i=0; i<folders.size(); ++i) {
			const auto& f = jidx(folders, i);
			roots_.push_back({jstr(f, "id"), jstr(f, "name"), jstr(f, "contentType")});
			const auto& rt = roots_.back();
			labels.push_back(rt.type.empty() ? rt.name : rt.name + " (" + rt.type + ")");
			}
		root_.set_model(Gtk::StringList::create(labels));
		root_.set_selected(0);
		root_changed();
		});
	}

PromoteDialog::~PromoteDialog() = default;

void PromoteDialog::root_changed()
	{
	auto i = root_.get_selected();
	if(i>=roots_.size()) return;
	const auto& rt = roots_[i];
	bool categories = rt.type=="categories";
	// Blank under a categories root: there level 1 is a category, and the
	// batch's artist is whatever the source called it, a channel usually,
	// which is never the answer.
	folder_label_.set_text(categories ? "Category:" : "Artist:");
	folder_.entry().set_text(categories ? "" : artist_);
	folder_.entry().set_placeholder_text(categories ? "Which category?" : artist_);
	note_.set_text(categories ? "A category that does not exist yet is created."
	                          : "An artist that does not exist yet is created.");
	// Suggestions from that root, so that typing a name not in them is how a
	// new artist or category is made.
	folder_.set_names({});
	unsigned gen = ++names_gen_;
	std::weak_ptr<bool> alive = alive_;
	api_->call("getArtists", {{"musicFolderId", rt.id}}, [this, alive, gen](const ApiResult& r) {
		if(alive.expired() || gen!=names_gen_) return;
		std::vector<std::string> names;
		const auto& index = jsub(jsub(r.body, "artists"), "index");
		for(size_t i=0; i<index.size(); ++i) {
			const auto& arr = jsub(jidx(index, i), "artist");
			for(size_t k=0; k<arr.size(); ++k)
				names.push_back(jstr(jidx(arr, k), "name"));
			}
		folder_.set_names(std::move(names));
		});
	sync_go();
	}

void PromoteDialog::sync_go()
	{
	// The server needs both halves, so the button says so rather than a
	// refusal after the fact.
	go_.set_sensitive(root_.get_selected()<roots_.size() && !trimmed(folder_.entry().get_text()).empty());
	}

void PromoteDialog::move()
	{
	auto i = root_.get_selected();
	if(i>=roots_.size()) return;
	go_.set_sensitive(false);
	go_.set_label("Moving…");
	std::weak_ptr<bool> alive = alive_;
	// moveAlbum is the server's one mover; naming a root makes it a promote
	// rather than a rename in place.
	api_->call("moveAlbum", {{"id", album_.ref.id}, {"musicFolderId", roots_[i].id},
	                         {"folder", trimmed(folder_.entry().get_text())}},
	           [this, alive](const ApiResult& r) {
		if(alive.expired()) return;
		close();
		signal_moved.emit(r);
		});
	}

// ---- Deleting an upload -------------------------------------------------

void confirm_delete_upload(Gtk::Window& parent, std::shared_ptr<ApiClient> api,
                           const Album& album, std::function<void(const ApiResult&)> done)
	{
	auto dialog = Gtk::AlertDialog::create("Delete from uploads?");
	dialog->set_detail(curly(album.title) + " and its files are removed from the server. "
	                   "This cannot be undone.");
	dialog->set_buttons({"Cancel", "Delete"});
	dialog->set_cancel_button(0);
	dialog->set_default_button(0);
	dialog->choose(parent, [dialog, api, id=album.ref.id, done](Glib::RefPtr<Gio::AsyncResult>& res) {
		int button = 0;
		try {
			button = dialog->choose_finish(res);
			}
		catch(const Glib::Error& e) {
			// Dismissed: the same as Cancel.
			std::cout << stamp() << "Delete upload dialog: " << e.what() << std::endl;
			}
		if(button!=1) return;
		api->call("deleteUpload", {{"id", id}}, done);
		});
	}

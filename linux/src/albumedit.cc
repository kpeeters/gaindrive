#include "albumedit.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "stamp.hh"

#include <giomm/file.h>
#include <giomm/liststore.h>
#include <glibmm/bytes.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/filedialog.h>
#include <gtkmm/filefilter.h>
#include <gtkmm/separator.h>
#include <gtkmm/window.h>

#include <iostream>
#include <set>

namespace {
	std::string trimmed(const std::string& s)
		{
		auto a = s.find_first_not_of(" \t");
		if(a==std::string::npos) return "";
		auto b = s.find_last_not_of(" \t");
		return s.substr(a, b-a+1);
		}

	Gtk::Entry *number_entry(const std::string& text, int chars)
		{
		auto e = Gtk::make_managed<Gtk::Entry>();
		e->set_text(text);
		e->set_input_purpose(Gtk::InputPurpose::DIGITS);
		e->set_width_chars(chars);
		e->set_max_width_chars(chars);
		return e;
		}

	Gtk::Button *all_button(const char *tooltip)
		{
		auto b = Gtk::make_managed<Gtk::Button>("all");
		b->add_css_class("flat");
		b->add_css_class("edit-all");
		b->set_tooltip_text(tooltip);
		return b;
		}
}

AlbumEditor::AlbumEditor()
	{
	name_.set_hexpand(true);
	name_.set_placeholder_text("Album");
	artist_.set_hexpand(true);
	artist_.set_placeholder_text("Artist");
	fields.append(name_);
	fields.append(artist_);
	fields.set_hexpand(true);
	fields.set_valign(Gtk::Align::CENTER);

	save_.set_label("Save");
	save_.add_css_class("suggested-action");
	cancel_.set_label("Cancel");
	buttons.append(save_);
	buttons.append(cancel_);
	buttons.set_valign(Gtk::Align::CENTER);
	save_.signal_clicked().connect(sigc::mem_fun(*this, &AlbumEditor::save));
	cancel_.signal_clicked().connect([this]() {
		stop();
		signal_cancelled.emit();
		});

	pencil.set_icon_name("gd-edit-symbolic");
	pencil.set_tooltip_text("Change cover art");
	pencil.set_halign(Gtk::Align::END);
	pencil.set_valign(Gtk::Align::END);
	pencil.set_margin(6);
	pencil.add_css_class("osd");
	pencil.add_css_class("circular");
	choose_.set_label("Upload from this computer…");
	choose_.signal_clicked().connect(sigc::mem_fun(*this, &AlbumEditor::choose_file));
	file_name_.set_xalign(0);
	file_name_.set_ellipsize(Pango::EllipsizeMode::MIDDLE);
	file_name_.add_css_class("dim-label");
	url_label_.set_text("Or paste an image URL:");
	url_label_.set_xalign(0);
	url_.set_placeholder_text("https://…");
	url_.set_width_chars(32);
	url_.signal_activate().connect(sigc::mem_fun(*this, &AlbumEditor::apply_url));
	apply_.set_label("Apply URL");
	apply_.set_halign(Gtk::Align::END);
	apply_.signal_clicked().connect(sigc::mem_fun(*this, &AlbumEditor::apply_url));
	cover_note_.set_xalign(0);
	cover_note_.set_wrap(true);
	cover_note_.set_max_width_chars(32);
	cover_box_.set_margin(8);
	cover_box_.append(choose_);
	cover_box_.append(file_name_);
	cover_box_.append(*Gtk::make_managed<Gtk::Separator>());
	cover_box_.append(url_label_);
	cover_box_.append(url_);
	cover_box_.append(apply_);
	cover_box_.append(cover_note_);
	cover_pop_.set_child(cover_box_);
	pencil.set_popover(cover_pop_);

	tracks.set_column_spacing(6);
	tracks.set_row_spacing(4);
	tracks.add_css_class("track-edit");

	stop();
	}

AlbumEditor::~AlbumEditor() = default;

bool AlbumEditor::editing() const
	{
	return editing_;
	}

void AlbumEditor::start(std::shared_ptr<ApiClient> api, const Album& album,
                        const std::vector<Song>& songs, const std::vector<std::string>& numbers)
	{
	stop();
	editing_ = true;
	api_     = std::move(api);
	album_   = album;
	songs_   = songs;
	// Both are directory names on the server, never read from tags, so
	// saving either moves files; which is why they are edited only here.
	name0_   = album.title;
	artist0_ = album.artist;
	name_.set_text(name0_);
	artist_.set_text(artist0_);
	cover_file_.clear();
	cover_url_.clear();
	file_name_.set_text("");
	url_.set_text("");
	cover_note_.set_visible(false);

	// Only the songs themselves: a chapter is a place inside one, and has
	// nothing of its own to edit.
	std::set<int> discs;
	for(const auto& s: songs_) discs.insert(s.disc);
	bool multi_disc = discs.size()>1;

	int r = 0;
	// The disc "all" always heads the grid, and the year's too for a single
	// disc; with several, each disc's heading carries its own.
	auto disc_all = all_button("Give every track the first track's disc");
	disc_all->signal_clicked().connect([this]() {
		if(rows_.empty()) return;
		for(auto& row: rows_)
			row.disc->set_text(rows_.front().disc->get_text());
		});
	tracks.attach(*disc_all, 2, r);
	if(!multi_disc) add_year_all(r, 0);
	++r;

	int disc = 0;
	for(size_t i=0; i<songs_.size(); ++i) {
		const auto& s = songs_[i];
		if(multi_disc && s.disc!=disc) {
			disc = s.disc;
			auto h = Gtk::make_managed<Gtk::Label>((s.season>0 ? "Series " : "Disc ") + std::to_string(disc));
			h->set_xalign(0);
			h->add_css_class("heading");
			tracks.attach(*h, 0, r, 3, 1);
			add_year_all(r, disc);
			++r;
			}
		Row row;
		row.song   = i;
		row.num0   = i<numbers.size() ? numbers[i] : "";
		row.title0 = s.title;
		row.disc0  = std::to_string(s.disc);
		row.year0  = s.year>0 ? std::to_string(s.year) : "";
		row.num    = number_entry(row.num0, 3);
		row.title  = Gtk::make_managed<Gtk::Entry>();
		row.title->set_text(row.title0);
		row.title->set_hexpand(true);
		row.disc   = number_entry(row.disc0, 2);
		row.year   = number_entry(row.year0, 4);
		row.num->set_tooltip_text("Track number");
		row.disc->set_tooltip_text("Disc");
		row.year->set_tooltip_text("Year");
		tracks.attach(*row.num,   0, r);
		tracks.attach(*row.title, 1, r);
		tracks.attach(*row.disc,  2, r);
		tracks.attach(*row.year,  3, r);
		rows_.push_back(row);
		++r;
		}

	fields.set_visible(true);
	buttons.set_visible(true);
	pencil.set_visible(true);
	tracks.set_visible(true);
	save_.set_sensitive(true);
	cancel_.set_sensitive(true);
	name_.grab_focus();
	}

void AlbumEditor::add_year_all(int grid_row, int disc)
	{
	// The first year of the disc to the rest of it; disc 0 is all of them.
	auto b = all_button("Give the other tracks the first track's year");
	b->signal_clicked().connect([this, disc]() {
		Gtk::Entry *first = nullptr;
		for(auto& row: rows_) {
			if(disc!=0 && songs_[row.song].disc!=disc) continue;
			if(!first) first = row.year;
			else       row.year->set_text(first->get_text());
			}
		});
	tracks.attach(*b, 3, grid_row);
	}

void AlbumEditor::stop()
	{
	++gen_;
	editing_ = false;
	rows_.clear();
	while(auto c = tracks.get_first_child())
		tracks.remove(*c);
	cover_pop_.popdown();
	fields.set_visible(false);
	buttons.set_visible(false);
	pencil.set_visible(false);
	tracks.set_visible(false);
	}

void AlbumEditor::choose_file()
	{
	auto win = dynamic_cast<Gtk::Window*>(tracks.get_root());
	if(!win) return;
	auto filter = Gtk::FileFilter::create();
	filter->set_name("Images");
	filter->add_mime_type("image/*");
	auto filters = Gio::ListStore<Gtk::FileFilter>::create();
	filters->append(filter);
	auto dialog = Gtk::FileDialog::create();
	dialog->set_title("Choose cover art");
	dialog->set_filters(filters);
	dialog->set_default_filter(filter);
	cover_pop_.popdown();
	unsigned gen = gen_;
	dialog->open(*win, [this, dialog, gen](Glib::RefPtr<Gio::AsyncResult>& res) {
		Glib::RefPtr<Gio::File> file;
		try {
			file = dialog->open_finish(res);
			}
		catch(const Glib::Error& e) {
			// Dismissed, which is nothing to report.
			std::cout << stamp() << "Cover file dialog: " << e.what() << std::endl;
			return;
			}
		if(gen!=gen_ || !file) return;
		auto path = file->get_path();
		Glib::RefPtr<Gdk::Texture> tex;
		try {
			tex = Gdk::Texture::create_from_file(file);
			}
		catch(const Glib::Error& e) {
			std::cout << stamp() << "Cover file " << path << ": " << e.what() << std::endl;
			}
		if(path.empty() || !tex) {
			show_errors("Not an image", "That file could not be read as an image.");
			return;
			}
		cover_file_ = path;
		cover_url_.clear();
		file_name_.set_text(file->get_basename());
		signal_preview.emit(tex);
		});
	}

void AlbumEditor::apply_url()
	{
	auto url = trimmed(url_.get_text());
	cover_note_.set_visible(true);
	if(url.rfind("https://", 0)!=0 && url.rfind("http://", 0)!=0) {
		cover_note_.set_text("The URL has to start with https://.");
		return;
		}
	cover_note_.set_text("Fetching…");
	apply_.set_sensitive(false);
	unsigned gen = gen_;
	// Fetched here only to preview it; on Save the server fetches it itself.
	api_->fetch(url, [this, gen, url](bool ok, const std::string& body) {
		if(gen!=gen_) return;
		apply_.set_sensitive(true);
		Glib::RefPtr<Gdk::Texture> tex;
		if(ok)
			try {
				tex = Gdk::Texture::create_from_bytes(Glib::Bytes::create(body.data(), body.size()));
				}
			catch(const Glib::Error& e) {
				std::cout << stamp() << "Cover URL preview: " << e.what() << std::endl;
				}
		if(!tex) {
			cover_note_.set_text(ok ? "That URL is not an image." : "That URL could not be fetched.");
			return;
			}
		cover_note_.set_visible(false);
		cover_url_ = url;
		cover_file_.clear();
		file_name_.set_text("");
		cover_pop_.popdown();
		signal_preview.emit(tex);
		});
	}

void AlbumEditor::save()
	{
	if(!editing_ || !api_) return;
	save_.set_sensitive(false);
	cancel_.set_sensitive(false);
	errors_.clear();
	order_changed_ = year_changed_ = cover_changed_ = false;
	// The cover first, then the tracks, and the rename last: it moves the
	// directory, and a move into one that did not exist yet makes the album
	// a new id, which calls after it would no longer find.
	if(cover_file_.empty() && cover_url_.empty()) {
		save_track(0);
		return;
		}
	Params p = {{"id", album_.ref.id}};
	if(!cover_url_.empty()) p.push_back({"url", cover_url_});
	unsigned gen = gen_;
	api_->post_form("setCoverArt", p, cover_file_, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(r.ok) cover_changed_ = true;
		else     errors_.push_back("Cover art: " + r.error);
		save_track(0);
		});
	}

void AlbumEditor::save_track(size_t from)
	{
	for(size_t i=from; i<rows_.size(); ++i) {
		auto& row = rows_[i];
		Params p = {{"id", songs_[row.song].ref.id}};
		std::string num   = row.num->get_text();
		std::string title = row.title->get_text();
		std::string disc  = row.disc->get_text();
		std::string year  = row.year->get_text();
		if(num!=row.num0) {
			p.push_back({"track", num});
			order_changed_ = true;
			}
		if(title!=row.title0) p.push_back({"title", title});
		if(disc!=row.disc0) {
			p.push_back({"disc", disc});
			order_changed_ = true;
			}
		if(year!=row.year0) {
			p.push_back({"year", year});
			year_changed_ = true;
			}
		if(p.size()==1) continue;
		unsigned gen = gen_;
		api_->call("updateSong", p, [this, gen, i, title](const ApiResult& r) {
			if(gen!=gen_) return;
			if(!r.ok) errors_.push_back("Track “" + title + "”: " + r.error);
			save_track(i+1);
			});
		return;
		}
	rename();
	}

void AlbumEditor::rename()
	{
	auto name   = trimmed(name_.get_text());
	auto artist = trimmed(artist_.get_text());
	if(!errors_.empty() || (name==name0_ && artist==artist0_)) {
		finish(nlohmann::json());
		return;
		}
	if(name.empty() || artist.empty()) {
		errors_.push_back("Album and artist names cannot be empty.");
		finish(nlohmann::json());
		return;
		}
	unsigned gen = gen_;
	// No musicFolderId, so everything above the artist stays and this is a
	// rename in place.
	api_->call("moveAlbum", {{"id", album_.ref.id}, {"album", name}, {"folder", artist}},
	           [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			errors_.push_back("Rename: " + r.error);
			finish(nlohmann::json());
			return;
			}
		finish(jsub(r.body, "movedAlbum"));
		});
	}

void AlbumEditor::finish(const nlohmann::json& moved)
	{
	if(!errors_.empty()) {
		std::string text;
		for(const auto& e: errors_)
			text += (text.empty() ? "" : "\n") + e;
		show_errors("Some changes could not be saved", text);
		save_.set_sensitive(true);
		cancel_.set_sensitive(true);
		return;
		}
	Saved s;
	s.cover  = cover_changed_;
	s.album  = album_.ref;
	s.artist = {album_.ref.server, album_.artist_id};
	if(!jstr(moved, "id").empty()) {
		int failures = jint(moved, "tagFailures");
		if(failures>0)
			show_errors("Renamed", "The tags in " + std::to_string(failures)
			            + " file(s) could not be rewritten.");
		s.album  = {album_.ref.server, jstr(moved, "id")};
		s.artist = {album_.ref.server, jstr(moved, "parent")};
		}
	// The album's own year is the earliest of its tracks', worked out by the
	// server; and track and disc numbers settle the order and the headings.
	// A new cover needs the album's cover id, which one without a cover yet
	// has only once it is asked again.
	s.reload = s.album.id!=album_.ref.id || year_changed_ || order_changed_ || cover_changed_;
	if(!s.reload) {
		s.songs = songs_;
		for(const auto& row: rows_)
			s.songs[row.song].title = row.title->get_text();
		}
	stop();
	signal_saved.emit(s);
	}

void AlbumEditor::show_errors(const std::string& heading, const std::string& text)
	{
	std::cout << stamp() << "Album edit: " << heading << ": " << text << std::endl;
	auto win = dynamic_cast<Gtk::Window*>(tracks.get_root());
	if(!win) return;
	auto dialog = Gtk::AlertDialog::create(heading);
	dialog->set_detail(text);
	dialog->show(*win);
	}

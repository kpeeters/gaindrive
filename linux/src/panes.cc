#include "panes.hh"
#include "apiclient.hh"
#include "coverstore.hh"
#include "jsonread.hh"

#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

#include <algorithm>
#include <set>

namespace {
	int compare_names(const std::string& a, const std::string& b)
		{
		auto fa = Glib::ustring(a).casefold();
		auto fb = Glib::ustring(b).casefold();
		return fa<fb ? -1 : (fb<fa ? 1 : 0);
		}

	std::string plural(int n, const char *one, const char *many)
		{
		return std::to_string(n) + " " + (n==1 ? one : many);
		}
}

// ---- RowWidget ----------------------------------------------------------

RowWidget::RowWidget(CoverStore *covers, int cover_px)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  item_(Gtk::Orientation::HORIZONTAL, 10),
	  text_(Gtk::Orientation::VERTICAL)
	{
	heading_.set_xalign(0);
	heading_.add_css_class("row-heading");
	append(heading_);

	if(covers && cover_px>0) {
		cover_ = Gtk::make_managed<CoverImage>(*covers, cover_px);
		item_.append(*cover_);
		}
	lead_.set_width_chars(3);
	lead_.set_xalign(1);
	lead_.add_css_class("dim-label");
	lead_.add_css_class("numeric");
	item_.append(lead_);

	title_.set_xalign(0);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	subtitle_.set_xalign(0);
	subtitle_.set_ellipsize(Pango::EllipsizeMode::END);
	subtitle_.add_css_class("dim-label");
	text_.set_hexpand(true);
	text_.set_valign(Gtk::Align::CENTER);
	text_.append(title_);
	text_.append(subtitle_);
	item_.append(text_);

	trail_.add_css_class("dim-label");
	trail_.add_css_class("numeric");
	item_.append(trail_);
	append(item_);
	}

void RowWidget::show_heading(const std::string& text)
	{
	heading_.set_text(text);
	heading_.set_visible(true);
	item_.set_visible(false);
	}

void RowWidget::show_item(const std::string& cover_id, const std::string& lead,
                          const std::string& title, const std::string& subtitle,
                          const std::string& trail)
	{
	heading_.set_visible(false);
	item_.set_visible(true);
	if(cover_) cover_->show_cover(cover_id);
	lead_.set_text(lead);
	lead_.set_visible(!lead.empty());
	title_.set_text(title);
	subtitle_.set_text(subtitle);
	subtitle_.set_visible(!subtitle.empty());
	trail_.set_text(trail);
	}

// ---- Pane ---------------------------------------------------------------

Pane::Pane(CoverStore& covers, int cover_px, bool hero)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  covers_(covers),
	  header_(Gtk::Orientation::HORIZONTAL, 6),
	  scroll_box_(Gtk::Orientation::VERTICAL)
	{
	add_css_class("pane");
	title_.set_xalign(0);
	title_.set_hexpand(true);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	title_.add_css_class("pane-title");
	header_.add_css_class("pane-header");
	header_.append(title_);
	append(header_);

	store_     = Gio::ListStore<ListRow>::create();
	selection_ = Gtk::SingleSelection::create(store_);
	selection_->set_autoselect(false);
	selection_->set_can_unselect(true);
	selection_->unselect_all();

	auto factory = Gtk::SignalListItemFactory::create();
	factory->signal_setup().connect([this, cover_px](const Glib::RefPtr<Gtk::ListItem>& li) {
		li->set_child(*Gtk::make_managed<RowWidget>(&covers_, cover_px));
		});
	factory->signal_bind().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
		auto row = std::dynamic_pointer_cast<ListRow>(li->get_item());
		auto w   = dynamic_cast<RowWidget*>(li->get_child());
		if(!row || !w) return;
		bool item = row->kind!=ListRow::Kind::heading;
		li->set_activatable(item);
		li->set_selectable(item);
		bind(*w, *row);
		});
	list_.set_model(selection_);
	list_.set_factory(factory);

	// Selecting is what opens the next pane, as a click on a web row does;
	// Enter selects too.
	selection_->signal_selection_changed().connect([this](guint, guint) {
		auto row = std::dynamic_pointer_cast<ListRow>(selection_->get_selected_item());
		if(row) activated(*row);
		});
	list_.signal_activate().connect([this](guint pos) {
		selection_->set_selected(pos);
		});

	scroll_.set_vexpand(true);
	scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
	if(hero) {
		hero_ = std::make_unique<CoverImage>(covers_, 320);
		hero_->set_halign(Gtk::Align::CENTER);
		hero_->add_css_class("hero");
		scroll_box_.append(*hero_);
		scroll_box_.append(list_);
		scroll_.set_child(scroll_box_);
		}
	else
		scroll_.set_child(list_);
	append(scroll_);
	}

void Pane::clear()
	{
	++gen_;
	set_title("");
	set_rows({});
	if(hero_) {
		hero_->show_cover("");
		hero_->set_visible(false);
		}
	}

void Pane::set_title(const std::string& t)
	{
	title_.set_text(t);
	}

void Pane::set_rows(const std::vector<Glib::RefPtr<ListRow>>& rows)
	{
	selection_->unselect_all();
	store_->splice(0, store_->get_n_items(), rows);
	scroll_.get_vadjustment()->set_value(0);
	}

// ---- ArtistPane ---------------------------------------------------------

ArtistPane::ArtistPane(CoverStore& covers)
	: Pane(covers, 0, false)
	{
	}

void ArtistPane::load(std::shared_ptr<ApiClient> api)
	{
	clear();
	unsigned gen = gen_;
	set_title("Library");
	// Roots of each kind are listed apart, categories first, as on the web
	// client. contentType is a gaindrive extension: another Subsonic server
	// sends none, and gets one plain listing.
	api->call("getMusicFolders", {}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			signal_failed.emit(r);
			return;
			}
		std::set<std::string> types;
		const auto& folders = jsub(jsub(r.body, "musicFolders"), "musicFolder");
		for(size_t i=0; i<folders.size(); ++i)
			types.insert(jstr(jidx(folders, i), "contentType"));
		typed_ = types.count("categories") || types.count("artists");
		load_artists(api, gen, types.count("categories")>0,
		             !typed_ || types.count("artists")>0, {});
		});
	}

void ArtistPane::load_artists(std::shared_ptr<ApiClient> api, unsigned gen, bool categories,
                              bool artists, std::vector<Glib::RefPtr<ListRow>> rows)
	{
	if(!categories && !artists) {
		set_rows(rows);
		return;
		}
	Params p;
	if(categories)  p.push_back({"contentType", "categories"});
	else if(typed_) p.push_back({"contentType", "artists"});

	api->call("getArtists", p, [this, api, gen, categories, artists, rows](const ApiResult& r) mutable {
		if(gen!=gen_) return;
		if(!r.ok) {
			signal_failed.emit(r);
			return;
			}
		const auto& server = api->session().server_key;
		const auto& index  = jsub(jsub(r.body, "artists"), "index");
		if(categories) {
			// A handful of sections, so one heading over an alphabetical list
			// rather than the server's letter buckets.
			std::vector<Artist> list;
			for(size_t i=0; i<index.size(); ++i) {
				const auto& arr = jsub(jidx(index, i), "artist");
				for(size_t k=0; k<arr.size(); ++k)
					list.push_back(parse_artist(server, jidx(arr, k), true));
				}
			std::sort(list.begin(), list.end(), [](const Artist& a, const Artist& b) {
				return compare_names(a.name, b.name)<0;
				});
			if(!list.empty())
				rows.push_back(ListRow::heading("Categories"));
			for(const auto& a: list)
				rows.push_back(ListRow::of(a));
			load_artists(api, gen, false, artists, std::move(rows));
			return;
			}
		for(size_t i=0; i<index.size(); ++i) {
			const auto& bucket = jidx(index, i);
			const auto& arr    = jsub(bucket, "artist");
			if(arr.empty()) continue;
			rows.push_back(ListRow::heading(jstr(bucket, "name")));
			for(size_t k=0; k<arr.size(); ++k)
				rows.push_back(ListRow::of(parse_artist(server, jidx(arr, k), false)));
			}
		set_rows(rows);
		});
	}

void ArtistPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::heading)
		w.show_heading(row.text);
	else
		w.show_item("", "", row.artist.name, "", plural(row.artist.album_count, "album", "albums"));
	}

void ArtistPane::activated(const ListRow& row)
	{
	signal_artist.emit(row.artist);
	}

// ---- AlbumPane ----------------------------------------------------------

AlbumPane::AlbumPane(CoverStore& covers)
	: Pane(covers, 80, false)
	{
	sort_.set_visible(false);
	sort_.add_css_class("flat");
	sort_.signal_clicked().connect([this]() {
		by_name_[category_] = !by_name_[category_];
		label_sort();
		render();
		});
	header_.append(sort_);
	}

void AlbumPane::label_sort()
	{
	// The label says the state, the tooltip what pressing it does.
	bool by_name = by_name_[category_];
	sort_.set_label(by_name ? "Name" : "Year");
	sort_.set_tooltip_text(by_name ? "Sort by year" : "Sort by name");
	}

void AlbumPane::show_artist(std::shared_ptr<ApiClient> api, const Artist& artist)
	{
	clear();
	unsigned gen = gen_;
	albums_.clear();
	category_ = artist.category;
	set_title(artist.name);
	label_sort();
	sort_.set_visible(true);
	api->call("getArtist", {{"id", artist.ref.id}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			signal_failed.emit(r);
			return;
			}
		const auto& arr = jsub(jsub(r.body, "artist"), "album");
		for(size_t i=0; i<arr.size(); ++i)
			albums_.push_back(parse_album(api->session().server_key, jidx(arr, i)));
		render();
		});
	}

void AlbumPane::render()
	{
	// Year then title is the server's own order, so switching back to Year
	// restores exactly the list it sent.
	auto sorted = albums_;
	bool by_name = by_name_[category_];
	std::stable_sort(sorted.begin(), sorted.end(), [by_name](const Album& a, const Album& b) {
		int n = compare_names(a.title, b.title);
		if(by_name) return n!=0 ? n<0 : a.year<b.year;
		return a.year!=b.year ? a.year<b.year : n<0;
		});
	std::vector<Glib::RefPtr<ListRow>> rows;
	for(const auto& a: sorted)
		rows.push_back(ListRow::of(a));
	set_rows(rows);
	}

void AlbumPane::bind(RowWidget& w, const ListRow& row)
	{
	const auto& a = row.album;
	std::string meta;
	if(a.year)       meta = std::to_string(a.year);
	if(a.song_count) meta += (meta.empty() ? "" : " · ") + plural(a.song_count, "track", "tracks");
	w.show_item(a.cover_art, "", a.title, meta, "");
	}

void AlbumPane::activated(const ListRow& row)
	{
	signal_album.emit(row.album);
	}

// ---- TrackPane ----------------------------------------------------------

TrackPane::TrackPane(CoverStore& covers)
	: Pane(covers, 0, true)
	{
	hero_->set_visible(false);
	}

void TrackPane::show_album(std::shared_ptr<ApiClient> api, const Album& album)
	{
	clear();
	unsigned gen = gen_;
	set_title(album.title);
	hero_->set_visible(!album.cover_art.empty());
	hero_->show_cover(album.cover_art);
	api->call("getAlbum", {{"id", album.ref.id}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			signal_failed.emit(r);
			return;
			}
		std::vector<Song> songs;
		const auto& arr = jsub(jsub(r.body, "album"), "song");
		for(size_t i=0; i<arr.size(); ++i)
			songs.push_back(parse_song(api->session().server_key, jidx(arr, i)));

		std::set<int> discs;
		for(const auto& s: songs) discs.insert(s.disc);
		bool multi_disc = discs.size()>1;
		// Untagged files all say track 0 or 1; counting is more use then.
		bool sequential = std::all_of(songs.begin(), songs.end(),
		                              [](const Song& s) { return s.track<=1; });

		std::vector<Glib::RefPtr<ListRow>> rows;
		int disc = 0;
		for(size_t i=0; i<songs.size(); ++i) {
			const auto& s = songs[i];
			if(multi_disc && s.disc!=disc) {
				disc = s.disc;
				// A season of a series arrives as a disc.
				rows.push_back(ListRow::heading((s.season>0 ? "Series " : "Disc ") + std::to_string(disc)));
				}
			auto row = ListRow::of(s);
			int n = sequential ? int(i)+1 : s.track;
			row->text = n>0 ? std::to_string(n) : "";
			rows.push_back(row);
			}
		set_rows(rows);
		});
	}

void TrackPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::heading) {
		w.show_heading(row.text);
		return;
		}
	const auto& s = row.song;
	// The server sends the folder's spelling in `artist` when the tag merely
	// writes the same name differently, so an exact comparison is enough.
	std::string artist = (!s.display_album_artist.empty() && s.artist!=s.display_album_artist)
		? s.artist : "";
	w.show_item("", row.text, s.title, artist, format_duration(s.duration));
	}

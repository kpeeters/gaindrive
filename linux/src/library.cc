#include "panes.hh"
#include "apiclient.hh"
#include "coverstore.hh"
#include "infoblock.hh"
#include "jsonread.hh"
#include "settings.hh"

#include <algorithm>
#include <set>

namespace {
	int compare_names(const std::string& a, const std::string& b)
		{
		auto fa = Glib::ustring(a).casefold();
		auto fb = Glib::ustring(b).casefold();
		return fa<fb ? -1 : (fb<fa ? 1 : 0);
		}
}

// ---- ArtistPane ---------------------------------------------------------

ArtistPane::ArtistPane(CoverStore& covers)
	: Pane(covers, 0, false)
	{
	}

bool ArtistPane::loaded() const
	{
	return loaded_;
	}

void ArtistPane::load(std::shared_ptr<ApiClient> api)
	{
	clear();
	loaded_ = true;
	unsigned gen = gen_;
	set_title("Library");
	// Roots of each kind are listed apart, categories first, as on the web
	// client. contentType is a gaindrive extension: another Subsonic server
	// sends none, and gets one plain listing.
	api->call("getMusicFolders", {}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			loaded_ = false;
			report(r);
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
			loaded_ = false;
			report(r);
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
	signal_open_artist.emit(row.artist);
	}

// ---- AlbumPane ----------------------------------------------------------

AlbumPane::AlbumPane(CoverStore& covers, AppSettings& settings)
	: Pane(covers, 80, false),
	  settings_(settings),
	  bio_(std::make_unique<InfoBlock>(covers, 120))
	{
	sort_.set_visible(false);
	sort_.add_css_class("flat");
	sort_.signal_clicked().connect([this]() {
		settings_.sort_by_name[section()] = !settings_.sort_by_name[section()];
		settings_.save();
		label_sort();
		render();
		});
	header_.append(sort_);
	// Above the list rather than scrolling with it: a list in a box binds
	// every row at once, and an artist, or a category of radio shows, can
	// have hundreds of albums, each with a cover to fetch.
	// Its own scroller, so an expanded biography cannot push the albums
	// out of the pane.
	bio_scroll_.set_child(*bio_);
	bio_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
	bio_scroll_.set_propagate_natural_height(true);
	bio_scroll_.set_max_content_height(320);
	insert_child_after(bio_scroll_, header_);
	bio_scroll_.set_visible(false);
	bio_->property_visible().signal_changed().connect([this]() {
		bio_scroll_.set_visible(bio_->get_visible());
		});
	}

AlbumPane::~AlbumPane() = default;

const std::string& AlbumPane::artist_id() const
	{
	return artist_id_;
	}

std::string AlbumPane::section() const
	{
	return category_ ? "categories" : "artists";
	}

void AlbumPane::label_sort()
	{
	// The label says the state, the tooltip what pressing it does.
	bool by_name = settings_.sort_by_name[section()];
	sort_.set_label(by_name ? "Name" : "Year");
	sort_.set_tooltip_text(by_name ? "Sort by year" : "Sort by name");
	}

void AlbumPane::clear()
	{
	Pane::clear();
	bio_->clear();
	sort_.set_visible(false);
	artist_id_.clear();
	albums_.clear();
	}

void AlbumPane::show_artist(std::shared_ptr<ApiClient> api, const Artist& artist)
	{
	clear();
	unsigned gen = gen_;
	artist_id_ = artist.ref.id;
	category_  = artist.category;
	set_title(artist.name);
	label_sort();
	sort_.set_visible(true);
	// A category is a section of the library, with nothing to look up.
	if(!artist.category)
		bio_->load(api, "getArtistInfo2", artist.ref.id, "artistInfo2", "biography", artist.ref.id);
	api->call("getArtist", {{"id", artist.ref.id}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
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
	bool by_name = settings_.sort_by_name[section()];
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
	w.show_actions(true, a.starred, RowWidget::ListButton::none, false);
	}

void AlbumPane::activated(const ListRow& row)
	{
	signal_album.emit(row.album);
	}

// ---- TrackPane ----------------------------------------------------------

TrackPane::TrackPane(CoverStore& covers)
	: Pane(covers, 0, true),
	  hero_(std::make_unique<CoverImage>(covers, 320)),
	  notes_(std::make_unique<InfoBlock>(covers, 0)),
	  texts_(Gtk::Orientation::VERTICAL, 6),
	  texts_nav_(Gtk::Orientation::HORIZONTAL, 6)
	{
	// The album's further images, behind arrows over the cover.
	hero_box_.set_child(*hero_);
	hero_box_.set_halign(Gtk::Align::CENTER);
	hero_box_.add_css_class("hero");
	img_prev_.set_icon_name("gd-chevron-left-symbolic");
	img_prev_.set_tooltip_text("Previous image");
	img_prev_.set_halign(Gtk::Align::START);
	img_next_.set_icon_name("gd-chevron-right-symbolic");
	img_next_.set_tooltip_text("Next image");
	img_next_.set_halign(Gtk::Align::END);
	for(auto b: {&img_prev_, &img_next_}) {
		b->set_valign(Gtk::Align::CENTER);
		b->set_margin(6);
		b->add_css_class("osd");
		b->add_css_class("circular");
		hero_box_.add_overlay(*b);
		}
	img_prev_.signal_clicked().connect([this]() { if(image_>0) show_image(image_-1); });
	img_next_.signal_clicked().connect([this]() { if(image_+1<images_) show_image(image_+1); });
	top_.append(hero_box_);
	notes_->set_margin_start(12);
	notes_->set_margin_end(12);
	top_.append(*notes_);

	text_prev_.set_icon_name("gd-chevron-left-symbolic");
	text_prev_.add_css_class("flat");
	text_next_.set_icon_name("gd-chevron-right-symbolic");
	text_next_.add_css_class("flat");
	text_name_.add_css_class("heading");
	text_name_.set_hexpand(true);
	text_name_.set_xalign(0);
	texts_nav_.append(text_prev_);
	texts_nav_.append(text_name_);
	texts_nav_.append(text_next_);
	text_body_.set_wrap(true);
	text_body_.set_xalign(0);
	text_body_.set_selectable(true);
	text_body_.add_css_class("monospace");
	texts_.append(texts_nav_);
	texts_.append(text_body_);
	texts_.add_css_class("liner-notes");
	text_prev_.signal_clicked().connect([this]() { if(text_>0) show_text(text_-1); });
	text_next_.signal_clicked().connect([this]() { if(text_+1<text_files_.size()) show_text(text_+1); });
	bottom_.append(texts_);

	clear();
	}

TrackPane::~TrackPane() = default;

void TrackPane::clear()
	{
	Pane::clear();
	hero_->show_cover("");
	hero_box_.set_visible(false);
	image_  = 0;
	images_ = 1;
	img_prev_.set_visible(false);
	img_next_.set_visible(false);
	notes_->clear();
	texts_.set_visible(false);
	text_files_.clear();
	songs_.clear();
	album_ = Album{};
	api_.reset();
	}

void TrackPane::show_album(std::shared_ptr<ApiClient> api, const std::string& album_id,
                           const std::string& play_id)
	{
	clear();
	unsigned gen = gen_;
	api_ = api;
	notes_->load(api, "getAlbumInfo2", album_id, "albumInfo2", "notes", "");
	api->call("getAlbum", {{"id", album_id}}, [this, api, gen, album_id, play_id](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& server = api->session().server_key;
		const auto& album  = jsub(r.body, "album");
		album_ = parse_album(server, album);
		set_title(album_.title);
		if(!album_.cover_art.empty()) {
			hero_box_.set_visible(true);
			show_image(0);
			api->call("getAlbumImages", {{"id", album_.cover_art}}, [this, gen](const ApiResult& r) {
				if(gen!=gen_ || !r.ok) return;
				images_ = std::max(1, jint(jsub(r.body, "albumImages"), "count"));
				show_image(image_);
				});
			}
		load_texts(api, album_id);

		const auto& arr = jsub(album, "song");
		for(size_t i=0; i<arr.size(); ++i)
			songs_.push_back(parse_song(server, jidx(arr, i)));

		std::set<int> discs;
		for(const auto& s: songs_) discs.insert(s.disc);
		bool multi_disc = discs.size()>1;
		// Untagged files all say track 0 or 1; counting is more use then.
		bool sequential = std::all_of(songs_.begin(), songs_.end(),
		                              [](const Song& s) { return s.track<=1; });

		std::vector<Glib::RefPtr<ListRow>> rows;
		int disc = 0;
		for(size_t i=0; i<songs_.size(); ++i) {
			const auto& s = songs_[i];
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
		signal_loaded.emit(album_);

		if(!play_id.empty())
			for(size_t i=0; i<songs_.size(); ++i)
				if(songs_[i].ref.id==play_id) {
					signal_play.emit(songs_, i);
					break;
					}
		});
	}

void TrackPane::show_image(int index)
	{
	image_ = index;
	hero_->show_cover(album_.cover_art, index);
	img_prev_.set_visible(images_>1);
	img_next_.set_visible(images_>1);
	img_prev_.set_sensitive(index>0);
	img_next_.set_sensitive(index+1<images_);
	}

void TrackPane::load_texts(std::shared_ptr<ApiClient> api, const std::string& album_id)
	{
	unsigned gen = gen_;
	api->call("getAlbumTexts", {{"id", album_id}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_ || !r.ok) return;
		const auto& files = jsub(jsub(r.body, "albumTexts"), "textFile");
		for(size_t i=0; i<files.size(); ++i)
			text_files_.push_back(jstr(jidx(files, i), "name"));
		if(text_files_.empty()) return;
		texts_.set_visible(true);
		text_prev_.set_visible(text_files_.size()>1);
		text_next_.set_visible(text_files_.size()>1);
		show_text(0);
		});
	}

void TrackPane::show_text(size_t index)
	{
	if(!api_) return;
	text_ = index;
	text_name_.set_text(text_files_[index]);
	text_body_.set_text("");
	text_prev_.set_sensitive(index>0);
	text_next_.set_sensitive(index+1<text_files_.size());
	unsigned gen = gen_;
	api_->text("getAlbumText", {{"id", album_.ref.id}, {"name", text_files_[index]}},
	           [this, gen, index](bool ok, const std::string& body) {
		if(gen!=gen_ || index!=text_) return;
		text_body_.set_text(ok ? body : "(could not load file)");
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
	w.show_actions(true, s.starred, RowWidget::ListButton::add, true);
	}

void TrackPane::activated(const ListRow& row)
	{
	for(size_t i=0; i<songs_.size(); ++i)
		if(songs_[i].ref.id==row.song.ref.id) {
			signal_play.emit(songs_, i);
			break;
			}
	// The playing mark shows what plays; a lingering selection would only
	// stop the same row from being clicked again.
	unselect();
	}

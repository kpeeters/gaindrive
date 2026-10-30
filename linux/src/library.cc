#include "panes.hh"
#include "albumedit.hh"
#include "apiclient.hh"
#include "coverstore.hh"
#include "infoblock.hh"
#include "merge.hh"
#include "jsonread.hh"
#include "settings.hh"
#include "stamp.hh"

#include <algorithm>
#include <iostream>
#include <map>
#include <sstream>
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
	scope_.set_icon_name("gd-server-symbolic");
	scope_.set_tooltip_text("Servers");
	scope_.add_css_class("flat");
	scope_.add_css_class("scope-button");
	scope_.set_valign(Gtk::Align::CENTER);
	scope_.set_visible(false);
	scope_list_.set_margin_start(6);
	scope_list_.set_margin_end(6);
	scope_list_.set_margin_top(6);
	scope_list_.set_margin_bottom(6);
	scope_popover_.set_child(scope_list_);
	scope_.set_popover(scope_popover_);
	header_.append(scope_);
	uploads_.set_icon_name("gd-uploads-symbolic");
	uploads_.set_tooltip_text("Uploads");
	uploads_.add_css_class("flat");
	uploads_.add_css_class("uploads-button");
	uploads_.set_valign(Gtk::Align::CENTER);
	uploads_.set_visible(false);
	uploads_.signal_clicked().connect([this]() { signal_uploads.emit(); });
	header_.append(uploads_);
	}

void ArtistPane::set_uploads_button(bool on)
	{
	uploads_.set_visible(on);
	}

void ArtistPane::set_servers(const std::vector<std::pair<std::string, std::string>>& servers,
                             const std::string& chosen)
	{
	while(auto child = scope_list_.get_first_child())
		scope_list_.remove(*child);
	// Nothing to choose between with one server.
	scope_.set_visible(servers.size()>1);
	if(servers.size()<2) return;
	// A choice since logged out or removed means all, as scope() reads it.
	bool known = false;
	for(const auto& s: servers)
		if(s.first==chosen) known = true;
	auto all = Gtk::make_managed<Gtk::CheckButton>("All servers");
	all->set_active(!known);
	scope_list_.append(*all);
	std::vector<std::pair<Gtk::CheckButton*, std::string>> buttons = {{all, ""}};
	for(const auto& [key, label]: servers) {
		auto b = Gtk::make_managed<Gtk::CheckButton>(label);
		b->set_group(*all);
		b->set_active(key==chosen);
		scope_list_.append(*b);
		buttons.push_back({b, key});
		}
	for(const auto& [b, key]: buttons)
		b->signal_toggled().connect([this, b=b, key=key]() {
			if(!b->get_active()) return;
			scope_popover_.popdown();
			signal_scope.emit(key);
			});
	}

bool ArtistPane::loaded() const
	{
	return loaded_;
	}

namespace {
	// One server's getArtists, with each artist's letter bucket and the key
	// it sorts by within it.
	struct Indexed {
		std::vector<Artist>      artists;
		std::vector<std::string> buckets, sort_keys;
	};

	// The name as the server sorts it: without a leading "The" or such,
	// from the response's ignoredArticles.
	std::string sort_key(const std::string& name, const std::vector<std::string>& articles)
		{
		std::string folded = Glib::ustring(name).casefold();
		for(const auto& a: articles)
			if(folded.size()>a.size() && folded.compare(0, a.size(), a)==0 && folded[a.size()]==' ')
				return folded.substr(a.size()+1);
		return folded;
		}

	Indexed parse_index(const ApiResult& r, bool category)
		{
		Indexed out;
		const auto& root = jsub(r.body, "artists");
		std::vector<std::string> articles;
		std::istringstream words(jstr(root, "ignoredArticles"));
		for(std::string w; words >> w;)
			articles.push_back(Glib::ustring(w).casefold());
		const auto& index = jsub(root, "index");
		for(size_t i=0; i<index.size(); ++i) {
			const auto& bucket = jidx(index, i);
			const auto& arr    = jsub(bucket, "artist");
			for(size_t k=0; k<arr.size(); ++k) {
				out.artists.push_back(parse_artist(r.server, jidx(arr, k), category));
				out.buckets.push_back(jstr(bucket, "name"));
				out.sort_keys.push_back(sort_key(out.artists.back().name, articles));
				}
			}
		return out;
		}
}

void ArtistPane::load(const std::vector<std::shared_ptr<ApiClient>>& apis)
	{
	clear();
	loaded_ = true;
	unsigned gen = gen_;
	set_title("Library");
	// Roots of each kind are listed apart, categories first, as on the web
	// client. contentType is a gaindrive extension: another Subsonic server
	// sends none, and gets one plain listing.
	call_each(apis, "getMusicFolders", {}, [this, apis, gen](const std::vector<ApiResult>& rs) {
		if(gen!=gen_) return;
		ServerCalls categories, artists;
		for(size_t i=0; i<rs.size(); ++i) {
			if(!rs[i].ok) {
				loaded_ = false;
				report(rs[i]);
				continue;
				}
			std::set<std::string> types;
			const auto& folders = jsub(jsub(rs[i].body, "musicFolders"), "musicFolder");
			for(size_t k=0; k<folders.size(); ++k)
				types.insert(jstr(jidx(folders, k), "contentType"));
			bool typed = types.count("categories") || types.count("artists");
			if(types.count("categories"))
				categories.push_back({apis[i], {{"contentType", "categories"}}});
			if(!typed)
				artists.push_back({apis[i], {}});
			else if(types.count("artists"))
				artists.push_back({apis[i], {{"contentType", "artists"}}});
			}
		call_each(categories, "getArtists", [this, gen, artists](const std::vector<ApiResult>& cr) {
			if(gen!=gen_) return;
			call_each(artists, "getArtists", [this, gen, cr](const std::vector<ApiResult>& ar) {
				if(gen!=gen_) return;
				show_lists(cr, ar);
				});
			});
		});
	}

void ArtistPane::show_lists(const std::vector<ApiResult>& categories,
                            const std::vector<ApiResult>& artists)
	{
	std::vector<Glib::RefPtr<ListRow>> rows;
	// A handful of sections, so one heading over an alphabetical list
	// rather than the server's letter buckets.
	std::vector<std::vector<Artist>> lists;
	for(const auto& r: categories) {
		if(!r.ok) {
			loaded_ = false;
			report(r);
			continue;
			}
		lists.push_back(parse_index(r, true).artists);
		}
	auto list = merge_artists(lists);
	std::sort(list.begin(), list.end(), [](const Artist& a, const Artist& b) {
		return compare_names(a.name, b.name)<0;
		});
	if(!list.empty())
		rows.push_back(ListRow::heading("Categories"));
	for(const auto& a: list)
		rows.push_back(ListRow::of(a));

	std::vector<Indexed> indexes;
	for(const auto& r: artists) {
		if(!r.ok) {
			loaded_ = false;
			report(r);
			continue;
			}
		indexes.push_back(parse_index(r, false));
		}
	// Each artist under its bucket, as the first server to list it puts it.
	struct Placed {
		std::string bucket, key;
		Artist      artist;
	};
	std::vector<Placed> placed;
	if(indexes.size()==1) {
		// As the server sent it.
		const auto& ix = indexes.front();
		for(size_t i=0; i<ix.artists.size(); ++i)
			placed.push_back({ix.buckets[i], ix.sort_keys[i], ix.artists[i]});
		}
	else {
		std::map<std::string, std::pair<std::string, std::string>> where;
		lists.clear();
		for(const auto& ix: indexes) {
			for(size_t i=0; i<ix.artists.size(); ++i)
				where[ix.artists[i].ref.server + "|" + ix.artists[i].ref.id] = {ix.buckets[i], ix.sort_keys[i]};
			lists.push_back(ix.artists);
			}
		for(auto& a: merge_artists(lists)) {
			const auto& [bucket, key] = where[a.ref.server + "|" + a.ref.id];
			placed.push_back({bucket, key, std::move(a)});
			}
		std::stable_sort(placed.begin(), placed.end(), [](const Placed& a, const Placed& b) {
			int n = compare_names(a.bucket, b.bucket);
			return n!=0 ? n<0 : a.key<b.key;
			});
		}
	for(size_t i=0; i<placed.size(); ++i) {
		if(i==0 || placed[i].bucket!=placed[i-1].bucket)
			rows.push_back(ListRow::heading(placed[i].bucket));
		rows.push_back(ListRow::of(placed[i].artist));
		}
	set_rows(rows);
	}

void ArtistPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::heading)
		w.show_heading(row.text);
	else
		w.show_item({}, "", row.artist.name, "", plural(row.artist.album_count, "album", "albums"));
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
	}

AlbumPane::~AlbumPane() = default;

bool AlbumPane::shows_artist(const ItemRef& ref) const
	{
	for(const auto& r: artist_)
		if(r.server==ref.server && r.id==ref.id) return true;
	return false;
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
	artist_.clear();
	albums_.clear();
	}

void AlbumPane::show_artist(const ServerCalls& sources, const Artist& artist)
	{
	clear();
	if(sources.empty()) return;
	unsigned gen = gen_;
	for(const auto& [api, p]: sources)
		artist_.push_back({api->session().server_key, p.front().second});
	category_  = artist.category;
	artist_name_ = artist.name;
	set_title(artist.name);
	label_sort();
	sort_.set_visible(true);
	// A category is a section of the library, with nothing to look up. The
	// biography from the first server, as the artist's row is.
	const auto& first = artist_.front();
	if(!artist.category)
		bio_->load(sources.front().first, "getArtistInfo2", first.id, "artistInfo2", "biography", first.id);
	call_each(sources, "getArtist", [this, gen](const std::vector<ApiResult>& rs) {
		if(gen!=gen_) return;
		std::vector<std::vector<Album>> lists;
		for(const auto& r: rs) {
			if(!r.ok) {
				report(r);
				continue;
				}
			// Reached through one of its albums, a category comes as a plain
			// artist; only the server knows better.
			const auto& cat = jsub(jsub(r.body, "artist"), "isCategory");
			if(!category_ && cat.is_boolean() && cat.get<bool>()) {
				category_ = true;
				bio_->clear();
				label_sort();
				}
			lists.emplace_back();
			const auto& arr = jsub(jsub(r.body, "artist"), "album");
			for(size_t i=0; i<arr.size(); ++i)
				lists.back().push_back(parse_album(r.server, jidx(arr, i)));
			}
		if(settings_.merge_albums)
			albums_ = merge_albums(lists);
		else
			for(const auto& l: lists)
				albums_.insert(albums_.end(), l.begin(), l.end());
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
	// The biography as the list's first row, so it scrolls with the albums
	// as on the web, however far it expands. Not in a box above the list:
	// a list in a box binds every row at once, and an artist can have
	// hundreds of albums, each with a cover to fetch.
	if(!category_)
		rows.push_back(ListRow::extra());
	for(const auto& a: sorted)
		rows.push_back(ListRow::of(a));
	set_rows(rows);
	}

void AlbumPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::extra) {
		w.show_extra(*bio_);
		return;
		}
	const auto& a = row.album;
	std::string meta;
	if(a.year)       meta = std::to_string(a.year);
	if(a.song_count) meta += (meta.empty() ? "" : " · ") + plural(a.song_count, "track", "tracks");
	auto where = badges(a.ref, a.also);
	if(!where.empty()) meta += (meta.empty() ? "" : " · ") + where;
	w.show_item({a.ref.server, a.cover_art}, "", a.title, meta, "");
	show_album_videos(w, a);
	w.show_actions(true, a.starred, RowWidget::ListButton::none, false);
	w.show_upload_actions(uploads_, uploads_ && admin_);
	}

void AlbumPane::set_uploads(bool on, bool admin)
	{
	uploads_ = on;
	admin_   = admin;
	}

void AlbumPane::upload_action(const ListRow& row, bool promote)
	{
	if(row.kind!=ListRow::Kind::album) return;
	if(promote) signal_promote.emit(row.album, artist_name_);
	else        signal_delete_upload.emit(row.album, artist_name_);
	}

void AlbumPane::activated(const ListRow& row)
	{
	signal_album.emit(row.album);
	}

Glib::ustring AlbumPane::search_text(const ListRow& row) const
	{
	// The rows here are one artist's, so the name would match every one.
	if(row.kind!=ListRow::Kind::album) return Pane::search_text(row);
	std::string t = row.album.title;
	if(row.album.year) t += " " + std::to_string(row.album.year);
	return Glib::ustring(t).lowercase();
	}

// ---- TrackPane ----------------------------------------------------------

TrackPane::TrackPane(CoverStore& covers)
	: Pane(covers, 0, true),
	  editor_(std::make_unique<AlbumEditor>()),
	  hero_(std::make_unique<CoverImage>(covers, 320)),
	  notes_(std::make_unique<InfoBlock>(covers, 0)),
	  texts_(Gtk::Orientation::VERTICAL, 6),
	  texts_nav_(Gtk::Orientation::HORIZONTAL, 6),
	  name_clip_(std::make_unique<HeightClip>(text_name_)),
	  text_clip_(std::make_unique<HeightClip>(text_body_))
	{
	star_.add_css_class("flat");
	star_.add_css_class("row-action");
	star_.add_css_class("album-star");
	star_.set_valign(Gtk::Align::CENTER);
	star_.signal_clicked().connect([this]() {
		signal_star.emit(ListRow::Kind::album, album_.ref, !album_.starred);
		});
	header_.append(star_);

	// Edit mode, as on the web: the title gives way to the album and artist
	// fields, Edit to Save and Cancel, the listing to a grid of fields.
	edit_.set_label("Edit");
	edit_.add_css_class("flat");
	edit_.set_valign(Gtk::Align::CENTER);
	edit_.signal_clicked().connect(sigc::mem_fun(*this, &TrackPane::start_edit));
	header_.insert_child_after(editor_->fields, title_);
	header_.append(edit_);
	header_.append(editor_->buttons);
	editor_->signal_preview.connect([this](const Glib::RefPtr<Gdk::Texture>& tex) {
		if(!tex) {
			show_hero();
			return;
			}
		hero_->show_cover({});
		hero_->set(tex);
		});
	editor_->signal_cancelled.connect(sigc::mem_fun(*this, &TrackPane::end_edit));
	editor_->signal_saved.connect([this](const AlbumEditor::Saved& saved) {
		if(saved.cover && !album_.cover_art.empty())
			covers_.forget({album_.ref.server, album_.cover_art});
		if(saved.reload) {
			end_edit();
			signal_edited.emit(saved.artist, saved.album);
			return;
			}
		songs_ = saved.songs;
		end_edit();
		});

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
	hero_box_.add_overlay(editor_->pencil);
	img_prev_.signal_clicked().connect([this]() { if(image_>0) show_image(image_-1); });
	img_next_.signal_clicked().connect([this]() { if(image_+1<images_) show_image(image_+1); });
	// No wider than the pane: the scaled cover would widen a narrow one
	// past its scrollbar.
	hero_fit_ = std::make_unique<WidthFit>(hero_box_);
	// Sized from the pane, not from the width given to the fit: that one
	// loses the scrollbar's width when it appears, and the smaller cover
	// that follows can make the scrollbar go away again, forever.
	hero_fit_->signal_width.connect([this](int) { hero_->set_max_px(int(get_width()*0.95)); });
	top_.append(*hero_fit_);
	notes_->set_margin_start(12);
	notes_->set_margin_end(12);
	top_.append(*notes_);
	editor_->tracks.set_margin_start(12);
	editor_->tracks.set_margin_end(12);
	top_.append(editor_->tracks);

	text_prev_.set_icon_name("gd-chevron-left-symbolic");
	text_prev_.add_css_class("flat");
	text_next_.set_icon_name("gd-chevron-right-symbolic");
	text_next_.add_css_class("flat");
	text_name_.add_css_class("heading");
	text_name_.set_xalign(0);
	// File names can be as long as the rules in them, so as the text below.
	text_name_.set_wrap(true);
	text_name_.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
	name_clip_->set_hexpand(true);
	// Arrows at their own size beside a name on two lines.
	text_prev_.set_valign(Gtk::Align::CENTER);
	text_next_.set_valign(Gtk::Align::CENTER);
	texts_nav_.append(text_prev_);
	texts_nav_.append(*name_clip_);
	texts_nav_.append(text_next_);
	text_body_.set_wrap(true);
	// Text files hold rules and URLs longer than the pane; breaking those
	// too, and the clip asking no more width than that, keeps the file
	// from widening the pane.
	text_body_.set_wrap_mode(Pango::WrapMode::WORD_CHAR);
	text_body_.set_xalign(0);
	text_body_.set_selectable(true);
	// As InfoBlock's text: no select-all when focus wanders onto it.
	text_body_.set_focusable(false);
	text_body_.add_css_class("monospace");
	texts_.append(texts_nav_);
	texts_.append(*text_clip_);
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
	editor_->stop();
	title_.set_visible(true);
	edit_.set_visible(false);
	hero_->show_cover({});
	hero_box_.set_visible(false);
	image_  = 0;
	images_ = 1;
	img_prev_.set_visible(false);
	img_next_.set_visible(false);
	notes_->clear();
	texts_.set_visible(false);
	text_files_.clear();
	songs_.clear();
	chapters_.clear();
	album_ = Album{};
	api_.reset();
	star_.set_visible(false);
	}

void TrackPane::star_changed(ListRow::Kind kind, const ItemRef& ref, bool on)
	{
	Pane::star_changed(kind, ref, on);
	if(kind==ListRow::Kind::album && ref.id==album_.ref.id && ref.server==album_.ref.server) {
		album_.starred = on;
		show_star();
		}
	}

void TrackPane::show_star()
	{
	star_.set_visible(true);
	star_.set_icon_name(album_.starred ? "gd-star-filled-symbolic" : "gd-star-symbolic");
	star_.set_tooltip_text(album_.starred ? "Unstar album" : "Star album");
	if(album_.starred) star_.add_css_class("starred");
	else               star_.remove_css_class("starred");
	}

void TrackPane::show_album(std::shared_ptr<ApiClient> api, const std::string& album_id,
                           const std::string& play_id, const std::string& mark_id)
	{
	clear();
	unsigned gen = gen_;
	api_ = api;
	mark_id_ = mark_id;
	notes_->load(api, "getAlbumInfo2", album_id, "albumInfo2", "notes", "");
	api->call("getAlbum", {{"id", album_id}}, [this, api, gen, album_id, play_id, mark_id](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& server = api->session().server_key;
		const auto& album  = jsub(r.body, "album");
		album_ = parse_album(server, album);
		set_title(album_.title);
		show_star();
		edit_.set_visible(album_.writable);
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
		show_rows();
		if(!mark_id.empty())
			scroll_to([mark_id](const ListRow& row) {
				return row.kind==ListRow::Kind::song && row.song.ref.id==mark_id;
				});
		// Films may carry chapters, listed under them once they arrive.
		if(std::any_of(songs_.begin(), songs_.end(), [](const Song& s) { return s.is_video; }))
			api->call("getAlbumChapters", {{"id", album_id}}, [this, gen, album_id](const ApiResult& r) {
				if(gen!=gen_) return;
				if(!r.ok) {
					// Not reported: the tracks are listed all the same.
					std::cout << stamp() << "getAlbumChapters " << album_id << ": " << r.error << std::endl;
					return;
					}
				const auto& films = jsub(jsub(r.body, "albumChapters"), "song");
				for(size_t i=0; i<films.size(); ++i) {
					const auto& f = jidx(films, i);
					auto& list = chapters_[jstr(f, "id")];
					const auto& ch = jsub(f, "chapter");
					for(size_t k=0; k<ch.size(); ++k)
						list.push_back({jnum(jidx(ch, k), "start"), jint(jidx(ch, k), "duration"),
						                jstr(jidx(ch, k), "name")});
					}
				if(!chapters_.empty()) show_rows();
				});
		signal_loaded.emit(album_);

		if(!play_id.empty())
			for(size_t i=0; i<songs_.size(); ++i)
				if(songs_[i].ref.id==play_id) {
					signal_play.emit(songs_, i);
					break;
					}
		});
	}

void TrackPane::show_rows()
	{
	// Chapters arriving late must not put the listing back over the fields.
	if(editor_->editing()) return;
	std::set<int> discs;
	for(const auto& s: songs_) discs.insert(s.disc);
	bool multi_disc = discs.size()>1;

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
		row->text = number(i);
		rows.push_back(row);
		auto ch = chapters_.find(s.ref.id);
		if(ch!=chapters_.end())
			for(size_t k=0; k<ch->second.size(); ++k) {
				const auto& c = ch->second[k];
				rows.push_back(ListRow::chapter(s, int(k)+1, c.name, c.start, c.duration));
				}
		}
	set_rows(rows);
	}

std::string TrackPane::number(size_t i) const
	{
	// Untagged files all say track 0 or 1; counting is more use then.
	bool sequential = std::all_of(songs_.begin(), songs_.end(),
	                              [](const Song& s) { return s.track<=1; });
	int n = sequential ? int(i)+1 : songs_[i].track;
	return n>0 ? std::to_string(n) : "";
	}

void TrackPane::start_edit()
	{
	if(!api_ || editor_->editing()) return;
	std::vector<std::string> numbers;
	for(size_t i=0; i<songs_.size(); ++i)
		numbers.push_back(number(i));
	editor_->start(api_, album_, songs_, numbers);
	title_.set_visible(false);
	edit_.set_visible(false);
	show_hero();
	set_rows({});
	}

void TrackPane::end_edit()
	{
	editor_->stop();
	title_.set_visible(true);
	edit_.set_visible(album_.writable);
	show_hero();
	show_rows();
	}

void TrackPane::show_hero()
	{
	if(!album_.cover_art.empty()) {
		hero_box_.set_visible(true);
		show_image(editor_->editing() ? 0 : image_);
		if(editor_->editing()) {
			img_prev_.set_visible(false);
			img_next_.set_visible(false);
			}
		return;
		}
	// Somewhere for the pencil to sit while there is no cover yet.
	hero_->show_cover({});
	if(editor_->editing()) hero_->set_from_icon_name("gd-music-note-symbolic");
	hero_box_.set_visible(editor_->editing());
	}

void TrackPane::show_image(int index)
	{
	image_ = index;
	hero_->show_cover({album_.ref.server, album_.cover_art}, index);
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

void TrackPane::set_chapter(const ItemRef& film, int index)
	{
	if(film.id==chapter_film_.id && film.server==chapter_film_.server && index==chapter_) return;
	chapter_film_ = film;
	chapter_      = index;
	rebind_all();
	}

bool TrackPane::is_playing(const ListRow& row) const
	{
	if(row.kind!=ListRow::Kind::chapter) return Pane::is_playing(row);
	return row.song.ref.id==chapter_film_.id && row.song.ref.server==chapter_film_.server
	    && row.index==chapter_+1;
	}

Glib::ustring TrackPane::search_text(const ListRow& row) const
	{
	// One album's tracks: its title and artist would match every row, so
	// only what bind() shows counts.
	if(row.kind!=ListRow::Kind::song) return Pane::search_text(row);
	const auto& s = row.song;
	std::string t = s.title;
	if(!s.display_album_artist.empty() && s.artist!=s.display_album_artist)
		t += " " + s.artist;
	return Glib::ustring(t).lowercase();
	}

void TrackPane::bind(RowWidget& w, const ListRow& row)
	{
	// Recycled widgets: the class must come off as surely as it goes on.
	if(row.kind==ListRow::Kind::chapter) w.add_css_class("chapter");
	else                                 w.remove_css_class("chapter");
	if(row.kind==ListRow::Kind::song && !mark_id_.empty() && row.song.ref.id==mark_id_)
		w.add_css_class("peer");
	else
		w.remove_css_class("peer");
	if(row.kind==ListRow::Kind::heading) {
		w.show_heading(row.text);
		return;
		}
	if(row.kind==ListRow::Kind::chapter) {
		w.show_item({}, std::to_string(row.index),
		            row.text.empty() ? "Chapter " + std::to_string(row.index) : row.text,
		            "", row.duration>0 ? format_duration(row.duration) : "");
		// No star, playlist or queue: they all address a song, and a
		// chapter is not one. The film's own row has them.
		return;
		}
	const auto& s = row.song;
	// The server sends the folder's spelling in `artist` when the tag merely
	// writes the same name differently, so an exact comparison is enough.
	std::string artist = (!s.display_album_artist.empty() && s.artist!=s.display_album_artist)
		? s.artist : "";
	w.show_item({}, row.text, s.title, artist, format_duration(s.duration));
	w.show_actions(true, s.starred, RowWidget::ListButton::add, true);
	}

void TrackPane::activated(const ListRow& row)
	{
	for(size_t i=0; i<songs_.size(); ++i)
		if(songs_[i].ref.id==row.song.ref.id) {
			if(row.kind==ListRow::Kind::chapter)
				signal_play_chapter.emit(songs_, i, row.start);
			else
				signal_play.emit(songs_, i);
			break;
			}
	// The playing mark shows what plays; a lingering selection would only
	// stop the same row from being clicked again.
	unselect();
	}

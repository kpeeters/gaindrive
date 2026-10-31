#include "panes.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "merge.hh"
#include "serverconfig.hh"
#include "settings.hh"
#include "stamp.hh"

#include <gtkmm/alertdialog.h>
#include <gtkmm/window.h>

#include <iostream>

// ---- PlaylistsPane ------------------------------------------------------

PlaylistsPane::PlaylistsPane(CoverStore& covers)
	: Pane(covers, 0, false)
	{
	row_badges_ = false;
	}

void PlaylistsPane::load(const std::vector<std::shared_ptr<ApiClient>>& apis)
	{
	clear();
	apis_ = apis;
	unsigned gen = gen_;
	set_title("Playlists");
	call_each(apis, "getPlaylists", {}, [this, apis, gen](const std::vector<ApiResult>& lists) {
		if(gen!=gen_) return;
		call_each(apis, "getStarred2", {}, [this, gen, lists](const std::vector<ApiResult>& stars) {
			if(gen!=gen_) return;
			// Server by server, as each knows only its own.
			std::vector<Glib::RefPtr<ListRow>> rows;
			for(size_t k=0; k<lists.size(); ++k) {
				const auto& r = lists[k];
				if(!r.ok) {
					report(r);
					continue;
					}
				const auto& server = r.server;
				const auto& pls = jsub(jsub(r.body, "playlists"), "playlist");
				std::string heading = heading_on("Playlists", server);
				if(!pls.empty() && heading!="Playlists")
					rows.push_back(ListRow::heading(heading));
				for(size_t i=0; i<pls.size(); ++i)
					rows.push_back(ListRow::of(parse_playlist(server, jidx(pls, i))));
				// The playlists stand on their own if this fails.
				const auto& starred = jsub(stars[k].body, "starred2");
				const auto& albums  = jsub(starred, "album");
				const auto& songs   = jsub(starred, "song");
				if(!albums.empty())
					rows.push_back(ListRow::heading(heading_on("Starred albums", server)));
				for(size_t i=0; i<albums.size(); ++i)
					rows.push_back(ListRow::of(parse_album(server, jidx(albums, i))));
				if(!songs.empty())
					rows.push_back(ListRow::heading(heading_on("Starred tracks", server)));
				for(size_t i=0; i<songs.size(); ++i)
					rows.push_back(ListRow::of(parse_song(server, jidx(songs, i))));
				}
			set_empty(rows.empty() ? "No playlists or starred items yet." : "");
			set_rows(rows);
			});
		});
	}

void PlaylistsPane::star_changed(ListRow::Kind, const ItemRef&, bool)
	{
	// The starred sections are this pane's content, not a mark on it.
	if(!apis_.empty()) load(apis_);
	}

void PlaylistsPane::bind(RowWidget& w, const ListRow& row)
	{
	switch(row.kind) {
		case ListRow::Kind::heading:
			w.show_heading(row.text);
			break;
		case ListRow::Kind::playlist: {
			const auto& p = row.playlist;
			std::string meta = plural(p.song_count, "track", "tracks");
			if(p.duration) meta += " · " + format_duration(p.duration);
			w.show_item({}, "", p.name, meta, "");
			break;
			}
		case ListRow::Kind::album:
			bind_album_hit(w, row.album);
			break;
		default:
			bind_song_hit(w, row.song, format_duration(row.song.duration));
			break;
		}
	}

void PlaylistsPane::activated(const ListRow& row)
	{
	if(row.kind==ListRow::Kind::playlist)
		signal_playlist.emit(row.playlist);
	else if(row.kind==ListRow::Kind::album)
		signal_open_album.emit(row.album.ref, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit({row.song.ref.server, row.song.album_id}, row.song.ref.id);
	}

// ---- PlaylistPane -------------------------------------------------------

PlaylistPane::PlaylistPane(CoverStore& covers)
	: Pane(covers, 0, false)
	{
	}

void PlaylistPane::show_playlist(std::shared_ptr<ApiClient> api, const Playlist& pl)
	{
	api_      = api;
	playlist_ = pl;
	reload();
	}

void PlaylistPane::reload()
	{
	clear();
	songs_.clear();
	unsigned gen = gen_;
	set_title(playlist_.name);
	auto api = api_;
	api->call("getPlaylist", {{"id", playlist_.ref.id}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& pl = jsub(r.body, "playlist");
		// Only the owner may change it; the server refuses anyone else.
		owner_ = jstr(pl, "owner")==api->session().user;
		const auto& entries = jsub(pl, "entry");
		std::vector<Glib::RefPtr<ListRow>> rows;
		for(size_t i=0; i<entries.size(); ++i) {
			songs_.push_back(parse_song(api->session().server_key, jidx(entries, i)));
			auto row = ListRow::of(songs_.back());
			row->index = int(i);
			row->text  = std::to_string(i+1);
			rows.push_back(row);
			}
		set_empty(rows.empty() ? "This playlist is empty." : "");
		set_rows(rows);
		});
	}

void PlaylistPane::bind(RowWidget& w, const ListRow& row)
	{
	const auto& s = row.song;
	w.show_item({}, row.text, s.title, s.artist, format_duration(s.duration));
	if(s.resumable()) w.show_watch(s.watched, s.watch_progress());
	w.show_actions(true, s.starred,
	               owner_ ? RowWidget::ListButton::remove : RowWidget::ListButton::add, true);
	}

void PlaylistPane::activated(const ListRow& row)
	{
	if(row.index>=0 && size_t(row.index)<songs_.size())
		signal_play.emit(songs_, size_t(row.index));
	unselect();
	}

Glib::ustring PlaylistPane::search_text(const ListRow& row) const
	{
	// The album is not on the row, so a match there would look like none.
	return Glib::ustring(row.song.title + " " + row.song.artist).lowercase();
	}

void PlaylistPane::remove_from_list(const ListRow& row)
	{
	auto window = dynamic_cast<Gtk::Window*>(get_root());
	if(!window) return;
	std::string what = "“" + row.song.title + "”";
	if(!row.song.album.empty())
		what += " (from “" + row.song.album + "”)";
	auto dialog = Gtk::AlertDialog::create("Remove track?");
	dialog->set_detail("Remove " + what + " from this playlist?");
	dialog->set_buttons({"Cancel", "Remove"});
	dialog->set_cancel_button(0);
	dialog->set_default_button(1);
	int index = row.index;
	std::string playlist_id = playlist_.ref.id;
	unsigned gen = gen_;
	dialog->choose(*window, [this, dialog, index, playlist_id, gen](Glib::RefPtr<Gio::AsyncResult>& res) {
		int button = 0;
		try {
			button = dialog->choose_finish(res);
			}
		catch(const Glib::Error& e) {
			// Dismissed: the same as Cancel.
			std::cout << stamp() << "Remove track dialog: " << e.what() << std::endl;
			}
		if(button!=1 || gen!=gen_ || !api_) return;
		// By index rather than id: a playlist may hold the same track twice.
		api_->call("updatePlaylist", {{"playlistId", playlist_id},
		                              {"songIndexToRemove", std::to_string(index)}},
		           [this, gen](const ApiResult& r) {
			if(gen!=gen_) return;
			if(!r.ok) report(r);
			reload();
			});
		});
	}

// ---- RecentsPane --------------------------------------------------------

RecentsPane::RecentsPane(CoverStore& covers)
	: Pane(covers, 48, false)
	{
	row_badges_ = false;
	}

void RecentsPane::load(const std::vector<std::shared_ptr<ApiClient>>& apis)
	{
	clear();
	unsigned gen = gen_;
	set_title("Recents");
	call_each(apis, "getAlbumList2", {{"type", "newest"}, {"size", "12"}},
	          [this, apis, gen](const std::vector<ApiResult>& added) {
		if(gen!=gen_) return;
		call_each(apis, "getRecentSongs", {{"size", "50"}}, [this, gen, added](const std::vector<ApiResult>& played) {
			if(gen!=gen_) return;
			std::vector<Glib::RefPtr<ListRow>> rows;
			for(size_t k=0; k<added.size(); ++k) {
				const auto& r = added[k];
				const auto& p = played[k];
				if(!r.ok || !p.ok) {
					report(r.ok ? p : r);
					continue;
					}
				const auto& albums = jsub(jsub(r.body, "albumList2"), "album");
				if(!albums.empty())
					rows.push_back(ListRow::heading(heading_on("Recently added", r.server)));
				for(size_t i=0; i<albums.size(); ++i)
					rows.push_back(ListRow::of(parse_album(r.server, jidx(albums, i))));
				const auto& songs = jsub(jsub(p.body, "recentSongs"), "song");
				if(!songs.empty())
					rows.push_back(ListRow::heading(heading_on("Recently played", p.server)));
				for(size_t i=0; i<songs.size(); ++i)
					rows.push_back(ListRow::of(parse_song(p.server, jidx(songs, i))));
				}
			set_empty(rows.empty() ? "Nothing added or played yet." : "");
			set_rows(rows);
			});
		});
	}

void RecentsPane::bind(RowWidget& w, const ListRow& row)
	{
	if(row.kind==ListRow::Kind::heading)
		w.show_heading(row.text);
	else if(row.kind==ListRow::Kind::album)
		bind_album_hit(w, row.album);
	else
		bind_song_hit(w, row.song, format_ago(row.song.last_played));
	}

void RecentsPane::activated(const ListRow& row)
	{
	if(row.kind==ListRow::Kind::album)
		signal_open_album.emit(row.album.ref, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit({row.song.ref.server, row.song.album_id}, row.song.ref.id);
	}

// ---- SearchPane ---------------------------------------------------------

SearchPane::SearchPane(CoverStore& covers, const AppSettings& settings)
	: Pane(covers, 48, false), settings_(settings)
	{
	set_title("Search");
	entry_.set_placeholder_text("Artists, albums, tracks");
	entry_.set_margin_start(12);
	entry_.set_margin_end(12);
	// Clear of the header's rule, as far as it is in from the side.
	entry_.set_margin_top(12);
	entry_.set_margin_bottom(6);
	entry_.add_css_class("pane-search");
	// The web client's debounce.
	entry_.set_search_delay(320);
	entry_.signal_search_changed().connect(sigc::mem_fun(*this, &SearchPane::run));
	insert_child_after(entry_, header_);
	}

void SearchPane::set_scope(const std::vector<std::shared_ptr<ApiClient>>& apis)
	{
	apis_ = apis;
	run();
	}

void SearchPane::clear()
	{
	Pane::clear();
	set_title("Search");
	}

std::string SearchPane::query() const
	{
	return entry_.get_text();
	}

void SearchPane::set_query(const std::string& q)
	{
	entry_.set_text(q);
	}

void SearchPane::focus_entry()
	{
	entry_.grab_focus();
	}

void SearchPane::run()
	{
	clear();
	std::string q = entry_.get_text();
	if(q.empty() || apis_.empty()) return;
	unsigned gen = gen_;
	// Shown as each server answers, rather than when the slowest has.
	auto results = std::make_shared<std::vector<ApiResult>>();
	for(const auto& api: apis_)
		api->call("search3", {{"query", q}, {"artistCount", "20"}, {"albumCount", "20"},
		                      {"songCount", "20"}}, [this, results, gen](const ApiResult& r) {
			if(gen!=gen_) return;
			if(!r.ok) {
				report(r);
				return;
				}
			results->push_back(r);
			show_results(*results);
			});
	}

void SearchPane::show_results(const std::vector<ApiResult>& results)
	{
	// In the server list's order, whichever answered first.
	std::vector<const ApiResult*> ordered;
	for(const auto& api: apis_)
		for(const auto& r: results)
			if(r.server==api->session().server_key) ordered.push_back(&r);
	std::vector<std::vector<Artist>> artist_lists;
	std::vector<std::vector<Album>>  album_lists;
	std::vector<Song>                songs;
	for(const ApiResult *r: ordered) {
		const auto& res = jsub(r->body, "searchResult3");
		const auto& ar  = jsub(res, "artist");
		const auto& al  = jsub(res, "album");
		const auto& so  = jsub(res, "song");
		artist_lists.emplace_back();
		for(size_t i=0; i<ar.size(); ++i)
			artist_lists.back().push_back(parse_artist(r->server, jidx(ar, i), false));
		album_lists.emplace_back();
		for(size_t i=0; i<al.size(); ++i)
			album_lists.back().push_back(parse_album(r->server, jidx(al, i)));
		for(size_t i=0; i<so.size(); ++i)
			songs.push_back(parse_song(r->server, jidx(so, i)));
		}
	auto artists = merge_artists(artist_lists);
	std::vector<Album> albums;
	if(settings_.merge_albums)
		albums = merge_albums(album_lists);
	else
		for(const auto& l: album_lists)
			albums.insert(albums.end(), l.begin(), l.end());

	std::vector<Glib::RefPtr<ListRow>> rows;
	if(!artists.empty())
		rows.push_back(ListRow::heading("Artists"));
	for(const auto& a: artists)
		rows.push_back(ListRow::of(a));
	if(!albums.empty())
		rows.push_back(ListRow::heading("Albums"));
	for(const auto& a: albums)
		rows.push_back(ListRow::of(a));
	if(!songs.empty())
		rows.push_back(ListRow::heading("Songs"));
	for(const auto& s: songs)
		rows.push_back(ListRow::of(s));
	set_empty(rows.empty() ? "No results found." : "");
	set_rows(rows);
	}

void SearchPane::bind(RowWidget& w, const ListRow& row)
	{
	switch(row.kind) {
		case ListRow::Kind::heading:
			w.show_heading(row.text);
			break;
		case ListRow::Kind::artist:
			// The artist's portrait: getCoverArt takes the artist's own id.
			// Only when known: a server other than gaindrive may not send
			// albumCount with a search hit.
			w.show_item(row.artist.ref, "", row.artist.name, badges(row.artist.ref, row.artist.also),
			            row.artist.album_count>0 ? plural(row.artist.album_count, "album", "albums") : "");
			w.round_cover();
			break;
		case ListRow::Kind::album:
			bind_album_hit(w, row.album);
			break;
		default:
			bind_song_hit(w, row.song, format_duration(row.song.duration));
			break;
		}
	}

void SearchPane::activated(const ListRow& row)
	{
	if(row.kind==ListRow::Kind::artist)
		signal_open_artist.emit(row.artist);
	else if(row.kind==ListRow::Kind::album)
		signal_open_album.emit(row.album.ref, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit({row.song.ref.server, row.song.album_id}, row.song.ref.id);
	}

// ---- SettingsPane -------------------------------------------------------

SettingsPane::SettingsPane(CoverStore& covers, AppSettings& settings,
                           const std::vector<ServerConfig>& servers)
	: Pane(covers, 0, false), settings_(settings), servers_(servers)
	{
	}

void SettingsPane::load()
	{
	clear();
	set_title("Settings");
	set_rows({
		ListRow::setting("playback", "Playback", settings_.quality_label()),
		ListRow::setting("appearance", "Appearance", settings_.theme_label()),
		ListRow::setting("servers",  "Servers",
		                 servers_.empty()     ? "None"
		                 : servers_.size()==1 ? servers_.front().url
		                 : std::to_string(servers_.size()) + " servers"),
		ListRow::setting("about",    "About",    "GainDrive " GAINDRIVE_VERSION)
		});
	}

void SettingsPane::bind(RowWidget& w, const ListRow& row)
	{
	w.show_item({}, "", row.text, row.detail, "");
	}

void SettingsPane::activated(const ListRow& row)
	{
	signal_category.emit(row.key);
	}

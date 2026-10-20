#include "panes.hh"
#include "apiclient.hh"
#include "jsonread.hh"
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
	}

void PlaylistsPane::load(std::shared_ptr<ApiClient> api)
	{
	clear();
	api_ = api;
	unsigned gen = gen_;
	set_title("Playlists");
	api->call("getPlaylists", {}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& server = api->session().server_key;
		std::vector<Glib::RefPtr<ListRow>> rows;
		const auto& lists = jsub(jsub(r.body, "playlists"), "playlist");
		for(size_t i=0; i<lists.size(); ++i)
			rows.push_back(ListRow::of(parse_playlist(server, jidx(lists, i))));

		api->call("getStarred2", {}, [this, api, gen, rows](const ApiResult& r) mutable {
			if(gen!=gen_) return;
			// The playlists stand on their own if this fails.
			const auto& server  = api->session().server_key;
			const auto& starred = jsub(r.body, "starred2");
			const auto& albums  = jsub(starred, "album");
			const auto& songs   = jsub(starred, "song");
			if(!albums.empty())
				rows.push_back(ListRow::heading("Starred albums"));
			for(size_t i=0; i<albums.size(); ++i)
				rows.push_back(ListRow::of(parse_album(server, jidx(albums, i))));
			if(!songs.empty())
				rows.push_back(ListRow::heading("Starred tracks"));
			for(size_t i=0; i<songs.size(); ++i)
				rows.push_back(ListRow::of(parse_song(server, jidx(songs, i))));
			set_empty(rows.empty() ? "No playlists or starred items yet." : "");
			set_rows(rows);
			});
		});
	}

void PlaylistsPane::star_changed(ListRow::Kind, const ItemRef&, bool)
	{
	// The starred sections are this pane's content, not a mark on it.
	if(api_) load(api_);
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
			w.show_item("", "", p.name, meta, "");
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
		signal_open_album.emit(row.album.ref.id, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit(row.song.album_id, row.song.ref.id);
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
	w.show_item("", row.text, s.title, s.artist, format_duration(s.duration));
	w.show_actions(true, s.starred,
	               owner_ ? RowWidget::ListButton::remove : RowWidget::ListButton::add, true);
	}

void PlaylistPane::activated(const ListRow& row)
	{
	if(row.index>=0 && size_t(row.index)<songs_.size())
		signal_play.emit(songs_, size_t(row.index));
	unselect();
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
	}

void RecentsPane::load(std::shared_ptr<ApiClient> api)
	{
	clear();
	unsigned gen = gen_;
	set_title("Recents");
	api->call("getAlbumList2", {{"type", "newest"}, {"size", "12"}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& server = api->session().server_key;
		std::vector<Glib::RefPtr<ListRow>> rows;
		const auto& albums = jsub(jsub(r.body, "albumList2"), "album");
		if(!albums.empty())
			rows.push_back(ListRow::heading("Recently added"));
		for(size_t i=0; i<albums.size(); ++i)
			rows.push_back(ListRow::of(parse_album(server, jidx(albums, i))));

		api->call("getRecentSongs", {{"size", "50"}}, [this, api, gen, rows](const ApiResult& r) mutable {
			if(gen!=gen_) return;
			if(!r.ok) {
				report(r);
				return;
				}
			const auto& songs = jsub(jsub(r.body, "recentSongs"), "song");
			if(!songs.empty())
				rows.push_back(ListRow::heading("Recently played"));
			for(size_t i=0; i<songs.size(); ++i)
				rows.push_back(ListRow::of(parse_song(api->session().server_key, jidx(songs, i))));
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
		signal_open_album.emit(row.album.ref.id, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit(row.song.album_id, row.song.ref.id);
	}

// ---- SearchPane ---------------------------------------------------------

SearchPane::SearchPane(CoverStore& covers)
	: Pane(covers, 48, false)
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

void SearchPane::set_api(std::shared_ptr<ApiClient> api)
	{
	api_ = std::move(api);
	entry_.set_text("");
	clear();
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
	if(q.empty() || !api_) return;
	unsigned gen = gen_;
	auto api = api_;
	api->call("search3", {{"query", q}, {"artistCount", "20"}, {"albumCount", "20"},
	                      {"songCount", "20"}}, [this, api, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			report(r);
			return;
			}
		const auto& server = api->session().server_key;
		const auto& res    = jsub(r.body, "searchResult3");
		const auto& artists = jsub(res, "artist");
		const auto& albums  = jsub(res, "album");
		const auto& songs   = jsub(res, "song");
		std::vector<Glib::RefPtr<ListRow>> rows;
		if(!artists.empty())
			rows.push_back(ListRow::heading("Artists"));
		for(size_t i=0; i<artists.size(); ++i)
			rows.push_back(ListRow::of(parse_artist(server, jidx(artists, i), false)));
		if(!albums.empty())
			rows.push_back(ListRow::heading("Albums"));
		for(size_t i=0; i<albums.size(); ++i)
			rows.push_back(ListRow::of(parse_album(server, jidx(albums, i))));
		if(!songs.empty())
			rows.push_back(ListRow::heading("Songs"));
		for(size_t i=0; i<songs.size(); ++i)
			rows.push_back(ListRow::of(parse_song(server, jidx(songs, i))));
		set_empty(rows.empty() ? "No results found." : "");
		set_rows(rows);
		});
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
			w.show_item(row.artist.ref.id, "", row.artist.name, "",
			            row.artist.album_count>0 ? plural(row.artist.album_count, "album", "albums") : "");
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
		signal_open_album.emit(row.album.ref.id, "");
	else if(row.kind==ListRow::Kind::song)
		signal_open_album.emit(row.song.album_id, row.song.ref.id);
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
		ListRow::setting("servers",  "Servers",
		                 servers_.empty() ? "None" : servers_.front().url),
		ListRow::setting("about",    "About",    "GainDrive " GAINDRIVE_VERSION)
		});
	}

void SettingsPane::bind(RowWidget& w, const ListRow& row)
	{
	w.show_item("", "", row.text, row.detail, "");
	}

void SettingsPane::activated(const ListRow& row)
	{
	signal_category.emit(row.key);
	}

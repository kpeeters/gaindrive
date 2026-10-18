#include "mainwindow.hh"
#include "apiclient.hh"
#include "configfile.hh"
#include "jsonread.hh"
#include "playlistpicker.hh"
#include "secretstore.hh"
#include "stamp.hh"
#include "uithread.hh"

#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iostream>

namespace {
	const char *css = R"(
		/* The web client's light-theme accent. */
		@define-color gd_red #a31623;
		.pane { border-left: 1px solid alpha(currentColor, 0.15); }
		.pane-header { padding: 10px 12px; min-height: 34px; }
		.pane-title { font-weight: bold; font-size: 1.2em; }
		.row-heading { font-weight: bold; padding: 12px 4px 4px 4px; opacity: 0.6; }
		/* In the panes, flush with the row text below and in full colour. */
		.pane .row-heading { padding-left: 0; opacity: 1; color: @gd_red; }
		.pane listview > row { padding: 6px 10px; }
		.hero { margin: 12px; }
		.cover { border-radius: 4px; }
		.notice { padding: 6px 12px; background-color: alpha(@error_color, 0.2); }
		.sidebar { min-width: 255px; }
		.playing .row-title { font-weight: bold; }
		.player-bar { padding: 8px 12px; border-top: 1px solid alpha(currentColor, 0.15); }
		.pane-empty { padding: 12px; }
		.info-block { margin: 0 12px 12px 12px; }
		.liner-notes { margin: 12px; }
		.row-action { min-width: 24px; min-height: 24px; padding: 2px; }
		.row-action.starred { color: @accent_color; }
		/* Material Symbols leave a wide margin inside the em, so at the
		   default 16px they look much smaller than theme icons. */
		.pane button > image, .sidebar button > image { -gtk-icon-size: 20px; }
		/* Direct children only: the cast popover hangs off the bar too. */
		.player-bar > button > image,
		.player-bar > menubutton > button image { -gtk-icon-size: 48px; }
		.player-bar .casting image { color: @gd_red; }
		.nav image {
			-gtk-icon-size: 24px;
			color: @gd_red;
		}
	)";

	// Credentials the server refuses: wrong user or password.
	const int auth_refused = 40;

	// The server's own rule for what gets a resume point.
	const int resume_min_seconds = 20*60;

	struct Section { const char *name, *label; };
	const Section sections[] = {
		{ "library",   "Library" },
		{ "playlists", "Playlists" },
		{ "recents",   "Recents" },
		{ "search",    "Search" },
		{ "settings",  "Settings" }
	};
}

MainWindow::MainWindow()
	: pool_(4),
	  covers_(pool_),
	  playback_(settings_, pool_),
	  browse_(Gtk::Orientation::VERTICAL),
	  main_(Gtk::Orientation::HORIZONTAL),
	  sidebar_(Gtk::Orientation::VERTICAL),
	  queue_(playback_),
	  panes_(Gtk::Orientation::HORIZONTAL),
	  artists_(covers_),
	  playlists_(covers_),
	  recents_(covers_),
	  search_(covers_),
	  settings_pane_(covers_, settings_, servers_),
	  albums_(covers_, settings_),
	  playlist_(covers_),
	  settings_page_(settings_, servers_),
	  tracks_(covers_),
	  bar_(covers_, playback_)
	{
	ui_init();
	settings_.load();

	auto provider = Gtk::CssProvider::create();
	provider->load_from_data(css);
	Gtk::StyleProvider::add_provider_for_display(get_display(), provider,
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);

	set_title("GainDrive");
	set_icon_name(GD_APP_ID);
	restore_size();
	signal_close_request().connect([this]() {
		playback_.report_stopped_now();
		save_size();
		return false;
		}, false);
	set_titlebar(header_);

	// Notices about failed requests, above everything else.
	notice_text_.set_wrap(true);
	notice_text_.set_xalign(0);
	notice_text_.add_css_class("notice");
	notice_.set_child(notice_text_);
	browse_.append(notice_);

	for(const auto& sec: sections) {
		auto box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 10);
		auto icon  = Gtk::make_managed<Gtk::Image>();
		icon->set_from_icon_name(std::string("gd-")+sec.name+"-symbolic");
		auto label = Gtk::make_managed<Gtk::Label>(sec.label);
		label->set_xalign(0);
		box->append(*icon);
		box->append(*label);
		nav_.append(*box);
		}
	nav_.add_css_class("navigation-sidebar");
	nav_.add_css_class("nav");
	sidebar_.add_css_class("sidebar");
	sidebar_.append(nav_);
	queue_scroll_.set_child(queue_);
	queue_scroll_.set_vexpand(true);
	queue_scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
	sidebar_.append(queue_scroll_);
	main_.append(sidebar_);

	left_.add(artists_, "library");
	left_.add(playlists_, "playlists");
	left_.add(recents_, "recents");
	left_.add(search_, "search");
	left_.add(settings_pane_, "settings");
	middle_.add(albums_, "albums");
	middle_.add(playlist_, "playlist");
	middle_.add(settings_page_, "settings");
	panes_.set_homogeneous(true);
	panes_.set_hexpand(true);
	panes_.append(left_);
	panes_.append(middle_);
	panes_.append(tracks_);
	main_.append(panes_);
	main_.set_vexpand(true);
	browse_.append(main_);
	browse_.append(bar_);

	stack_.add(login_, "login");
	stack_.add(browse_, "browse");
	set_child(stack_);

	login_.signal_login.connect(sigc::mem_fun(*this, &MainWindow::login));
	nav_.signal_row_selected().connect([this](Gtk::ListBoxRow *row) {
		if(row && api_) show_section(sections[row->get_index()].name);
		});
	auto search = Gio::SimpleAction::create("search");
	search->signal_activate().connect([this](const Glib::VariantBase&) {
		if(api_) nav_.select_row(*nav_.get_row_at_index(3));
		});
	add_action(search);

	for(auto p: all_panes()) {
		p->signal_failed.connect(sigc::mem_fun(*this, &MainWindow::failed));
		p->signal_play.connect(sigc::mem_fun(*this, &MainWindow::play));
		p->signal_enqueue.connect(sigc::mem_fun(playback_, &Playback::enqueue));
		p->signal_star.connect(sigc::mem_fun(*this, &MainWindow::star));
		p->signal_open_album.connect(sigc::mem_fun(*this, &MainWindow::open_album));
		p->signal_open_artist.connect(sigc::mem_fun(*this, &MainWindow::open_artist));
		p->signal_pick_playlist.connect([this](const Song& s, Gtk::Widget& anchor) {
			pick_playlist(anchor, api_, s, [this](const ApiResult& r) { failed(r); });
			});
		}
	albums_.signal_album.connect([this](const Album& a) {
		open_album(a.ref.id, "");
		});
	tracks_.signal_loaded.connect(sigc::mem_fun(*this, &MainWindow::album_loaded));
	playlists_.signal_playlist.connect([this](const Playlist& pl) {
		middle_.set_visible_child("playlist");
		tracks_.clear();
		playlist_.show_playlist(api_, pl);
		});
	settings_pane_.signal_category.connect([this](const std::string& key) {
		middle_.set_visible_child("settings");
		settings_page_.show(key, api_);
		});
	settings_page_.signal_changed.connect([this]() {
		settings_pane_.load();
		});
	settings_page_.signal_remove_server.connect(sigc::mem_fun(*this, &MainWindow::remove_server));
	bar_.signal_cover.connect([this]() {
		if(auto s = playback_.current())
			open_album(s->album_id, "");
		});
	playback_.signal_track.connect([this]() {
		const Song *s = playback_.current();
		for(auto p: all_panes())
			p->set_playing(s ? s->ref : ItemRef{});
		});
	playback_.signal_error.connect([this](const std::string& e) {
		notify("Cannot play this track: " + e);
		});

	servers_ = load_servers();
	if(!servers_.empty()) {
		const auto& s = servers_.front();
		if(auto tok = secret_load(s)) {
			start_session(Session{s.key, s.url, s.user, tok->salt, tok->token});
			return;
			}
		}
	show_login("");
	}

MainWindow::~MainWindow()
	{
	// Before any member goes: a reply arriving during teardown must not run.
	ui_close();
	notice_timer_.disconnect();
	}

void MainWindow::restore_size()
	{
	auto j = read_config("state.json");
	int w = jint(j, "width"), h = jint(j, "height");
	set_default_size(w>0 ? w : 1200, h>0 ? h : 760);
	if(jsub(j, "maximized").is_boolean() && jsub(j, "maximized").get<bool>())
		maximize();
	}

void MainWindow::save_size()
	{
	// GTK 4 keeps the default size current as the window is resized, and
	// leaves it alone while maximised, so this is the size to come back to.
	int w=0, h=0;
	get_default_size(w, h);
	write_config("state.json", {{"width", w}, {"height", h}, {"maximized", is_maximized()}});
	}

void MainWindow::show_login(const std::string& error)
	{
	if(servers_.empty()) login_.prefill("", "");
	else                 login_.prefill(servers_.front().url, servers_.front().user);
	login_.set_busy(false);
	login_.set_error(error);
	stack_.set_visible_child("login");
	}

void MainWindow::login(const std::string& url, const std::string& user, const std::string& password)
	{
	login_.set_busy(true);
	ApiClient::login(pool_, url, user, password, [this](const ApiResult& r, const Session& s) {
		logged_in(r, s);
		});
	}

void MainWindow::logged_in(const ApiResult& r, const Session& s)
	{
	login_.set_busy(false);
	if(!r.ok) {
		login_.set_error(r.error);
		return;
		}
	// The same server and account keep their key, and with it their cache.
	ServerConfig cfg{"", s.base_url, s.user};
	for(const auto& old: servers_)
		if(old.url==cfg.url && old.user==cfg.user)
			cfg.key = old.key;
	if(cfg.key.empty())
		cfg.key = random_hex(8);
	// One server for now: the one just logged in to.
	servers_ = {cfg};
	save_servers(servers_);
	bool stored = secret_save(cfg, StoredToken{s.salt, s.token});

	Session session = s;
	session.server_key = cfg.key;
	start_session(session);
	if(!stored)
		notify("The keyring did not store this login; you will be asked again next time.");
	}

void MainWindow::start_session(const Session& s)
	{
	api_ = std::make_shared<ApiClient>(pool_, s);
	covers_.set_api(api_);
	playback_.set_api(api_);
	search_.set_api(api_);
	for(auto p: all_panes())
		p->clear();
	artists_.load(api_);
	// Selecting the row when it already is selected does nothing, so the
	// section is shown by hand as well.
	nav_.select_row(*nav_.get_row_at_index(0));
	show_section("library");
	stack_.set_visible_child("browse");
	}

void MainWindow::end_session(const std::string& error)
	{
	playback_.set_api(nullptr);
	api_.reset();
	covers_.set_api(nullptr);
	search_.set_api(nullptr);
	for(auto p: all_panes())
		p->clear();
	settings_page_.clear();
	show_login(error);
	}

void MainWindow::remove_server(const std::string& key)
	{
	auto it = std::find_if(servers_.begin(), servers_.end(),
	                       [&](const ServerConfig& s) { return s.key==key; });
	if(it==servers_.end()) return;
	auto dialog = Gtk::AlertDialog::create("Remove server?");
	dialog->set_detail("Forget the login for " + it->user + " on " + it->url + "?");
	dialog->set_buttons({"Cancel", "Remove"});
	dialog->set_cancel_button(0);
	dialog->set_default_button(1);
	dialog->choose(*this, [this, dialog, key](Glib::RefPtr<Gio::AsyncResult>& res) {
		int button = 0;
		try {
			button = dialog->choose_finish(res);
			}
		catch(const Glib::Error& e) {
			// Dismissed: the same as Cancel.
			std::cout << stamp() << "Remove server dialog: " << e.what() << std::endl;
			}
		if(button!=1) return;
		auto it = std::find_if(servers_.begin(), servers_.end(),
		                       [&](const ServerConfig& s) { return s.key==key; });
		if(it==servers_.end()) return;
		secret_clear(*it);
		servers_.erase(it);
		save_servers(servers_);
		if(api_ && api_->session().server_key==key)
			end_session("");
		else {
			settings_pane_.load();
			settings_page_.show("servers", api_);
			}
		});
	}

std::vector<Pane*> MainWindow::all_panes()
	{
	return { &artists_, &playlists_, &recents_, &search_, &settings_pane_,
	         &albums_, &playlist_, &tracks_ };
	}

void MainWindow::show_section(const std::string& name)
	{
	left_.set_visible_child(name);
	// As on the web client, a section starts with the other two panes empty.
	middle_.set_visible_child("albums");
	albums_.clear();
	playlist_.clear();
	settings_page_.clear();
	tracks_.clear();
	for(Pane *p: {static_cast<Pane*>(&artists_), static_cast<Pane*>(&playlists_),
	              static_cast<Pane*>(&recents_), static_cast<Pane*>(&search_),
	              static_cast<Pane*>(&settings_pane_)})
		p->unselect();

	if(name=="library") {
		if(!artists_.loaded()) artists_.load(api_);
		}
	else if(name=="playlists") playlists_.load(api_);
	else if(name=="recents")   recents_.load(api_);
	else if(name=="search")    search_.focus_entry();
	else if(name=="settings")  settings_pane_.load();
	}

void MainWindow::open_artist(const Artist& a)
	{
	middle_.set_visible_child("albums");
	tracks_.clear();
	albums_.show_artist(api_, a);
	}

void MainWindow::open_album(const std::string& album_id, const std::string& play_id)
	{
	if(album_id.empty() || !api_) return;
	tracks_.show_album(api_, album_id, play_id);
	}

void MainWindow::album_loaded(const Album& a)
	{
	// An album opened from a search, a recent play or the player bar brings
	// its artist's albums along, so the middle pane is not left empty or
	// showing something unrelated.
	if(a.artist_id.empty()) return;
	if(middle_.get_visible_child_name()=="albums" && albums_.artist_id()==a.artist_id) return;
	Artist artist;
	artist.ref  = {a.ref.server, a.artist_id};
	artist.name = a.artist;
	middle_.set_visible_child("albums");
	albums_.show_artist(api_, artist);
	}

void MainWindow::play(const std::vector<Song>& songs, size_t index)
	{
	if(index>=songs.size() || !api_) return;
	const Song& song = songs[index];
	if(song.duration<resume_min_seconds) {
		playback_.play(songs, index);
		return;
		}
	api_->call("getBookmarks", {}, [this, songs, index](const ApiResult& r) {
		const Song& song = songs[index];
		double pos = 0;
		const auto& marks = jsub(jsub(r.body, "bookmarks"), "bookmark");
		for(size_t i=0; i<marks.size(); ++i) {
			const auto& b = jidx(marks, i);
			if(jstr(jsub(b, "entry"), "id")==song.ref.id)
				pos = jnum(b, "position")/1000.0;
			}
		if(pos<=0) {
			playback_.play(songs, index);
			return;
			}
		auto dialog = Gtk::AlertDialog::create("Resume");
		dialog->set_detail("Continue \u201c" + song.title + "\u201d where you left off?");
		dialog->set_buttons({"Start over", "Resume at " + format_duration(int(pos))});
		dialog->set_default_button(1);
		dialog->choose(*this, [this, dialog, songs, index, pos](Glib::RefPtr<Gio::AsyncResult>& res) {
			try {
				int button = dialog->choose_finish(res);
				playback_.play(songs, index, button==1 ? pos : 0);
				}
			catch(const Glib::Error&) {
				// Dismissed: play nothing, as on the web client.
				}
			});
		});
	}

void MainWindow::star(ListRow::Kind kind, const ItemRef& ref, bool on)
	{
	if(!api_) return;
	Params p = {{kind==ListRow::Kind::album ? "albumId" : "id", ref.id}};
	api_->call(on ? "star" : "unstar", p, [this, kind, ref, on](const ApiResult& r) {
		if(!r.ok) {
			failed(r);
			return;
			}
		for(auto pane: all_panes())
			pane->star_changed(kind, ref, on);
		});
	}

void MainWindow::failed(const ApiResult& r)
	{
	if(r.code==auth_refused) {
		if(!servers_.empty())
			secret_clear(servers_.front());
		end_session("The server no longer accepts this login. Please log in again.");
		return;
		}
	notify(r.error);
	}

void MainWindow::notify(const std::string& text)
	{
	notice_text_.set_text(text);
	notice_.set_reveal_child(true);
	notice_timer_.disconnect();
	notice_timer_ = Glib::signal_timeout().connect_seconds([this]() {
		notice_.set_reveal_child(false);
		return false;
		}, 6);
	}

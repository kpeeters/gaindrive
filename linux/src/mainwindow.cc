#include "mainwindow.hh"
#include "apiclient.hh"
#include "configfile.hh"
#include "jsonread.hh"
#include "playlistpicker.hh"
#include "mpris.hh"
#include "trackinfo.hh"
#include "secretstore.hh"
#include "stamp.hh"
#include "tunebutton.hh"
#include "uithread.hh"

#include <giomm/simpleaction.h>
#include <glibmm/main.h>
#include <gtkmm/alertdialog.h>
#include <gtkmm/cssprovider.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/image.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <iterator>
#include <iostream>

namespace {
	const char *css = R"(
		/* The web client's light-theme accent. */
		@define-color gd_red #a31623;
		/* The content on the view colour, a shade off the sidebar's and the
		   bar's, as on macOS; the lists take it from the pane, so a header,
		   hero or notes above them sit on the same surface. */
		.pane {
			border-left: 1px solid alpha(currentColor, 0.15);
			background-color: @theme_base_color;
		}
		.pane listview { background-color: transparent; }
		.pane-header {
			padding: 16px 12px 12px 12px; min-height: 40px;
			border-bottom: 1px solid alpha(currentColor, 0.12);
		}
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
		/* Selected and focused too, so the keyboard reaches them. */
		.hover-actions .row-action { opacity: 0; transition: opacity 100ms; }
		row:hover .hover-actions .row-action,
		row:selected .hover-actions .row-action,
		row:focus-within .hover-actions .row-action,
		.hover-actions .row-action.starred { opacity: 1; }
		/* Material Symbols leave a wide margin inside the em, so at the
		   default 16px they look much smaller than theme icons. */
		.pane button > image, .sidebar button > image { -gtk-icon-size: 20px; }
		/* Direct children only: the cast popover hangs off the bar too. */
		.player-row > button > image,
		.player-row > menubutton > button image { -gtk-icon-size: 48px; }
		.player-row > .bar-extra image { -gtk-icon-size: 32px; }
		.player-bar .casting image { color: @gd_red; }
		.info-card .card-title { font-weight: bold; font-size: 1.15em; }
		.info-card .card-section {
			font-size: 0.75em; font-weight: bold; letter-spacing: 1px;
			opacity: 0.55; margin-top: 10px;
		}
		.back-link { color: @gd_red; padding: 2px 6px; min-height: 0; font-size: 0.9em; }
		.bottom-nav { border-top: 1px solid alpha(currentColor, 0.15); padding: 2px 4px; }
		.bottom-nav button { padding: 4px 0; }
		.bottom-nav image { -gtk-icon-size: 24px; color: @gd_red; }
		.bottom-nav label { font-size: 0.75em; }
		.player-bar .eq-on image { color: @gd_red; }
		.tune-panel, .info-card {
			padding: 10px 12px;
			background-color: @theme_bg_color;
			border: 1px solid alpha(currentColor, 0.15);
			border-radius: 6px;
			box-shadow: 0 4px 12px rgba(0, 0, 0, 0.35);
		}
		.info-card { padding: 14px 16px; }
		.player-bar scale highlight,
		.player-bar scale slider,
		.tune-panel scale highlight,
		.tune-panel scale slider { background: @gd_red; }
		.nav image {
			-gtk-icon-size: 24px;
			color: @gd_red;
		}
		.video { background-color: black; }
		.video .caption {
			color: white;
			font-size: 1.6em;
			background-color: alpha(black, 0.6);
			padding: 4px 12px;
			border-radius: 6px;
		}
		.video-controls {
			padding: 10px 16px;
			background-image: linear-gradient(to bottom, transparent, alpha(black, 0.7));
		}
		.video > revealer button, .video-controls button,
		.video-controls .chapter-now { color: white; }
		.pane .chapter { margin-left: 28px; }
		.video > revealer button image { -gtk-icon-size: 32px; }
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

	// Below this window width, the web client's phone layout: no sidebar,
	// the sections in a bar along the bottom.
	const int narrow_window = 650;

	AppSettings loaded_settings()
		{
		AppSettings s;
		s.load();
		return s;
		}
}

MainWindow::MainWindow()
	: pool_(4),
	  // Before anything built below reads it, the equaliser among them.
	  settings_(loaded_settings()),
	  playback_(settings_, pool_),
	  browse_(Gtk::Orientation::VERTICAL, narrow_window),
	  main_(Gtk::Orientation::HORIZONTAL),
	  sidebar_(Gtk::Orientation::VERTICAL),
	  queue_(playback_),
	  video_(playback_),
	  artists_(covers_),
	  playlists_(covers_),
	  recents_(covers_),
	  search_(covers_),
	  settings_pane_(covers_, settings_, servers_),
	  albums_(covers_, settings_),
	  playlist_(covers_),
	  settings_page_(settings_, servers_),
	  tracks_(covers_),
	  bar_(covers_, playback_, settings_),
	  info_card_(covers_, playback_, pool_),
	  tabs_(Gtk::Orientation::HORIZONTAL)
	{
	ui_init();

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
		// Again, for search text typed since the last navigation.
		save_view();
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
	strip_.add(left_);
	strip_.add(middle_);
	strip_.add(tracks_);
	strip_.signal_layout.connect(sigc::mem_fun(*this, &MainWindow::update_back));
	content_.add(strip_, "panes");
	content_.add(video_, "video");
	content_.set_hexpand(true);
	main_.append(content_);
	main_.set_vexpand(true);
	body_.set_child(main_);
	// Bottom right, just above the button that opens it, as on the web.
	auto& tune = bar_.tune_panel();
	tune.set_margin_end(16);
	tune.set_margin_bottom(6);
	body_.add_overlay(tune);
	info_card_.set_margin_end(16);
	info_card_.set_margin_bottom(6);
	body_.add_overlay(info_card_);
	body_.set_vexpand(true);
	browse_.append(body_);
	browse_.append(bar_);
	build_tabs();
	browse_.append(tabs_);
	browse_.signal_narrow.connect(sigc::mem_fun(*this, &MainWindow::set_narrow));
	install_tune_closers();

	// Back a pane, as a browser's back does on the web client: Alt+Left,
	// the keyboard's Back key, or a mouse's back button.
	auto back_keys = Gtk::EventControllerKey::create();
	back_keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
		bool alt_left = keyval==GDK_KEY_Left && (state & Gdk::ModifierType::ALT_MASK)==Gdk::ModifierType::ALT_MASK;
		if(!alt_left && keyval!=GDK_KEY_Back) return false;
		back();
		return true;
		}, false);
	browse_.add_controller(back_keys);
	auto back_button = Gtk::GestureClick::create();
	back_button->set_button(8);
	back_button->signal_pressed().connect([this](int, double, double) { back(); });
	browse_.add_controller(back_button);
	mpris_ = std::make_unique<Mpris>(playback_, covers_, [this]() { present(); });

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
	// No resume prompt: the chapter is where to start.
	tracks_.signal_play_chapter.connect([this](const std::vector<Song>& songs, size_t index, double start) {
		const Song *now = playback_.current();
		if(now && now->ref.id==songs[index].ref.id && now->ref.server==songs[index].ref.server)
			playback_.seek(start);
		else
			playback_.play(songs, index, start);
		});
	playlists_.signal_playlist.connect([this](const Playlist& pl) {
		middle_.set_visible_child("playlist");
		tracks_.clear();
		view_.playlist_id   = pl.ref.id;
		view_.playlist_name = pl.name;
		view_.album_id.clear();
		view_.album_title.clear();
		playlist_.show_playlist(api_, pl);
		go(1);
		});
	settings_pane_.signal_category.connect([this](const std::string& key) {
		middle_.set_visible_child("settings");
		settings_page_.show(key, api_);
		view_.settings = key;
		go(1);
		});
	for(auto p: all_panes())
		p->signal_back.connect(sigc::mem_fun(*this, &MainWindow::back));
	settings_page_.signal_back.connect(sigc::mem_fun(*this, &MainWindow::back));
	settings_page_.signal_changed.connect([this]() {
		settings_pane_.load();
		});
	settings_page_.signal_remove_server.connect(sigc::mem_fun(*this, &MainWindow::remove_server));
	// The info and equaliser cards share the corner: opening one closes the
	// other.
	bar_.info_button().signal_toggled().connect([this]() {
		bool on = bar_.info_button().get_active();
		if(on) {
			bar_.tune_button().set_active(false);
			info_card_.refresh();
			}
		info_card_.set_visible(on);
		});
	bar_.tune_button().signal_toggled().connect([this]() {
		if(bar_.tune_button().get_active()) bar_.info_button().set_active(false);
		});
	info_card_.signal_close.connect([this]() { bar_.info_button().set_active(false); });
	bar_.signal_cover.connect([this]() {
		if(auto s = playback_.current())
			open_album(s->album_id, "");
		});
	playback_.signal_track.connect([this]() {
		const Song *s = playback_.current();
		for(auto p: all_panes())
			p->set_playing(s ? s->ref : ItemRef{});
		// An open info card follows the track, not every queue change.
		if(info_card_.get_visible() && s && s->ref.id!=info_card_.shown())
			info_card_.refresh();
		});
	playback_.signal_started.connect([this]() {
		const Song *s = playback_.current();
		// While casting too: the controls, chapters and subtitles stay here,
		// and a soundtrack-only receiver leaves the subtitles to us.
		if(s && s->is_video) {
			video_.show_film(*s, api_);
			content_.set_visible_child("video");
			video_.grab_focus();
			}
		else if(content_.get_visible_child_name()=="video")
			close_video();
		});
	video_.signal_chapter.connect([this](int index) {
		const Song *s = playback_.current();
		tracks_.set_chapter(s ? s->ref : ItemRef{}, index);
		});
	video_.signal_close.connect([this]() {
		playback_.stop();
		close_video();
		});
	video_.signal_fullscreen.connect([this]() {
		if(is_fullscreen()) unfullscreen();
		else                fullscreen();
		});
	property_fullscreened().signal_changed().connect(sigc::mem_fun(*this, &MainWindow::fullscreen_changed));
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
	info_card_.set_api(api_);
	for(auto p: all_panes())
		p->clear();
	artists_.load(api_);
	restore_view();
	stack_.set_visible_child("browse");
	}

void MainWindow::save_view()
	{
	if(!api_) return;
	// Not while the last view is still reopening: what is open then is only
	// part of it, and a server that never answers would lose the rest.
	if(restoring_view_ || !expect_album_.empty()) return;
	for(auto p: all_panes())
		if(p->restoring()) return;
	nlohmann::json j = {{"server", api_->session().server_key}, {"section", view_.section}};
	if(!view_.artist_id.empty())   j["artist"]   = {{"id", view_.artist_id},   {"name", view_.artist_name}};
	if(!view_.playlist_id.empty()) j["playlist"] = {{"id", view_.playlist_id}, {"name", view_.playlist_name}};
	if(!view_.settings.empty())    j["settings"] = view_.settings;
	// The title only once it is known, so an album that never loaded is
	// not reopened.
	if(!view_.album_id.empty() && !view_.album_title.empty())
		j["album"] = {{"id", view_.album_id}, {"title", view_.album_title}};
	if(view_.section=="search" && !search_.query().empty())
		j["query"] = search_.query();
	write_config("view.json", j);
	}

void MainWindow::restore_view()
	{
	auto j = read_config("view.json");
	restoring_view_ = true;
	struct Done {
		bool& flag;
		~Done() { flag = false; }
	} done{restoring_view_};
	// Another server's ids mean nothing here.
	bool ours = jstr(j, "server")==api_->session().server_key;
	std::string section = jstr(j, "section");
	size_t index = 0;
	if(ours)
		for(size_t i=0; i<std::size(sections); ++i)
			if(section==sections[i].name) index = i;
	section = sections[index].name;
	// Selecting the row when it already is selected does nothing, so the
	// section is shown by hand as well.
	nav_.select_row(*nav_.get_row_at_index(int(index)));
	show_section(section);
	if(!ours) return;

	const auto& artist   = jsub(j, "artist");
	const auto& album    = jsub(j, "album");
	const auto& playlist = jsub(j, "playlist");
	std::string album_id = jstr(album, "id"), album_title = jstr(album, "title");
	auto album_match = [album_id, album_title](const ListRow& r) {
		return r.kind==ListRow::Kind::album && r.album.ref.id==album_id && r.album.title==album_title;
		};
	// Each level by reselecting its row once its list is there, so the
	// usual click path opens and slides; a level that is gone, or now
	// names something else, ends it there.
	if(section=="library" && !jstr(artist, "id").empty()) {
		std::string id = jstr(artist, "id"), name = jstr(artist, "name");
		artists_.restore([id, name](const ListRow& r) {
			return r.kind==ListRow::Kind::artist && r.artist.ref.id==id && r.artist.name==name;
			}, [this, album_id, album_match](bool found) {
			if(found && !album_id.empty()) albums_.restore(album_match);
			});
		}
	else if(section=="playlists" && !jstr(playlist, "id").empty()) {
		std::string id = jstr(playlist, "id"), name = jstr(playlist, "name");
		playlists_.restore([id, name](const ListRow& r) {
			return r.kind==ListRow::Kind::playlist && r.playlist.ref.id==id && r.playlist.name==name;
			});
		}
	else if(section=="settings" && !jstr(j, "settings").empty()) {
		std::string key = jstr(j, "settings");
		settings_pane_.restore([key](const ListRow& r) {
			return r.kind==ListRow::Kind::setting && r.key==key;
			});
		}
	else if(section=="search" || section=="recents") {
		if(section=="search") search_.set_query(jstr(j, "query"));
		// Not by reselecting a hit: selecting a song plays it. By id, and
		// album_loaded checks the title.
		if(!album_id.empty() && !album_title.empty()) {
			open_album(album_id, "");
			expect_album_ = album_title;
			}
		}
	}

void MainWindow::end_session(const std::string& error)
	{
	close_video();
	playback_.set_api(nullptr);
	api_.reset();
	covers_.set_api(nullptr);
	search_.set_api(nullptr);
	close_cards();
	info_card_.set_api(nullptr);
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
	// Whatever was still reopening from the last run gives way.
	for(auto p: all_panes()) p->cancel_restore();
	expect_album_.clear();
	view_ = View{};
	view_.section = name;
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
	go(0);
	}

void MainWindow::go(int depth)
	{
	strip_.slide_to(depth);
	update_back();
	save_view();
	}

void MainWindow::back()
	{
	if(strip_.leftmost()>0) go(strip_.depth()-1);
	}

void MainWindow::update_back()
	{
	// Only the leftmost pane shown carries one, and only when the strip has
	// moved off its first pane, as on the web client.
	for(auto p: all_panes()) p->set_back("");
	settings_page_.set_back("");
	int left = strip_.leftmost();
	if(left==1) {
		auto middle = middle_.get_visible_child_name();
		auto section = left_.get_visible_child_name();
		std::string label = "Artists";
		for(const auto& s: sections)
			if(section==s.name && section!="library") label = s.label;
		if(middle=="albums")        albums_.set_back(label);
		else if(middle=="playlist") playlist_.set_back("Playlists");
		else                        settings_page_.set_back("Settings");
		}
	else if(left==2)
		tracks_.set_back(tracks_back_.empty() ? "Albums" : tracks_back_);
	}

void MainWindow::build_tabs()
	{
	// The sidebar's sections as the web client's bottom bar: an icon over a
	// small label. Kept in step with nav_, which stays the one way in.
	tabs_.add_css_class("bottom-nav");
	tabs_.set_homogeneous(true);
	for(size_t i=0; i<std::size(sections); ++i) {
		auto button = Gtk::make_managed<Gtk::ToggleButton>();
		auto box    = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
		auto icon   = Gtk::make_managed<Gtk::Image>();
		icon->set_from_icon_name(std::string("gd-")+sections[i].name+"-symbolic");
		auto label  = Gtk::make_managed<Gtk::Label>(sections[i].label);
		box->append(*icon);
		box->append(*label);
		button->set_child(*box);
		button->add_css_class("flat");
		if(!tab_buttons_.empty()) button->set_group(*tab_buttons_.front());
		button->signal_toggled().connect([this, i, button]() {
			if(button->get_active())
				if(auto row = nav_.get_row_at_index(int(i))) nav_.select_row(*row);
			});
		tabs_.append(*button);
		tab_buttons_.push_back(button);
		}
	nav_.signal_row_selected().connect([this](Gtk::ListBoxRow *row) {
		if(row && size_t(row->get_index())<tab_buttons_.size())
			tab_buttons_[size_t(row->get_index())]->set_active(true);
		});
	tabs_.set_visible(false);
	}

void MainWindow::set_narrow(bool narrow)
	{
	narrow_ = narrow;
	bar_.set_narrow(narrow);
	// Across the window, as the web pins both edges of its panel.
	auto& tune = bar_.tune_panel();
	tune.set_narrow(narrow);
	tune.set_halign(narrow ? Gtk::Align::FILL : Gtk::Align::END);
	tune.set_margin_start(narrow ? 8 : 0);
	tune.set_margin_end(narrow ? 8 : 16);
	info_card_.set_narrow(narrow);
	info_card_.set_halign(narrow ? Gtk::Align::FILL : Gtk::Align::END);
	info_card_.set_margin_start(narrow ? 8 : 0);
	info_card_.set_margin_end(narrow ? 8 : 16);
	fullscreen_changed();
	}

void MainWindow::open_artist(const Artist& a)
	{
	middle_.set_visible_child("albums");
	tracks_.clear();
	view_.artist_id   = a.ref.id;
	view_.artist_name = a.name;
	view_.playlist_id.clear();
	view_.album_id.clear();
	view_.album_title.clear();
	albums_.show_artist(api_, a);
	go(1);
	}

void MainWindow::open_album(const std::string& album_id, const std::string& play_id)
	{
	if(album_id.empty() || !api_) return;
	tracks_back_.clear();
	expect_album_.clear();
	view_.album_id = album_id;
	view_.album_title.clear();
	tracks_.show_album(api_, album_id, play_id);
	go(2);
	}

void MainWindow::album_loaded(const Album& a)
	{
	// An album opened from a search, a recent play or the player bar brings
	// its artist's albums along, so the middle pane is not left empty or
	// showing something unrelated.
	if(!expect_album_.empty()) {
		bool same = a.title==expect_album_;
		expect_album_.clear();
		// The id now names another album: the library changed since.
		if(!same) {
			tracks_.clear();
			view_.album_id.clear();
			go(0);
			return;
			}
		}
	view_.album_title = a.title;
	save_view();
	tracks_back_ = a.artist;
	update_back();
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
	// Every film, as on the web client; music only when it is long.
	if(song.duration<resume_min_seconds && !song.is_video) {
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
			catch(const Glib::Error& e) {
				// Dismissed: play nothing, as on the web client.
				std::cout << stamp() << "Resume dialog: " << e.what() << std::endl;
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

void MainWindow::close_cards()
	{
	bar_.tune_button().set_active(false);
	bar_.info_button().set_active(false);
	}

void MainWindow::install_tune_closers()
	{
	// The card closes as the web client's panel does: its button, Escape, or
	// a click anywhere else. Not claimed, so that click still does its job.
	auto close = [this]() { close_cards(); };
	auto outside = Gtk::GestureClick::create();
	outside->set_button(0);
	outside->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	outside->signal_pressed().connect([close](int, double, double) { close(); });
	main_.add_controller(outside);
	auto in_bar = Gtk::GestureClick::create();
	in_bar->set_button(0);
	in_bar->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	in_bar->signal_pressed().connect([this, close](int, double x, double y) {
		// The buttons toggle the cards themselves.
		auto *w = bar_.pick(x, y);
		for(Gtk::Widget *button: {static_cast<Gtk::Widget*>(&bar_.tune_button()),
		                          static_cast<Gtk::Widget*>(&bar_.info_button())})
			if(w && (w==button || w->is_ancestor(*button))) return;
		close();
		});
	bar_.add_controller(in_bar);
	// Bubbling, so a list the card's preset menu has open takes its Escape
	// first.
	auto keys = Gtk::EventControllerKey::create();
	keys->signal_key_pressed().connect([this, close](guint keyval, guint, Gdk::ModifierType) {
		if(keyval!=GDK_KEY_Escape) return false;
		if(!bar_.tune_button().get_active() && !bar_.info_button().get_active()) return false;
		close();
		return true;
		}, false);
	add_controller(keys);
	}

void MainWindow::close_video()
	{
	if(is_fullscreen()) unfullscreen();
	content_.set_visible_child("panes");
	tracks_.set_chapter({}, -1);
	}

void MainWindow::fullscreen_changed()
	{
	// Only a film takes the whole screen; anything else fullscreened by the
	// window manager keeps its sidebar and player bar.
	bool film = is_fullscreen() && content_.get_visible_child_name()=="video";
	sidebar_.set_visible(!film && !narrow_);
	tabs_.set_visible(!film && narrow_);
	bar_.set_visible(!film);
	notice_.set_visible(!film);
	video_.set_fullscreen(film);
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

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
#include <gtkmm/settings.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
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
		.pane listview > row { padding: 0; }
		.pane-row { padding: 6px 10px; }
		.pane-row.extra { padding: 0; }
		/* Room around the text, which the theme sets tight to the icon. */
		.pane-search { min-height: 36px; padding: 0 10px; }
		.pane-search > image:first-child { margin-right: 8px; }
		/* As far down as the bio is in from the left. */
		.pane-row.extra > .info-block { margin-top: 12px; }
		.hero { margin: 12px; }
		.cover { border-radius: 4px; }
		/* Artists as discs, as on the web; GTK shrinks the radius to fit. */
		.cover.round { border-radius: 9999px; }
		.notice { padding: 6px 12px; background-color: alpha(@error_color, 0.2); }
		.sidebar { min-width: 255px; }
		.playing .row-title { font-weight: bold; }
		.playing-mark { opacity: 0; color: @gd_red; -gtk-icon-size: 20px; }
		/* Another user's track, as the web marks it. */
		.peer .row-title { color: @gd_red; }
		.playing .playing-mark { opacity: 1; }
		.player-bar { padding: 8px 12px; border-top: 1px solid alpha(currentColor, 0.15); }
		.pane-empty { padding: 12px; }
		.isearch {
			padding: 6px 18px; font-size: 0.9em;
			border-top: 1px solid alpha(currentColor, 0.12);
			background-color: @theme_base_color;
		}
		.isearch-label { opacity: 0.6; }
		/* "Failing" is emacs' word; the colour says it to everyone else. */
		.isearch.failing .isearch-label, .isearch.failing .isearch-text {
			color: @gd_red; opacity: 1;
		}
		/* Settings as GNOME's preference groups: bordered, rounded lists
		   with a small title above and hints below. On the sidebar's grey,
		   so they stand off the white pane as GNOME's cards do off its. */
		.settings-content { margin: 12px 18px 18px 18px; }
		.settings-group-title { font-weight: bold; margin: 18px 4px 6px 4px; }
		.settings-content > .settings-group-title:first-child { margin-top: 0; }
		.settings-group {
			border: 1px solid alpha(currentColor, 0.15);
			border-radius: 8px;
			background-color: @theme_bg_color;
		}
		.settings-group > row { padding: 10px 12px; min-height: 32px; }
		.settings-group > row:first-child { border-top-left-radius: 8px; border-top-right-radius: 8px; }
		.settings-group > row:last-child { border-bottom-left-radius: 8px; border-bottom-right-radius: 8px; }
		.settings-group > row:not(:selected):hover { background-color: transparent; }
		.settings-group > row:activatable:hover { background-color: alpha(currentColor, 0.05); }
		.settings-subtitle { font-size: 0.9em; }
		.pane .remove-server > image { -gtk-icon-size: 30px; }
		.video-mark { -gtk-icon-size: 16px; }
		/* GainDrive red where the theme would put its blue accent: an on
		   switch, the default button, a focused field. */
		/* Image and animation too: WhiteSur paints the track with its blue
		   gradient and animates into it, and an animation overrides every
		   stylesheet while it runs. */
		switch:checked {
			background-color: @gd_red;
			background-image: none;
			border-color: @gd_red;
			animation: none;
		}
		button.suggested-action { background-color: @gd_red; color: white; }
		button.suggested-action:hover { background-color: shade(@gd_red, 1.1); }
		button.suggested-action:active { background-color: shade(@gd_red, 0.9); }
		/* WhiteSur rings a focused field with an inset shadow, others with
		   an outline. */
		entry:focus-within {
			outline-color: alpha(@gd_red, 0.5);
			box-shadow: inset 0 0 0 2px alpha(@gd_red, 0.5);
		}
		/* Selected text in fields, text views and selectable labels. */
		text > selection, textview > text > selection, label > selection {
			background-color: @gd_red;
			color: white;
		}
		/* The drop-down's own button is checked while its list is open,
		   which WhiteSur paints as a pressed button, in blue. */
		dropdown > button:checked, dropdown > button:active {
			background-color: @gd_red;
			border-color: @gd_red;
			color: white;
		}
		/* The picked entry in a drop-down's list. */
		popover > contents listview > row:selected {
			background-color: @gd_red;
			color: white;
		}
		.settings-hint { font-size: 0.9em; margin: 6px 4px 0 4px; }
		/* The upload form's cards hold fields rather than rows. */
		.upload-card { padding: 12px; }
		.upload-note { color: @gd_red; }
		.upload-card progressbar > trough > progress {
			background-color: @gd_red;
			background-image: none;
			border-color: @gd_red;
		}
		/* The fetch mode in red, not the theme's blue; WhiteSur paints a
		   checked button with a gradient and light text. */
		.upload-mode > button:checked {
			background-color: alpha(@gd_red, 0.15);
			background-image: none;
			color: @gd_red;
			box-shadow: none;
		}
		/* The server's name is the row's title, edited in place: no field
		   until it is pointed at or typed in. */
		.name-entry {
			font-weight: bold;
			background-color: transparent;
			border-color: transparent;
			box-shadow: none;
			outline-color: transparent;
			min-height: 0;
			padding: 2px 4px;
			margin-left: -4px;
		}
		.name-entry:hover { border-color: alpha(currentColor, 0.2); }
		.name-entry:focus-within {
			box-shadow: none;
			background-color: @theme_base_color;
			border-color: @gd_red;
		}
		.info-block { margin: 0 12px 12px 12px; }
		/* The external links as tinted pills, as iOS's small bordered
		   buttons: the theme's link button pads plain text with the
		   background, which reads as a gap rather than a button. */
		.info-links { margin-top: 4px; }
		.info-links button.link,
		.info-links button.link:visited {
			color: @gd_red;
			background-color: alpha(@gd_red, 0.12);
			border: none;
			box-shadow: none;
			border-radius: 999px;
			padding: 2px 10px;
			min-height: 0;
			font-size: 0.85em;
		}
		.info-links button.link:hover { background-color: alpha(@gd_red, 0.2); }
		.info-links button.link label { text-decoration: none; }
		.liner-notes { margin: 12px; }
		.track-edit { margin-bottom: 12px; }
		.track-edit .edit-all { padding: 0 6px; min-height: 0; }
		.row-action { min-width: 24px; min-height: 24px; padding: 2px; }
		.row-action.starred { color: @gd_red; }
		/* Selected and focused too, so the keyboard reaches them. */
		.hover-actions .row-action { opacity: 0; transition: opacity 100ms; }
		row:hover .hover-actions .row-action,
		row:selected .hover-actions .row-action,
		row:focus-within .hover-actions .row-action,
		.hover-actions .row-action.starred { opacity: 1; }
		/* Material Symbols leave a wide margin inside the em, so at the
		   default 16px they look much smaller than theme icons. */
		.pane button > image, .sidebar button > image { -gtk-icon-size: 20px; }
		/* Filled, so the full foreground reads as black beside the
		   outlined icons; dimmed as Material's top bar dims its actions. */
		.pane .scope-button image { -gtk-icon-size: 40px; opacity: 0.65; }
		/* Half as large again as other pane buttons, as the bar's extras are:
		   the outlined arrow is thin at 20px. The margin pads it out to the
		   scope button's 40px, so both buttons are the same size. */
		.pane .uploads-button > image { -gtk-icon-size: 30px; margin: 5px; }
		/* The album header's star, sized as the bar's other extras. */
		.pane .album-star > image { -gtk-icon-size: 30px; }
		/* Direct children only: the cast popover hangs off the bar too. */
		.player-row > button > image,
		.player-row > menubutton > button image { -gtk-icon-size: 48px; }
		.player-row > .bar-extra image { -gtk-icon-size: 32px; }
		/* A checked (open or on) button keeps the flat look: the theme's
		   dark fill would hide a red icon. Red says what is on. The theme's
		   light text for that fill goes too, or a plain icon vanishes.
		   The same for the server picker, so open buttons look alike. */
		.player-row > .bar-extra:checked,
		.player-row > .bar-extra > button:checked,
		.pane .scope-button > button:checked {
			background-color: transparent;
			color: @theme_fg_color;
		}
		.player-row > .bar-extra:checked:hover,
		.player-row > .bar-extra > button:checked:hover,
		.pane .scope-button > button:checked:hover { background-color: alpha(currentColor, 0.07); }
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
		.player-bar .eq-on image, .player-bar .audio-only image { color: @gd_red; }
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
			/* The subtitles' own size follows the picture; see VideoView. */
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
		{ "uploads",   "Uploads" },
		{ "playlists", "Playlists" },
		{ "recents",   "Recents" },
		{ "search",    "Search" },
		{ "settings",  "Settings" }
	};

	int section_index(const std::string& name)
		{
		for(size_t i=0; i<std::size(sections); ++i)
			if(name==sections[i].name) return int(i);
		return 0;
		}

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
	  covers_(clients_),
	  playback_(settings_, pool_, clients_),
	  browse_(Gtk::Orientation::VERTICAL, narrow_window),
	  main_(Gtk::Orientation::HORIZONTAL),
	  sidebar_(Gtk::Orientation::VERTICAL),
	  queue_(playback_),
	  video_(playback_, settings_),
	  artists_(covers_),
	  uploads_(covers_, settings_),
	  playlists_(covers_),
	  recents_(covers_),
	  search_(covers_, settings_),
	  settings_pane_(covers_, settings_, servers_),
	  albums_(covers_, settings_),
	  playlist_(covers_),
	  settings_page_(settings_, servers_, clients_),
	  tracks_(covers_),
	  bar_(covers_, playback_, settings_),
	  info_card_(covers_, playback_, pool_, clients_),
	  tabs_(Gtk::Orientation::HORIZONTAL)
	{
	ui_init();

	auto provider = Gtk::CssProvider::create();
	provider->load_from_data(css);
	Gtk::StyleProvider::add_provider_for_display(get_display(), provider,
		GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
	gtk_dark_ = Gtk::Settings::get_default()->property_gtk_application_prefer_dark_theme();
	dark_css_ = Gtk::CssProvider::create();
	dark_css_->load_from_data("@define-color gd_red #c9202f;");
	apply_theme();
	desktop_dpi_ = Gtk::Settings::get_default()->property_gtk_xft_dpi();
	std::cout << stamp() << "Desktop text dpi: " << desktop_dpi_/1024.0 << std::endl;
	if(desktop_dpi_<=0) desktop_dpi_ = 96*1024;
	apply_text_scale();

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
	// Below the queue, which takes the height between, as on the web.
	users_.signal_open_album.connect([this](const ItemRef& album, const std::string& song_id) {
		open_album(album, "", song_id);
		});
	sidebar_.append(users_);
	main_.append(sidebar_);

	left_.add(artists_, "library");
	left_.add(uploads_, "uploads");
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
	content_.set_vexpand(true);
	content_box_.append(content_);
	content_box_.append(isearch_);
	main_.append(content_box_);
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
	set_upload_server(nullptr, false);
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
	// The web client's view keys: Shift and the initial, and / for search.
	// Bubbling, so a text field takes them as typing first; and not over a
	// film, which they would leave playing out of sight.
	auto view_keys = Gtk::EventControllerKey::create();
	view_keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
		using M = Gdk::ModifierType;
		if(clients_.empty() || content_.get_visible_child_name()=="video") return false;
		if((state & (M::CONTROL_MASK | M::ALT_MASK | M::SUPER_MASK))!=M(0)) return false;
		bool shift = (state & M::SHIFT_MASK)==M::SHIFT_MASK;
		std::string name;
		switch(gdk_keyval_to_lower(keyval)) {
			case GDK_KEY_l:     if(shift) name = "library"; break;
			case GDK_KEY_p:     if(shift) name = "playlists"; break;
			case GDK_KEY_r:     if(shift) name = "recents"; break;
			case GDK_KEY_s:     if(shift) name = "settings"; break;
			case GDK_KEY_slash: name = "search"; break;
			}
		if(name.empty()) return false;
		select_section(name);
		if(name=="search") search_.focus_entry();
		return true;
		}, false);
	browse_.add_controller(view_keys);
	// Text size, as in a browser: Ctrl+plus, Ctrl+minus, Ctrl+0. On the
	// window, so it works on the login page and over a film too.
	auto zoom_keys = Gtk::EventControllerKey::create();
	zoom_keys->signal_key_pressed().connect([this](guint keyval, guint, Gdk::ModifierType state) {
		using M = Gdk::ModifierType;
		if((state & (M::CONTROL_MASK | M::ALT_MASK | M::SUPER_MASK))!=M::CONTROL_MASK) return false;
		double scale = settings_.text_scale;
		switch(keyval) {
			case GDK_KEY_plus: case GDK_KEY_equal: case GDK_KEY_KP_Add:
				scale += text_scale_step; break;
			case GDK_KEY_minus: case GDK_KEY_KP_Subtract:
				scale -= text_scale_step; break;
			case GDK_KEY_0: case GDK_KEY_KP_0:
				scale = 1.0; break;
			default:
				return false;
			}
		// Rounded, so steps land back on 1.0 exactly.
		settings_.text_scale = std::clamp(std::round(scale*100)/100, min_text_scale, max_text_scale);
		settings_.save();
		apply_text_scale();
		return true;
		}, false);
	add_controller(zoom_keys);
	// The web's incremental search, Ctrl+S and Ctrl+R. Capturing, so a
	// running search sees its keys before the focused list does; a text
	// field keeps them, for its own editing.
	auto isearch_keys = Gtk::EventControllerKey::create();
	isearch_keys->signal_key_pressed().connect([this, ctl = isearch_keys.get()](guint keyval, guint, Gdk::ModifierType state) {
		using M = Gdk::ModifierType;
		auto focus = get_focus();
		if(focus && GTK_IS_EDITABLE(focus->gobj())) {
			isearch_.end(false);
			return false;
			}
		// Shift on its way to a capital is not the end of the search.
		auto ev = ctl->get_current_event();
		if(ev && ev->is_modifier()) return false;
		if(isearch_.on()) return isearch_.key(keyval, state);
		if(clients_.empty() || content_.get_visible_child_name()=="video") return false;
		if((state & (M::CONTROL_MASK | M::SHIFT_MASK | M::ALT_MASK | M::SUPER_MASK))!=M::CONTROL_MASK)
			return false;
		auto lower = gdk_keyval_to_lower(keyval);
		if(lower!=GDK_KEY_s && lower!=GDK_KEY_r) return false;
		auto pane = active_pane();
		if(!pane) return false;
		isearch_.start(*pane, lower==GDK_KEY_r);
		return true;
		}, false);
	isearch_keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	browse_.add_controller(isearch_keys);
	// Reaching for the mouse says the search is over.
	auto isearch_click = Gtk::GestureClick::create();
	isearch_click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	isearch_click->signal_pressed().connect([this](int, double, double) { isearch_.end(false); });
	strip_.add_controller(isearch_click);
	auto back_button = Gtk::GestureClick::create();
	back_button->set_button(8);
	back_button->signal_pressed().connect([this](int, double, double) { back(); });
	browse_.add_controller(back_button);
	mpris_ = std::make_unique<Mpris>(playback_, covers_, [this]() { present(); });

	stack_.add(login_, "login");
	stack_.add(browse_, "browse");
	set_child(stack_);

	login_.signal_login.connect(sigc::mem_fun(*this, &MainWindow::login));
	login_.signal_cancel.connect([this]() { stack_.set_visible_child("browse"); });
	nav_.signal_row_selected().connect([this](Gtk::ListBoxRow *row) {
		if(row && !clients_.empty()) show_section(sections[row->get_index()].name);
		});
	auto search = Gio::SimpleAction::create("search");
	search->signal_activate().connect([this](const Glib::VariantBase&) {
		if(!clients_.empty()) select_section("search");
		});
	add_action(search);

	for(auto p: all_panes()) {
		p->signal_failed.connect(sigc::mem_fun(*this, &MainWindow::failed));
		p->signal_play.connect(sigc::mem_fun(*this, &MainWindow::play));
		p->signal_enqueue.connect(sigc::mem_fun(playback_, &Playback::enqueue));
		p->signal_star.connect(sigc::mem_fun(*this, &MainWindow::star));
		p->signal_open_album.connect([this](const ItemRef& album, const std::string& play_id) {
			open_album(album, play_id);
			});
		p->signal_open_artist.connect(sigc::mem_fun(*this, &MainWindow::open_artist));
		p->signal_pick_playlist.connect([this](const Song& s, Gtk::Widget& anchor) {
			pick_playlist(anchor, client_for(s.ref.server), s, [this](const ApiResult& r) { failed(r); });
			});
		}
	albums_.signal_album.connect([this](const Album& a) {
		open_album(a.ref, "");
		});
	albums_.signal_delete_upload.connect([this](const Album& a, const std::string&) {
		delete_upload(a);
		});
	albums_.signal_promote.connect(sigc::mem_fun(*this, &MainWindow::promote_upload));
	artists_.signal_uploads.connect([this]() { select_section("uploads"); });
	tracks_.signal_loaded.connect(sigc::mem_fun(*this, &MainWindow::album_loaded));
	// An edit can rename the album, and its artist's directory with it, or
	// change the year its row shows; the listings it came from follow.
	tracks_.signal_edited.connect([this](const ItemRef& artist, const ItemRef& album) {
		if(view_.section=="library" || view_.section=="uploads") {
			reveal_album(artist, album);
			return;
			}
		// Opened from a list of another kind, which stays as it is; the
		// artist's albums are drawn afresh with the album.
		albums_.clear();
		open_album(album, "");
		});
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
		auto api = client_for(pl.ref.server);
		if(!api) return;
		view_.playlist      = pl.ref;
		view_.playlist_name = pl.name;
		view_.album = {};
		view_.album_title.clear();
		playlist_.show_playlist(api, pl);
		go(1);
		});
	settings_pane_.signal_category.connect([this](const std::string& key) {
		middle_.set_visible_child("settings");
		settings_page_.show(key);
		view_.settings = key;
		go(1);
		});
	for(auto p: all_panes()) {
		p->signal_back.connect(sigc::mem_fun(*this, &MainWindow::back));
		p->set_badge([this](const std::string& key) {
			return scope().size()>1 ? server_label(key) : std::string();
			});
		}
	artists_.signal_scope.connect(sigc::mem_fun(*this, &MainWindow::set_scope));
	settings_page_.signal_merge.connect(sigc::mem_fun(*this, &MainWindow::reload_lists));
	settings_page_.signal_back.connect(sigc::mem_fun(*this, &MainWindow::back));
	settings_page_.signal_changed.connect([this]() {
		settings_pane_.load();
		});
	settings_page_.signal_theme.connect(sigc::mem_fun(*this, &MainWindow::apply_theme));
	settings_page_.signal_remove_server.connect(sigc::mem_fun(*this, &MainWindow::remove_server));
	settings_page_.signal_login_server.connect(sigc::mem_fun(*this, &MainWindow::relogin));
	settings_page_.signal_add_server.connect(sigc::mem_fun(*this, &MainWindow::add_server));
	settings_page_.signal_rename_server.connect(sigc::mem_fun(*this, &MainWindow::rename_server));
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
			open_album({s->ref.server, s->album_id}, "");
		});
	playback_.signal_track.connect([this]() {
		const Song *s = playback_.current();
		for(auto p: all_panes())
			p->set_playing(s ? s->ref : ItemRef{});
		// An open info card follows the track, not every queue change.
		if(info_card_.get_visible() && s && s->ref.server + "|" + s->ref.id!=info_card_.shown())
			info_card_.refresh();
		});
	playback_.signal_started.connect([this]() {
		const Song *s = playback_.current();
		// While casting too: the controls, chapters and subtitles stay here,
		// and a soundtrack-only receiver leaves the subtitles to us. Not
		// for a video played as audio only, which has nothing to show.
		if(s && playback_.shows_picture(*s)) {
			video_.show_film(*s, client_for(s->ref.server));
			isearch_.end(false);
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
	for(const auto& s: servers_)
		if(auto tok = secret_load(s))
			clients_.set(std::make_shared<ApiClient>(pool_, Session{s.key, s.url, s.user, tok->salt, tok->token}),
			             clients_.all().size());
	if(clients_.empty()) show_login("");
	else                 open_browse();
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
	login_.set_cancellable(false);
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
	ServerConfig cfg{"", s.base_url, s.user, ""};
	size_t pos = servers_.size();
	for(size_t i=0; i<servers_.size(); ++i)
		if(servers_[i].url==cfg.url && servers_[i].user==cfg.user) {
			cfg.key = servers_[i].key;
			pos     = i;
			}
	if(cfg.key.empty()) {
		cfg.key = random_hex(8);
		servers_.push_back(cfg);
		}
	save_servers(servers_);
	bool stored = secret_save(cfg, StoredToken{s.salt, s.token});

	Session session = s;
	session.server_key = cfg.key;
	// In servers.json's order, among those with a session.
	size_t at = 0;
	for(size_t i=0; i<pos; ++i)
		if(clients_.get(servers_[i].key)) ++at;
	bool first = clients_.empty();
	clients_.set(std::make_shared<ApiClient>(pool_, session), at);
	if(first) open_browse();
	else {
		stack_.set_visible_child("browse");
		servers_changed();
		reload_lists();
		}
	if(!stored)
		notify("The keyring did not store this login; you will be asked again next time.");
	}

void MainWindow::open_browse()
	{
	for(auto p: all_panes())
		p->clear();
	artists_.set_servers(server_choices(), settings_.scope);
	search_.set_scope(scope());
	artists_.load(scope());
	find_upload_server();
	poll_other_users();
	restore_view();
	stack_.set_visible_child("browse");
	}

void MainWindow::reload_lists()
	{
	// What is open in the other panes stays: it is on a server still there.
	artists_.set_servers(server_choices(), settings_.scope);
	search_.set_scope(scope());
	artists_.load(scope());
	find_upload_server();
	poll_other_users();
	auto section = left_.get_visible_child_name();
	if(section=="playlists")    playlists_.load(scope());
	else if(section=="recents") recents_.load(scope());
	}

void MainWindow::poll_other_users()
	{
	auto apis = clients_.all();
	std::vector<std::pair<std::shared_ptr<ApiClient>, std::string>> servers;
	for(const auto& api: apis)
		servers.push_back({api, apis.size()>1 ? server_label(api->session().server_key) : std::string()});
	users_.set_servers(std::move(servers));
	}

void MainWindow::set_scope(const std::string& key)
	{
	settings_.scope = key;
	settings_.save();
	reload_lists();
	}

std::shared_ptr<ApiClient> MainWindow::client_for(const std::string& key) const
	{
	return clients_.get(key);
	}

std::vector<std::shared_ptr<ApiClient>> MainWindow::scope() const
	{
	// A server since removed or logged out: all of them, as on the phones.
	if(auto api = clients_.get(settings_.scope)) return {api};
	return clients_.all();
	}

std::string MainWindow::server_label(const std::string& key) const
	{
	for(const auto& s: servers_) {
		if(s.key!=key) continue;
		if(!s.name.empty()) return s.name;
		std::string h = server_host(s.url);
		for(const auto& o: servers_)
			if(o.key!=key && server_host(o.url)==h) return s.user + "@" + h;
		return h;
		}
	return "";
	}

void MainWindow::rename_server(const std::string& key, const std::string& name)
	{
	for(auto& s: servers_)
		if(s.key==key) s.name = name;
	save_servers(servers_);
	// Not the servers page itself, whose entry this came from.
	settings_pane_.load();
	reload_lists();
	}

std::vector<std::pair<std::string, std::string>> MainWindow::server_choices() const
	{
	std::vector<std::pair<std::string, std::string>> out;
	for(const auto& s: servers_)
		if(clients_.get(s.key)) out.push_back({s.key, server_label(s.key)});
	return out;
	}

void MainWindow::save_view()
	{
	if(clients_.empty()) return;
	// Not while the last view is still reopening: what is open then is only
	// part of it, and a server that never answers would lose the rest.
	if(restoring_view_ || !expect_album_.empty()) return;
	for(auto p: all_panes())
		if(p->restoring()) return;
	nlohmann::json j = {{"section", view_.section}};
	auto item = [](const ItemRef& ref, const char *label, const std::string& name) {
		return nlohmann::json{{"server", ref.server}, {"id", ref.id}, {label, name}};
		};
	if(!view_.artist.id.empty())   j["artist"]   = item(view_.artist, "name", view_.artist_name);
	if(!view_.playlist.id.empty()) j["playlist"] = item(view_.playlist, "name", view_.playlist_name);
	if(!view_.settings.empty())    j["settings"] = view_.settings;
	// The title only once it is known, so an album that never loaded is
	// not reopened.
	if(!view_.album.id.empty() && !view_.album_title.empty())
		j["album"] = item(view_.album, "title", view_.album_title);
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
	std::string section = jstr(j, "section");
	size_t index = 0;
	for(size_t i=0; i<std::size(sections); ++i)
		if(section==sections[i].name) index = i;
	section = sections[index].name;
	// Selecting the row when it already is selected does nothing, so the
	// section is shown by hand as well.
	nav_.select_row(*nav_.get_row_at_index(int(index)));
	show_section(section);

	// Each item with its server: an id means nothing on another one.
	auto ref = [](const nlohmann::json& o) { return ItemRef{jstr(o, "server"), jstr(o, "id")}; };
	auto same = [](const ItemRef& a, const ItemRef& b) { return a.server==b.server && a.id==b.id; };
	const auto& artist   = jsub(j, "artist");
	const auto& album    = jsub(j, "album");
	const auto& playlist = jsub(j, "playlist");
	ItemRef album_ref = ref(album);
	std::string album_title = jstr(album, "title");
	auto album_match = [album_ref, album_title, same](const ListRow& r) {
		return r.kind==ListRow::Kind::album && same(r.album.ref, album_ref) && r.album.title==album_title;
		};
	// Each level by reselecting its row once its list is there, so the
	// usual click path opens and slides; a level that is gone, or now
	// names something else, ends it there.
	if(section=="library" && !jstr(artist, "id").empty()) {
		ItemRef id = ref(artist);
		std::string name = jstr(artist, "name");
		artists_.restore([id, name, same](const ListRow& r) {
			return r.kind==ListRow::Kind::artist && same(r.artist.ref, id) && r.artist.name==name;
			}, [this, album_ref, album_match](bool found) {
			if(found && !album_ref.id.empty()) albums_.restore(album_match);
			});
		}
	else if(section=="playlists" && !jstr(playlist, "id").empty()) {
		ItemRef id = ref(playlist);
		std::string name = jstr(playlist, "name");
		playlists_.restore([id, name, same](const ListRow& r) {
			return r.kind==ListRow::Kind::playlist && same(r.playlist.ref, id) && r.playlist.name==name;
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
		if(!album_ref.id.empty() && !album_title.empty() && client_for(album_ref.server)) {
			open_album(album_ref, "");
			expect_album_ = album_title;
			}
		}
	}

void MainWindow::end_session(const std::string& error)
	{
	close_video();
	auto clients = clients_.all();
	for(const auto& c: clients)
		playback_.forget_server(c->session().server_key);
	for(const auto& c: clients)
		clients_.drop(c->session().server_key);
	search_.set_query("");
	search_.set_scope({});
	set_upload_server(nullptr, false);
	users_.set_servers({});
	close_cards();
	for(auto p: all_panes())
		p->clear();
	settings_page_.clear();
	show_login(error);
	}

void MainWindow::add_server()
	{
	login_.prefill("", "");
	login_.set_cancellable(true);
	login_.set_busy(false);
	login_.set_error("");
	stack_.set_visible_child("login");
	}

void MainWindow::relogin(const std::string& key)
	{
	for(const auto& s: servers_)
		if(s.key==key) {
			login_.prefill(s.url, s.user);
			login_.set_cancellable(!clients_.empty());
			login_.set_busy(false);
			login_.set_error("");
			stack_.set_visible_child("login");
			}
	}

void MainWindow::drop_client(const std::string& key, const std::string& message)
	{
	if(!clients_.get(key)) return;
	const Song *s = playback_.current();
	if(s && s->ref.server==key) close_video();
	// Before the client goes: the queue still says "stopped" with it.
	playback_.forget_server(key);
	clients_.drop(key);
	if(clients_.empty()) {
		end_session(message);
		return;
		}
	if(!message.empty()) notify(message);
	servers_changed();
	reload_lists();
	}

void MainWindow::servers_changed()
	{
	settings_pane_.load();
	if(middle_.get_visible_child_name()=="settings" && view_.settings=="servers")
		settings_page_.show("servers");
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
		if(clients_.get(key)) drop_client(key, "");
		else                  servers_changed();
		});
	}

std::vector<Pane*> MainWindow::all_panes()
	{
	return { &artists_, &uploads_, &playlists_, &recents_, &search_, &settings_pane_,
	         &albums_, &playlist_, &tracks_ };
	}

Pane *MainWindow::active_pane()
	{
	for(auto w = get_focus(); w; w = w->get_parent())
		if(auto p = dynamic_cast<Pane*>(w)) return p;
	Gtk::Stack *stacks[] = { &left_, &middle_ };
	int d = strip_.depth();
	if(d==2) return &tracks_;
	if(d<0 || d>1) return nullptr;
	return dynamic_cast<Pane*>(stacks[d]->get_visible_child());
	}

void MainWindow::show_section(const std::string& name)
	{
	isearch_.end(false);
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
	for(Pane *p: {static_cast<Pane*>(&artists_), static_cast<Pane*>(&uploads_),
	              static_cast<Pane*>(&playlists_),
	              static_cast<Pane*>(&recents_), static_cast<Pane*>(&search_),
	              static_cast<Pane*>(&settings_pane_)})
		p->unselect();

	if(name=="uploads") uploads_.load();
	else                uploads_.leave();
	if(name=="library") {
		if(!artists_.loaded()) artists_.load(scope());
		}
	else if(name=="playlists") playlists_.load(scope());
	else if(name=="recents")   recents_.load(scope());
	else if(name=="search")    search_.focus_entry();
	else if(name=="settings")  settings_pane_.load();
	go(0);
	}

void MainWindow::select_section(const std::string& name)
	{
	if(auto row = nav_.get_row_at_index(section_index(name))) nav_.select_row(*row);
	}

void MainWindow::find_upload_server()
	{
	// Only gaindrive takes uploads this way, and "type" in every answer
	// says which server this is; uploadRole alone is standard Subsonic.
	unsigned gen = ++upload_probe_;
	auto apis = clients_.all();
	ServerCalls calls;
	for(const auto& api: apis)
		calls.push_back({api, {{"username", api->session().user}}});
	call_each(calls, "getUser", [this, gen, apis](const std::vector<ApiResult>& rs) {
		if(gen!=upload_probe_) return;
		auto role = [](const nlohmann::json& u, const char *key) {
			const auto& v = jsub(u, key);
			return v.is_boolean() && v.get<bool>();
			};
		for(size_t i=0; i<rs.size(); ++i) {
			const auto& u = jsub(rs[i].body, "user");
			bool admin = role(u, "adminRole");
			if(rs[i].ok && jstr(rs[i].body, "type")=="gaindrive" && (admin || role(u, "uploadRole"))) {
				set_upload_server(apis[i], admin);
				return;
				}
			}
		set_upload_server(nullptr, false);
		});
	}

void MainWindow::set_upload_server(std::shared_ptr<ApiClient> api, bool admin)
	{
	bool on = bool(api);
	upload_admin_ = admin;
	uploads_.set_server(api, admin);
	artists_.set_uploads_button(on);
	int index = section_index("uploads");
	if(auto row = nav_.get_row_at_index(index)) row->set_visible(on);
	if(size_t(index)<tab_buttons_.size()) tab_buttons_[size_t(index)]->set_visible(on);
	if(view_.section!="uploads") return;
	if(on) uploads_.load();
	else   select_section("library");
	}

void MainWindow::delete_upload(const Album& a)
	{
	auto api = client_for(a.ref.server);
	if(!api) return;
	confirm_delete_upload(*this, api, a, [this](const ApiResult& r) {
		if(!r.ok) {
			failed(r);
			return;
			}
		if(view_.section!="uploads") return;
		albums_.clear();
		tracks_.clear();
		uploads_.load();
		go(0);
		});
	}

void MainWindow::promote_upload(const Album& a, const std::string& artist)
	{
	auto api = client_for(a.ref.server);
	if(!api) return;
	promote_ = std::make_unique<PromoteDialog>(*this, api, a, artist);
	promote_->signal_moved.connect(sigc::mem_fun(*this, &MainWindow::promoted));
	// Not from inside its own handler.
	promote_->signal_hide().connect([this]() {
		Glib::signal_idle().connect_once([this]() { promote_.reset(); });
		});
	promote_->present();
	}

void MainWindow::promoted(const ApiResult& r)
	{
	if(!r.ok) {
		// The server's own words: "already in that folder" and "outside the
		// library" want different fixes.
		failed(r);
		return;
		}
	const auto& moved = jsub(r.body, "movedAlbum");
	ItemRef artist{r.server, jstr(moved, "parent")};
	ItemRef album{r.server, jstr(moved, "id")};
	// The reply did not say where it went; the uploads listing is at least
	// certainly no longer right.
	if(artist.id.empty() || album.id.empty()) {
		if(view_.section=="uploads") show_section("uploads");
		return;
		}
	// It is in the shared library now, so that is where to look at it.
	select_section("library");
	reveal_album(artist, album);
	}

void MainWindow::reveal_album(const ItemRef& artist, const ItemRef& album)
	{
	auto has = [](const ItemRef& ref, const std::vector<ItemRef>& also, const ItemRef& want) {
		if(ref.server==want.server && ref.id==want.id) return true;
		for(const auto& o: also)
			if(o.server==want.server && o.id==want.id) return true;
		return false;
		};
	Pane *left = &artists_;
	if(view_.section=="uploads") {
		uploads_.load();
		left = &uploads_;
		}
	else
		artists_.load(scope());
	left->restore([artist, has](const ListRow& row) {
		return row.kind==ListRow::Kind::artist && has(row.artist.ref, row.artist.also, artist);
		}, [this, album, has](bool found) {
		// Outside the scope picked, say; the album is still worth showing.
		if(!found) {
			open_album(album, "");
			return;
			}
		albums_.restore([album, has](const ListRow& row) {
			return row.kind==ListRow::Kind::album && has(row.album.ref, row.album.also, album);
			});
		});
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
	// The same artist on every server it was merged from.
	ServerCalls sources;
	std::vector<ItemRef> refs = {a.ref};
	refs.insert(refs.end(), a.also.begin(), a.also.end());
	for(const auto& r: refs)
		if(auto api = client_for(r.server)) sources.push_back({api, {{"id", r.id}}});
	if(sources.empty()) return;
	middle_.set_visible_child("albums");
	tracks_.clear();
	view_.artist      = a.ref;
	view_.artist_name = a.name;
	view_.playlist    = {};
	view_.album       = {};
	view_.album_title.clear();
	albums_.set_uploads(view_.section=="uploads", upload_admin_);
	albums_.show_artist(sources, a);
	go(1);
	}

void MainWindow::open_album(const ItemRef& album, const std::string& play_id,
                            const std::string& mark_id)
	{
	auto api = client_for(album.server);
	if(album.id.empty() || !api) return;
	tracks_back_.clear();
	expect_album_.clear();
	view_.album = album;
	view_.album_title.clear();
	tracks_.show_album(api, album.id, play_id, mark_id);
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
			view_.album = {};
			go(0);
			return;
			}
		}
	view_.album_title = a.title;
	save_view();
	tracks_back_ = a.artist;
	update_back();
	auto api = client_for(a.ref.server);
	if(a.artist_id.empty() || !api) return;
	if(middle_.get_visible_child_name()=="albums" && albums_.shows_artist({a.ref.server, a.artist_id}))
		return;
	Artist artist;
	artist.ref  = {a.ref.server, a.artist_id};
	artist.name = a.artist;
	middle_.set_visible_child("albums");
	albums_.set_uploads(view_.section=="uploads", upload_admin_);
	albums_.show_artist({{api, {{"id", a.artist_id}}}}, artist);
	}

void MainWindow::play(const std::vector<Song>& songs, size_t index)
	{
	if(index>=songs.size()) return;
	const Song& song = songs[index];
	// Every film, as on the web client; music only when it is long.
	if(song.duration<resume_min_seconds && !song.is_video) {
		playback_.play(songs, index);
		return;
		}
	auto api = client_for(song.ref.server);
	if(!api) return;
	api->call("getBookmarks", {}, [this, songs, index](const ApiResult& r) {
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
	auto api = client_for(ref.server);
	if(!api) return;
	Params p = {{kind==ListRow::Kind::album ? "albumId" : "id", ref.id}};
	api->call(on ? "star" : "unstar", p, [this, kind, ref, on](const ApiResult& r) {
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

void MainWindow::apply_theme()
	{
	bool dark = settings_.theme=="dark" || (settings_.theme=="auto" && gtk_dark_);
	Gtk::Settings::get_default()->property_gtk_application_prefer_dark_theme() = dark;
	if(dark==dark_css_on_) return;
	dark_css_on_ = dark;
	// Above the main sheet, so its @gd_red is the one looked up.
	if(dark)
		Gtk::StyleProvider::add_provider_for_display(get_display(), dark_css_,
			GTK_STYLE_PROVIDER_PRIORITY_APPLICATION+1);
	else
		Gtk::StyleProvider::remove_provider_for_display(get_display(), dark_css_);
	}

void MainWindow::apply_text_scale()
	{
	Gtk::Settings::get_default()->property_gtk_xft_dpi() = int(std::lround(desktop_dpi_*settings_.text_scale));
	covers_.set_art_scale(desktop_dpi_/(96.0*1024) * settings_.text_scale);
	}

void MainWindow::failed(const ApiResult& r)
	{
	if(r.code==auth_refused) {
		// Every call still in flight to it says so too; the first one acts.
		if(!client_for(r.server)) return;
		const ServerConfig *cfg = nullptr;
		for(const auto& s: servers_)
			if(s.key==r.server) cfg = &s;
		if(!cfg) return;
		secret_clear(*cfg);
		ServerConfig refused = *cfg;
		drop_client(r.server, refused.url + " no longer accepts this login. Please log in again.");
		// The login view, if that was the last one: for this server.
		if(clients_.empty()) login_.prefill(refused.url, refused.user);
		return;
		}
	// Which server, when there is more than one to ask.
	notify(servers_.size()>1 ? server_label(r.server) + ": " + r.error : r.error);
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

#include "mainwindow.hh"
#include "apiclient.hh"
#include "configfile.hh"
#include "jsonread.hh"
#include "secretstore.hh"
#include "uithread.hh"

#include <glibmm/main.h>
#include <gtkmm/cssprovider.h>
#include <nlohmann/json.hpp>

namespace {
	const char *css = R"(
		.pane { border-left: 1px solid alpha(currentColor, 0.15); }
		.pane-header { padding: 10px 12px; min-height: 34px; }
		.pane-title { font-weight: bold; font-size: 1.2em; }
		.row-heading { font-weight: bold; padding: 12px 4px 4px 4px; opacity: 0.6; }
		.pane listview > row { padding: 6px 10px; }
		.hero { margin: 12px; }
		.cover { border-radius: 4px; }
		.notice { padding: 6px 12px; background-color: alpha(@error_color, 0.2); }
		.sidebar { min-width: 170px; }
	)";

	// Credentials the server refuses: wrong user or password.
	const int auth_refused = 40;
}

MainWindow::MainWindow()
	: pool_(4),
	  covers_(pool_),
	  browse_(Gtk::Orientation::VERTICAL),
	  main_(Gtk::Orientation::HORIZONTAL),
	  sidebar_(Gtk::Orientation::VERTICAL),
	  logout_("Log out"),
	  panes_(Gtk::Orientation::HORIZONTAL),
	  artists_(covers_),
	  albums_(covers_),
	  tracks_(covers_)
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

	// The sidebar holds only Library for now; Playlists, Recents, Search and
	// Settings join it in later stages, as on the web client.
	auto library = Gtk::make_managed<Gtk::Label>("Library");
	library->set_xalign(0);
	nav_.append(*library);
	nav_.add_css_class("navigation-sidebar");
	nav_.select_row(*nav_.get_row_at_index(0));
	nav_.set_vexpand(true);
	sidebar_.add_css_class("sidebar");
	sidebar_.append(nav_);
	logout_.add_css_class("flat");
	logout_.set_margin(6);
	sidebar_.append(logout_);
	main_.append(sidebar_);

	panes_.set_homogeneous(true);
	panes_.set_hexpand(true);
	panes_.append(artists_);
	panes_.append(albums_);
	panes_.append(tracks_);
	main_.append(panes_);
	main_.set_vexpand(true);
	browse_.append(main_);

	stack_.add(login_, "login");
	stack_.add(browse_, "browse");
	set_child(stack_);

	login_.signal_login.connect(sigc::mem_fun(*this, &MainWindow::login));
	logout_.signal_clicked().connect(sigc::mem_fun(*this, &MainWindow::logout));
	artists_.signal_artist.connect([this](const Artist& a) {
		tracks_.clear();
		albums_.show_artist(api_, a);
		});
	albums_.signal_album.connect([this](const Album& a) {
		tracks_.show_album(api_, a);
		});
	for(Pane *p: {static_cast<Pane*>(&artists_), static_cast<Pane*>(&albums_), static_cast<Pane*>(&tracks_)})
		p->signal_failed.connect(sigc::mem_fun(*this, &MainWindow::failed));

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
	albums_.clear();
	tracks_.clear();
	artists_.load(api_);
	stack_.set_visible_child("browse");
	}

void MainWindow::logout()
	{
	if(!servers_.empty())
		secret_clear(servers_.front());
	api_.reset();
	covers_.set_api(nullptr);
	artists_.clear();
	albums_.clear();
	tracks_.clear();
	show_login("");
	}

void MainWindow::failed(const ApiResult& r)
	{
	if(r.code==auth_refused) {
		logout();
		login_.set_error("The server no longer accepts this login. Please log in again.");
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

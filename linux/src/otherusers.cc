#include "otherusers.hh"
#include "apiclient.hh"
#include "jsonread.hh"

#include <glibmm/main.h>

OtherUsers::OtherUsers()
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  heading_("Other users")
	{
	heading_.set_xalign(0);
	heading_.add_css_class("row-heading");
	heading_.set_margin_start(6);
	append(heading_);
	list_.add_css_class("navigation-sidebar");
	list_.set_selection_mode(Gtk::SelectionMode::NONE);
	// Opens the album and marks the track without playing it: cutting off
	// our own playback would punish the curiosity.
	list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		size_t i = size_t(row->get_index());
		if(i<rows_.size()) signal_open_album.emit(rows_[i].album, rows_[i].song_id);
		});
	append(list_);
	set_visible(false);
	}

OtherUsers::~OtherUsers()
	{
	timer_.disconnect();
	}

void OtherUsers::set_servers(std::vector<std::pair<std::shared_ptr<ApiClient>, std::string>> servers)
	{
	++gen_;
	timer_.disconnect();
	servers_ = std::move(servers);
	shown_.assign(servers_.size(), {});
	rebuild();
	if(servers_.empty()) return;
	poll();
	timer_ = Glib::signal_timeout().connect_seconds([this]() {
		poll();
		return true;
		}, 30);
	}

void OtherUsers::poll()
	{
	ServerCalls calls;
	for(const auto& s: servers_)
		calls.push_back({s.first, {}});
	unsigned gen = gen_;
	call_each(calls, "getNowPlaying", [this, gen](const std::vector<ApiResult>& rs) {
		if(gen!=gen_) return;
		for(size_t i=0; i<rs.size(); ++i) {
			if(!rs[i].ok) continue;
			const auto& session = servers_[i].first->session();
			shown_[i].clear();
			for(const auto& e: jsub(jsub(rs[i].body, "nowPlaying"), "entry")) {
				// The server keeps listing a paused entry; this list is
				// what is audible.
				std::string user = jstr(e, "username");
				if(user==session.user || jstr(e, "state")=="paused") continue;
				Song s = parse_song(session.server_key, e);
				shown_[i].push_back({user, s.title, s.artist, s.ref.id, {s.ref.server, s.album_id}});
				}
			}
		rebuild();
		});
	}

void OtherUsers::rebuild()
	{
	while(auto row = list_.get_row_at_index(0))
		list_.remove(*row);
	rows_.clear();
	for(size_t i=0; i<shown_.size(); ++i)
		for(const auto& e: shown_[i]) {
			auto box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
			const auto& label = servers_[i].second;
			auto user = Gtk::make_managed<Gtk::Label>(label.empty() ? e.user : e.user + " (" + label + ")");
			user->set_xalign(0);
			user->set_ellipsize(Pango::EllipsizeMode::END);
			auto track = Gtk::make_managed<Gtk::Label>(e.title);
			track->set_xalign(0);
			track->set_ellipsize(Pango::EllipsizeMode::END);
			track->add_css_class("dim-label");
			box->set_tooltip_text(e.artist.empty() ? e.title : e.artist + " - " + e.title);
			box->append(*user);
			box->append(*track);
			list_.append(*box);
			rows_.push_back(e);
			}
	set_visible(!rows_.empty());
	}

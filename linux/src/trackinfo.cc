#include "trackinfo.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "model.hh"
#include "coverstore.hh"
#include "playback.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <gdkmm/clipboard.h>
#include <glibmm/main.h>

#include <algorithm>
#include <cctype>
#include <charconv>

namespace {
	std::string upper(std::string s)
		{
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::toupper(c); });
		return s;
		}

	// X-Gaindrive-Sent in words, as the web client and the apps put it:
	// "original", or a format with an optional bitrate.
	std::string describe_sent(const std::string& sent, const std::string& suffix)
		{
		auto space = sent.find(' ');
		std::string target = sent.substr(0, space);
		std::string kbps   = space==std::string::npos ? "" : sent.substr(space+1);
		if(target=="original")
			return suffix.empty() ? "unconverted" : upper(suffix) + ", unconverted";
		std::string name = upper(target);
		if(target=="m4a" || target=="aac") name = "AAC";
		else if(target=="opus")            name = "Opus";
		else if(target=="ogg" || target=="oga" || target=="vorbis") name = "Vorbis";
		if(!kbps.empty()) return name + " " + kbps + " kbps";
		return name=="FLAC" ? "FLAC, converted losslessly" : name + ", converted";
		}

	// The shortest text that reads back as the same number, as a browser
	// writes a chapter's start into t=.
	std::string number(double v)
		{
		char buf[32];
		auto r = std::to_chars(buf, buf+sizeof buf, v);
		return std::string(buf, r.ptr);
		}

	// A small dim caps heading over a section, as a card's sections are.
	void section(Gtk::Box& box, const std::string& text)
		{
		auto l = Gtk::make_managed<Gtk::Label>(text);
		l->set_xalign(0);
		l->add_css_class("card-section");
		box.append(*l);
		}
}

TrackInfo::TrackInfo(CoverStore& covers, Playback& playback, WorkPool& pool)
	: Gtk::Box(Gtk::Orientation::VERTICAL, 6),
	  playback_(playback),
	  pool_(pool),
	  cover_(std::make_unique<CoverImage>(covers, 64)),
	  head_(Gtk::Orientation::HORIZONTAL, 12),
	  head_text_(Gtk::Orientation::VERTICAL, 2),
	  share_row_(Gtk::Orientation::HORIZONTAL, 6),
	  starts_(Gtk::Orientation::VERTICAL, 0),
	  alive_(std::make_shared<bool>(true))
	{
	add_css_class("info-card");
	set_halign(Gtk::Align::END);
	set_valign(Gtk::Align::END);
	set_size_request(380, -1);
	set_visible(false);

	// What it is, as the player bar shows it, rather than as rows.
	cover_->set_valign(Gtk::Align::START);
	head_.append(*cover_);
	title_.set_xalign(0);
	title_.set_wrap(true);
	title_.set_lines(2);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	title_.add_css_class("card-title");
	sub_.set_xalign(0);
	sub_.set_wrap(true);
	sub_.add_css_class("dim-label");
	head_text_.append(title_);
	head_text_.append(sub_);
	head_text_.set_hexpand(true);
	head_text_.set_valign(Gtk::Align::CENTER);
	head_.append(head_text_);
	close_.set_icon_name("gd-close-symbolic");
	close_.set_tooltip_text("Close");
	close_.add_css_class("flat");
	close_.add_css_class("circular");
	close_.set_valign(Gtk::Align::START);
	head_.append(close_);
	head_.set_margin_bottom(4);
	append(head_);

	for(auto g: {&file_, &playback_grid_}) {
		g->set_column_spacing(16);
		g->set_row_spacing(3);
		}
	section(*this, "FILE");
	append(file_);
	section(*this, "PLAYBACK");
	append(playback_grid_);
	section(*this, "SHARE");
	link_.set_editable(false);
	link_.set_can_focus(false);
	link_.set_hexpand(true);
	copy_.set_icon_name("gd-copy-symbolic");
	copy_.set_tooltip_text("Copy link");
	copy_.add_css_class("flat");
	share_row_.append(link_);
	share_row_.append(copy_);
	append(share_row_);
	starts_.append(at_);
	starts_.append(at_chapter_);
	append(starts_);

	close_.signal_clicked().connect([this]() { signal_close.emit(); });
	copy_.signal_clicked().connect([this]() {
		get_clipboard()->set_text(link_.get_text());
		// Said for a moment in place of the icon, so the click shows.
		copy_.set_label("Copied");
		copied_.disconnect();
		copied_ = Glib::signal_timeout().connect([this]() {
			copy_.set_icon_name("gd-copy-symbolic");
			return false;
			}, 1500);
		});
	// The two starts exclude each other: a link has one t=.
	at_.signal_toggled().connect([this]() {
		if(at_.get_active()) at_chapter_.set_active(false);
		update_link();
		});
	at_chapter_.signal_toggled().connect([this]() {
		if(at_chapter_.get_active()) at_.set_active(false);
		update_link();
		});
	}

TrackInfo::~TrackInfo()
	{
	copied_.disconnect();
	alive_.reset();
	}

void TrackInfo::set_api(std::shared_ptr<ApiClient> api)
	{
	api_ = std::move(api);
	shown_.clear();
	}

const std::string& TrackInfo::shown() const
	{
	return shown_;
	}

void TrackInfo::set_narrow(bool narrow)
	{
	set_size_request(narrow ? -1 : 380, -1);
	}

void TrackInfo::refresh()
	{
	unsigned gen = ++gen_;
	const Song *cur = playback_.current();
	if(!cur || !api_) return;
	Song song = *cur;
	shown_ = song.ref.id;

	title_.set_text(song.title);
	std::string sub = song.artist;
	if(!song.album.empty()) sub += (sub.empty() ? "" : " · ") + song.album;
	sub_.set_text(sub);
	sub_.set_visible(!sub.empty());
	cover_->show_cover(song.cover_art);

	base_          = api_->session().base_url + "/?track=" + song.ref.id;
	at_seconds_    = int(playback_.position());
	chapter_start_ = 0;
	at_.set_active(false);
	at_chapter_.set_active(false);
	at_.set_label("Start at " + format_duration(at_seconds_));
	// "Start at 0:00" offers nothing.
	at_.set_visible(at_seconds_>0);
	at_chapter_.set_visible(false);
	update_link();

	// How the sound reaches the speaker, which is another question than
	// what the file is.
	auto& caster = playback_.caster();
	bool casting = caster.active();
	Rows play = {
		{"Output", casting ? "Chromecast “" + caster.device_name() + "”, fetching from the server"
		                   : "This computer"},
		{"Sent", ""},
		{"Declared type", casting ? playback_.cast_type() : ""}
		};
	std::weak_ptr<bool> alive = alive_;
	if(casting) {
		const auto& sent = playback_.cast_sent();
		// A film's is in words already; a track's is the server's header.
		play[1].second = song.is_video ? sent : sent.empty() ? "" : describe_sent(sent, song.suffix);
		fill(playback_grid_, play);
		}
	else {
		// Asked of the server: with a quality cap, what arrives cannot be
		// told from the request.
		fill(playback_grid_, play);
		std::string url = playback_.local_url();
		pool_.submit([this, alive, gen, url, play, suffix=song.suffix]() mutable {
			std::string sent = ApiClient::sent_of(url);
			on_ui([this, alive, gen, url, play, sent, suffix]() mutable {
				if(alive.expired() || gen!=gen_) return;
				if(!sent.empty())                                  play[1].second = describe_sent(sent, suffix);
				else if(url.find("/rest/hls.")!=std::string::npos) play[1].second = "HLS";
				fill(playback_grid_, play);
				});
			});
		}

	// Asked again rather than taken from the queue, so the starred state
	// and such are current.
	auto show_file = [this](const Song& s, const nlohmann::json& j) {
		bool fresh  = j.is_object();
		int track   = fresh ? jint(j, "track") : s.track;
		int year    = jint(j, "year");
		int bitrate = jint(j, "bitRate");
		int length  = fresh && jint(j, "duration")>0 ? jint(j, "duration") : s.duration;
		bool starred = fresh ? !jsub(j, "starred").is_null() : s.starred;
		std::string suffix = fresh ? jstr(j, "suffix") : s.suffix;
		fill(file_, {
			{"Track",   track>0 ? std::to_string(track) : ""},
			{"Year",    year>0  ? std::to_string(year)  : ""},
			{"Format",  upper(suffix)},
			{"Bitrate", bitrate>0 ? std::to_string(bitrate) + " kbps" : ""},
			{"Length",  length>0 ? format_duration(length) : ""},
			{"Starred", starred ? "Yes" : "No"}
			});
		};
	show_file(song, nlohmann::json());
	api_->call("getSong", {{"id", song.ref.id}}, [this, alive, gen, show_file, song](const ApiResult& r) {
		if(alive.expired() || gen!=gen_ || !r.ok) return;
		show_file(song, jsub(r.body, "song"));
		});

	// The chapter the snapshot falls in, for a concert film or a DJ set that
	// is one file holding many songs. One starting at 0 is the plain link.
	api_->call("getChapters", {{"id", song.ref.id}}, [this, alive, gen](const ApiResult& r) {
		if(alive.expired() || gen!=gen_ || !r.ok) return;
		const auto& list = jsub(jsub(r.body, "chapters"), "chapter");
		if(!list.is_array()) return;
		double start = 0;
		std::string name;
		size_t index = 0;
		for(size_t i=0; i<list.size(); ++i) {
			double s = jnum(list[i], "start");
			if(s<=at_seconds_) {
				start = s;
				name  = jstr(list[i], "name");
				index = i+1;
				}
			}
		if(!(start>0)) return;
		chapter_start_ = start;
		at_chapter_.set_label("Start at chapter " + (name.empty() ? std::to_string(index) : name));
		at_chapter_.set_visible(true);
		});
	}

void TrackInfo::fill(Gtk::Grid& grid, const Rows& rows)
	{
	while(auto child = grid.get_first_child())
		grid.remove(*child);
	int row = 0;
	for(const auto& [label, value]: rows) {
		// As on the web: a row with nothing to say is left out.
		if(value.empty()) continue;
		auto l = Gtk::make_managed<Gtk::Label>(label);
		l->set_xalign(0);
		l->set_valign(Gtk::Align::START);
		l->add_css_class("dim-label");
		auto v = Gtk::make_managed<Gtk::Label>(value);
		v->set_xalign(0);
		v->set_wrap(true);
		v->set_hexpand(true);
		// Selectable for copying, but out of the focus chain, where a
		// selectable label selects all of itself.
		v->set_selectable(true);
		v->set_focusable(false);
		grid.attach(*l, 0, row);
		grid.attach(*v, 1, row);
		++row;
		}
	}

void TrackInfo::update_link()
	{
	double t = 0;
	if(at_chapter_.get_active())  t = chapter_start_;
	else if(at_.get_active())     t = at_seconds_;
	link_.set_text(t>0 ? base_ + "&t=" + number(t) : base_);
	copied_.disconnect();
	copy_.set_icon_name("gd-copy-symbolic");
	}

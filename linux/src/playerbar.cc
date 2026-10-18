#include "playerbar.hh"
#include "coverstore.hh"
#include "playback.hh"

#include <glibmm/main.h>
#include <gtkmm/gestureclick.h>

namespace {
	const auto drag_grace = std::chrono::milliseconds(600);
}

PlayerBar::PlayerBar(CoverStore& covers, Playback& playback)
	: Gtk::Box(Gtk::Orientation::HORIZONTAL, 10),
	  playback_(playback),
	  cover_(std::make_unique<CoverImage>(covers, 48)),
	  info_(Gtk::Orientation::VERTICAL),
	  seek_(Gtk::Orientation::HORIZONTAL)
	{
	add_css_class("player-bar");
	append(*cover_);
	auto click = Gtk::GestureClick::create();
	click->signal_released().connect([this](int, double, double) { signal_cover.emit(); });
	cover_->add_controller(click);
	cover_->set_cursor("pointer");
	cover_->set_tooltip_text("Show this album");

	title_.set_xalign(0);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	title_.add_css_class("heading");
	artist_.set_xalign(0);
	artist_.set_ellipsize(Pango::EllipsizeMode::END);
	artist_.add_css_class("dim-label");
	info_.set_valign(Gtk::Align::CENTER);
	info_.set_size_request(220, -1);
	info_.append(title_);
	info_.append(artist_);
	append(info_);

	prev_.set_icon_name("gd-skip-previous-symbolic");
	prev_.set_tooltip_text("Previous");
	play_.set_icon_name("gd-play-symbolic");
	play_.set_tooltip_text("Play");
	next_.set_icon_name("gd-skip-next-symbolic");
	next_.set_tooltip_text("Next");
	for(auto b: {&prev_, &play_, &next_}) {
		b->add_css_class("flat");
		b->set_valign(Gtk::Align::CENTER);
		append(*b);
		}
	play_.add_css_class("circular");

	seek_.set_draw_value(false);
	seek_.set_hexpand(true);
	seek_.set_valign(Gtk::Align::CENTER);
	seek_.set_range(0, 1);
	append(seek_);
	time_.add_css_class("numeric");
	time_.add_css_class("dim-label");
	append(time_);

	prev_.signal_clicked().connect([this]() { playback_.prev(); });
	play_.signal_clicked().connect([this]() { playback_.toggle(); });
	next_.signal_clicked().connect([this]() { playback_.next(); });
	// change_value fires for the user's moves only, not for set_value().
	seek_.signal_change_value().connect([this](Gtk::ScrollType, double v) {
		user_moved_ = std::chrono::steady_clock::now();
		playback_.seek(v);
		return false;
		}, false);

	playback_.signal_track.connect(sigc::mem_fun(*this, &PlayerBar::track_changed));
	playback_.signal_state.connect(sigc::mem_fun(*this, &PlayerBar::state_changed));
	timer_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &PlayerBar::tick), 250);
	track_changed();
	}

PlayerBar::~PlayerBar()
	{
	timer_.disconnect();
	}

void PlayerBar::track_changed()
	{
	const Song *s = playback_.current();
	title_.set_text(s ? s->title : "");
	artist_.set_text(s ? s->artist : "");
	cover_->show_cover(s ? s->cover_art : "");
	set_sensitive(s!=nullptr);
	auto idx = playback_.index();
	prev_.set_sensitive(s!=nullptr);
	next_.set_sensitive(idx>=0 && idx+1<long(playback_.queue().size()));
	seek_.set_range(0, s && s->duration>0 ? s->duration : 1);
	tick();
	}

void PlayerBar::state_changed(bool playing)
	{
	play_.set_icon_name(playing ? "gd-pause-symbolic" : "gd-play-symbolic");
	play_.set_tooltip_text(playing ? "Pause" : "Play");
	}

bool PlayerBar::tick()
	{
	const Song *s = playback_.current();
	if(!s) {
		time_.set_text("0:00 / 0:00");
		seek_.set_value(0);
		return true;
		}
	// The tag's duration, not the stream's: a converted stream has none
	// until it has been read to the end.
	double pos = playback_.position();
	time_.set_text(format_duration(int(pos)) + " / " + format_duration(s->duration));
	if(std::chrono::steady_clock::now()-user_moved_>drag_grace)
		seek_.set_value(pos);
	return true;
	}

QueueView::QueueView(Playback& playback)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  playback_(playback),
	  heading_("Next tracks")
	{
	heading_.set_xalign(0);
	heading_.add_css_class("row-heading");
	heading_.set_margin_start(6);
	append(heading_);
	list_.add_css_class("navigation-sidebar");
	list_.set_selection_mode(Gtk::SelectionMode::NONE);
	// Both this and the remove button below rebuild the list, so they run
	// after the click rather than inside it, while its row still exists.
	list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		size_t i = size_t(playback_.index()+1+row->get_index());
		Glib::signal_idle().connect_once([this, i]() { playback_.jump(i); });
		});
	append(list_);
	playback_.signal_track.connect(sigc::mem_fun(*this, &QueueView::rebuild));
	rebuild();
	}

void QueueView::rebuild()
	{
	while(auto row = list_.get_row_at_index(0))
		list_.remove(*row);

	const auto& q = playback_.queue();
	size_t first = size_t(playback_.index()+1);
	for(size_t i=first; i<q.size(); ++i) {
		auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 4);
		auto title = Gtk::make_managed<Gtk::Label>(q[i].title);
		title->set_xalign(0);
		title->set_hexpand(true);
		title->set_ellipsize(Pango::EllipsizeMode::END);
		box->set_tooltip_text(q[i].artist.empty() ? q[i].title : q[i].artist + " - " + q[i].title);
		auto remove = Gtk::make_managed<Gtk::Button>();
		remove->set_icon_name("gd-close-symbolic");
		remove->set_tooltip_text("Remove from queue");
		remove->add_css_class("flat");
		remove->signal_clicked().connect([this, i]() {
			Glib::signal_idle().connect_once([this, i]() { playback_.remove(i); });
			});
		box->append(*title);
		box->append(*remove);
		list_.append(*box);
		}
	set_visible(first<q.size());
	}

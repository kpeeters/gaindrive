#include "videoview.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "model.hh"
#include "playback.hh"
#include "settings.hh"
#include "stamp.hh"

#include <gdk/gdkkeysyms.h>
#include <glibmm/main.h>
#include <pangomm/attrlist.h>
#include <gtkmm/eventcontrollerkey.h>
#include <gtkmm/eventcontrollermotion.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/scrolledwindow.h>

#include <algorithm>
#include <iostream>
#include <sstream>

namespace {
	const unsigned hide_after_ms = 3000;
	const double   skip_seconds  = 10;
	// "Previous chapter" past this far into one restarts it instead, as
	// "previous track" does.
	const double   restart_after = 3;
	// A seek lands a little before its target; still that chapter.
	const double   chapter_slack = 0.25;
	// Below this the smallest setting in a small window is unreadable.
	const int      caption_min_px = 14;

	// "01:02:03.456" or "02:03.456", in seconds; -1 when it is neither.
	double vtt_time(const std::string& s)
		{
		double parts[3] = {0, 0, 0};
		int n = 0;
		std::istringstream in(s);
		std::string field;
		while(n<3 && std::getline(in, field, ':')) {
			try {
				parts[n++] = std::stod(field);
				}
			catch(const std::exception& e) {
				std::cout << stamp() << "WebVTT: bad time '" << s << "': " << e.what() << std::endl;
				return -1;
				}
			}
		if(n==2) return parts[0]*60 + parts[1];
		if(n==3) return parts[0]*3600 + parts[1]*60 + parts[2];
		return -1;
		}

	std::string strip_markup(const std::string& line)
		{
		std::string out;
		bool tag = false;
		for(char c: line) {
			if(c=='<')      tag = true;
			else if(c=='>') tag = false;
			else if(!tag)   out += c;
			}
		for(auto [from, to]: {std::pair{"&lt;", "<"}, {"&gt;", ">"}, {"&nbsp;", " "}, {"&amp;", "&"}}) {
			size_t at;
			while((at = out.find(from))!=std::string::npos)
				out.replace(at, std::string(from).size(), to);
			}
		return out;
		}

	Gtk::Button& icon_button(Gtk::Button& b, const char *icon, const char *tip)
		{
		b.set_icon_name(icon);
		b.set_tooltip_text(tip);
		b.add_css_class("flat");
		return b;
		}
}

std::vector<VideoView::Cue> parse_vtt(const std::string& text)
	{
	std::vector<VideoView::Cue> cues;
	std::istringstream in(text);
	std::string line;
	VideoView::Cue *cue = nullptr;
	while(std::getline(in, line)) {
		if(!line.empty() && line.back()=='\r') line.pop_back();
		if(line.empty()) {
			cue = nullptr;
			continue;
			}
		size_t arrow = line.find("-->");
		if(arrow!=std::string::npos) {
			// Settings such as "line:90%" may follow the end time.
			std::istringstream times(line.substr(arrow+3));
			std::string end;
			times >> end;
			std::istringstream first(line.substr(0, arrow));
			std::string start;
			first >> start;
			double a = vtt_time(start), b = vtt_time(end);
			if(a<0 || b<0) continue;
			cues.push_back({a, b, ""});
			cue = &cues.back();
			continue;
			}
		if(cue) {
			if(!cue->text.empty()) cue->text += '\n';
			cue->text += strip_markup(line);
			}
		}
	return cues;
	}

VideoView::VideoView(Playback& playback)
	: playback_(playback),
	  bar_(Gtk::Orientation::HORIZONTAL, 6)
	{
	add_css_class("video");
	set_focusable(true);

	picture_.set_content_fit(Gtk::ContentFit::CONTAIN);
	picture_.set_hexpand(true);
	picture_.set_vexpand(true);
	if(GdkPaintable *p = playback_.paintable())
		picture_.set_paintable(Glib::wrap(p, true));
	set_child(picture_);

	missing_.set_text("No video output: GStreamer's gtk4paintablesink is not installed.");
	missing_.add_css_class("caption");
	missing_.set_visible(playback_.paintable()==nullptr);
	missing_.set_valign(Gtk::Align::CENTER);
	add_overlay(missing_);

	casting_.add_css_class("caption");
	casting_.set_valign(Gtk::Align::CENTER);
	casting_.set_visible(false);
	add_overlay(casting_);

	caption_.add_css_class("caption");
	caption_.set_wrap(true);
	caption_.set_justify(Gtk::Justification::CENTER);
	caption_.set_halign(Gtk::Align::CENTER);
	caption_.set_valign(Gtk::Align::END);
	caption_.set_margin_bottom(90);
	caption_.set_visible(false);
	caption_.set_can_target(false);
	add_overlay(caption_);

	icon_button(close_, "gd-close-symbolic", "Close");
	close_.set_halign(Gtk::Align::END);
	close_.set_margin(8);
	top_.set_child(close_);
	top_.set_valign(Gtk::Align::START);
	top_.set_halign(Gtk::Align::END);
	top_.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
	add_overlay(top_);

	icon_button(chapter_prev_, "gd-skip-previous-symbolic", "Previous chapter");
	icon_button(chapter_next_, "gd-skip-next-symbolic",     "Next chapter");
	icon_button(back_,    "gd-replay-10-symbolic",  "Back 10 seconds");
	icon_button(play_,    "gd-play-symbolic",       "Play");
	icon_button(forward_, "gd-forward-10-symbolic", "Forward 10 seconds");
	icon_button(full_,    "gd-fullscreen-symbolic", "Fullscreen");
	bar_.add_css_class("video-controls");
	bar_.append(chapter_prev_);
	bar_.append(back_);
	bar_.append(play_);
	bar_.append(forward_);
	bar_.append(chapter_next_);
	chapter_now_.add_css_class("chapter-now");
	chapter_now_.set_hexpand(true);
	chapter_now_.set_xalign(0);
	chapter_now_.set_margin_start(12);
	chapter_now_.set_ellipsize(Pango::EllipsizeMode::END);
	bar_.append(chapter_now_);

	for(auto [button, popover, list, icon, tip]: {
			std::tuple{&chapters_button_, &chapters_popover_, &chapters_list_, "gd-chapters-symbolic", "Chapters"},
			std::tuple{&captions_button_, &captions_popover_, &captions_list_, "gd-captions-symbolic", "Subtitles"}}) {
		button->set_icon_name(icon);
		button->set_tooltip_text(tip);
		button->add_css_class("flat");
		button->set_direction(Gtk::ArrowType::UP);
		auto scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
		scroll->set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
		scroll->set_propagate_natural_height(true);
		scroll->set_max_content_height(400);
		list->add_css_class("navigation-sidebar");
		scroll->set_child(*list);
		popover->set_child(*scroll);
		button->set_popover(*popover);
		button->set_visible(false);
		bar_.append(*button);
		}
	bar_.append(full_);
	bottom_.set_child(bar_);
	bottom_.set_valign(Gtk::Align::END);
	bottom_.set_transition_type(Gtk::RevealerTransitionType::CROSSFADE);
	add_overlay(bottom_);

	close_.signal_clicked().connect([this]() { signal_close.emit(); });
	back_.signal_clicked().connect([this]() { playback_.seek(playback_.position()-skip_seconds); });
	forward_.signal_clicked().connect([this]() { playback_.seek(playback_.position()+skip_seconds); });
	play_.signal_clicked().connect([this]() { playback_.toggle(); });
	full_.signal_clicked().connect([this]() { signal_fullscreen.emit(); });
	chapter_prev_.signal_clicked().connect(sigc::mem_fun(*this, &VideoView::chapter_prev));
	chapter_next_.signal_clicked().connect(sigc::mem_fun(*this, &VideoView::chapter_next));

	chapters_list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		size_t i = size_t(row->get_index());
		if(i<chapters_.size()) playback_.seek(chapters_[i].start);
		chapters_popover_.popdown();
		});
	captions_list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		select_caption(row->get_index());
		captions_popover_.popdown();
		});

	playback_.signal_state.connect([this](bool playing) {
		play_.set_icon_name(playing ? "gd-pause-symbolic" : "gd-play-symbolic");
		play_.set_tooltip_text(playing ? "Pause" : "Play");
		poke();
		});

	auto motion = Gtk::EventControllerMotion::create();
	// Hiding the controls and the cursor changes what is under the pointer,
	// and GTK answers that with a motion event at the same spot; only a
	// pointer that really moved brings them back.
	motion->signal_motion().connect([this](double x, double y) {
		if(x==pointer_x_ && y==pointer_y_) return;
		pointer_x_ = x;
		pointer_y_ = y;
		poke();
		});
	add_controller(motion);
	auto click = Gtk::GestureClick::create();
	click->signal_pressed().connect([this](int n, double, double) {
		if(n==2) signal_fullscreen.emit();
		});
	picture_.add_controller(click);
	// In the capture phase, so space toggles playback even while a control
	// has the focus rather than pressing that control.
	auto keys = Gtk::EventControllerKey::create();
	keys->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	keys->signal_key_pressed().connect(sigc::mem_fun(*this, &VideoView::key), false);
	add_controller(keys);

	timer_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &VideoView::tick), 100);
	poke();
	}

VideoView::~VideoView()
	{
	timer_.disconnect();
	hide_.disconnect();
	}

void VideoView::show_film(const Song& film, std::shared_ptr<ApiClient> api)
	{
	if(film.ref.id==film_id_ && api==api_) return;
	film_id_ = film.ref.id;
	api_     = std::move(api);
	unsigned gen = ++gen_;
	chapters_.clear();
	chapter_ = -1;
	signal_chapter.emit(-1);
	caption_ids_.clear();
	cues_.clear();
	caption_.set_visible(false);
	fill_chapters();
	while(auto row = captions_list_.get_row_at_index(0))
		captions_list_.remove(*row);
	captions_button_.set_visible(false);
	if(!api_) return;

	api_->call("getVideoInfo", {{"id", film_id_}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			std::cout << stamp() << "getVideoInfo failed: " << r.error << std::endl;
			return;
			}
		const auto& caps = jsub(jsub(r.body, "videoInfo"), "captions");
		if(!caps.is_array() || caps.empty()) return;
		auto off = Gtk::make_managed<Gtk::Label>("Off");
		off->set_xalign(0);
		captions_list_.append(*off);
		for(const auto& c: caps) {
			std::string name = jstr(c, "name");
			auto label = Gtk::make_managed<Gtk::Label>(name.empty() ? "Subtitles" : name);
			label->set_xalign(0);
			captions_list_.append(*label);
			caption_ids_.push_back(jstr(c, "id"));
			}
		captions_list_.select_row(*captions_list_.get_row_at_index(0));
		captions_button_.set_visible(true);
		});

	api_->call("getChapters", {{"id", film_id_}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		if(!r.ok) {
			std::cout << stamp() << "getChapters failed: " << r.error << std::endl;
			return;
			}
		const auto& list = jsub(jsub(r.body, "chapters"), "chapter");
		if(list.is_array())
			for(const auto& c: list)
				chapters_.push_back({jnum(c, "start"), jstr(c, "name")});
		fill_chapters();
		});
	}

void VideoView::set_fullscreen(bool on)
	{
	fullscreen_ = on;
	full_.set_icon_name(on ? "gd-fullscreen-exit-symbolic" : "gd-fullscreen-symbolic");
	full_.set_tooltip_text(on ? "Leave fullscreen" : "Fullscreen");
	grab_focus();
	}

void VideoView::select_caption(int row)
	{
	if(auto r = captions_list_.get_row_at_index(row))
		captions_list_.select_row(*r);
	// Remembered there for a receiver, which draws its own; the cues are
	// still fetched, for when the film comes back here.
	playback_.set_caption(row);
	cues_.clear();
	caption_.set_visible(false);
	if(row<=0 || size_t(row)>caption_ids_.size() || !api_) return;
	unsigned gen = gen_;
	api_->text("getCaptions", {{"id", film_id_}, {"captionId", caption_ids_[size_t(row-1)]}},
		[this, gen](bool ok, const std::string& body) {
			if(gen!=gen_) return;
			if(!ok) {
				std::cout << stamp() << "getCaptions failed" << std::endl;
				return;
				}
			cues_ = parse_vtt(body);
			std::cout << stamp() << "Subtitles: " << cues_.size() << " cues" << std::endl;
			});
	}

void VideoView::fill_chapters()
	{
	while(auto row = chapters_list_.get_row_at_index(0))
		chapters_list_.remove(*row);
	for(size_t i=0; i<chapters_.size(); ++i) {
		const auto& c = chapters_[i];
		auto box   = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
		// The server leaves an empty name empty, so a save cannot write a
		// placeholder into the file; drawing one is ours to do.
		auto name  = Gtk::make_managed<Gtk::Label>(c.name.empty()
			? "Chapter " + std::to_string(i+1) : c.name);
		auto start = Gtk::make_managed<Gtk::Label>(format_duration(int(c.start)));
		name->set_xalign(0);
		name->set_hexpand(true);
		start->add_css_class("dim-label");
		start->add_css_class("numeric");
		box->append(*name);
		box->append(*start);
		chapters_list_.append(*box);
		}
	bool any = !chapters_.empty();
	chapters_button_.set_visible(any);
	chapter_prev_.set_visible(any);
	chapter_next_.set_visible(any);
	chapter_now_.set_visible(any);
	// Never a value of chapter_at(), so the update below redraws whatever
	// the old list showed.
	chapter_ = -2;
	update_chapter();
	}

int VideoView::chapter_at(double seconds) const
	{
	int at = -1;
	for(size_t i=0; i<chapters_.size(); ++i)
		if(chapters_[i].start<=seconds+chapter_slack) at = int(i);
	return at;
	}

void VideoView::chapter_prev()
	{
	double pos = playback_.position();
	int i = chapter_at(pos);
	if(i<0)
		playback_.seek(0);
	else if(i==0 || pos-chapters_[size_t(i)].start>restart_after)
		playback_.seek(chapters_[size_t(i)].start);
	else
		playback_.seek(chapters_[size_t(i-1)].start);
	poke();
	}

void VideoView::chapter_next()
	{
	double pos = playback_.position();
	for(const auto& c: chapters_)
		if(c.start>pos+chapter_slack) {
			playback_.seek(c.start);
			break;
			}
	poke();
	}

void VideoView::update_chapter()
	{
	int i = chapter_at(playback_.position());
	if(i==chapter_) return;
	chapter_ = i;
	if(i>=0) {
		const auto& name = chapters_[size_t(i)].name;
		chapter_now_.set_text(name.empty() ? "Chapter " + std::to_string(i+1) : name);
		chapters_list_.select_row(*chapters_list_.get_row_at_index(i));
		}
	else {
		chapter_now_.set_text("");
		chapters_list_.unselect_all();
		}
	signal_chapter.emit(i);
	}

bool VideoView::tick()
	{
	if(!get_mapped()) return true;
	update_chapter();
	auto& caster = playback_.caster();
	bool cast = caster.active();
	picture_.set_opacity(cast ? 0 : 1);
	missing_.set_visible(!cast && playback_.paintable()==nullptr);
	casting_.set_visible(cast);
	if(cast) casting_.set_text("Playing on " + caster.device_name());
	if(playback_.cast_picture()) {
		caption_.set_visible(false);
		return true;
		}
	size_caption();
	double pos = playback_.position();
	const Cue *now = nullptr;
	for(const auto& c: cues_)
		if(pos>=c.start && pos<c.end) {
			now = &c;
			break;
			}
	if(now && caption_.get_text().raw()!=now->text)
		caption_.set_text(now->text);
	caption_.set_visible(now!=nullptr);
	return true;
	}

void VideoView::size_caption()
	{
	// Polled from tick() rather than on a resize, which GTK 4 has no signal
	// for; it also picks up a change in the settings.
	int px = std::max(caption_min_px,
		int(get_height()*playback_.settings().subtitle_fraction()));
	if(px==caption_px_) return;
	caption_px_ = px;
	Pango::AttrList attrs;
	auto size = Pango::Attribute::create_attr_size_absolute(px*PANGO_SCALE);
	attrs.insert(size);
	caption_.set_attributes(attrs);
	}

void VideoView::poke()
	{
	top_.set_reveal_child(true);
	bottom_.set_reveal_child(true);
	set_cursor(Glib::RefPtr<Gdk::Cursor>());
	hide_.disconnect();
	hide_ = Glib::signal_timeout().connect(sigc::mem_fun(*this, &VideoView::hide_controls), hide_after_ms);
	}

bool VideoView::hide_controls()
	{
	// Paused, or with a menu open, the controls are what is being looked at.
	if(playback_.playing() && !chapters_popover_.get_visible()
	   && !captions_popover_.get_visible()) {
		top_.set_reveal_child(false);
		bottom_.set_reveal_child(false);
		set_cursor("none");
		}
	return false;
	}

bool VideoView::key(guint keyval, guint, Gdk::ModifierType state)
	{
	// Ctrl+Q and the other accelerators are the application's.
	if((state & (Gdk::ModifierType::CONTROL_MASK | Gdk::ModifierType::ALT_MASK))!=Gdk::ModifierType{})
		return false;
	switch(keyval) {
		case GDK_KEY_space:
			playback_.toggle();
			return true;
		case GDK_KEY_Left:
			playback_.seek(playback_.position()-skip_seconds);
			poke();
			return true;
		case GDK_KEY_Right:
			playback_.seek(playback_.position()+skip_seconds);
			poke();
			return true;
		case GDK_KEY_f:
		case GDK_KEY_F:
			signal_fullscreen.emit();
			return true;
		case GDK_KEY_Escape:
			if(!fullscreen_) return false;
			signal_fullscreen.emit();
			return true;
		default:
			return false;
		}
	}

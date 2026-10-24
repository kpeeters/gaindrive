#include "infoblock.hh"
#include "apiclient.hh"
#include "coverstore.hh"
#include "jsonread.hh"

#include <gdkmm/frameclock.h>
#include <gtkmm/gestureclick.h>
#include <glibmm/main.h>

#include <algorithm>

namespace {
	const unsigned poll_seconds = 15;
	const unsigned max_tries    = 40;
	const int      clamp_lines  = 6;
}

HeightClip::HeightClip(Gtk::Widget& child)
	: child_(child)
	{
	set_overflow(Gtk::Overflow::HIDDEN);
	child_.set_parent(*this);
	}

HeightClip::~HeightClip()
	{
	child_.unparent();
	}

void HeightClip::set_height(int height)
	{
	height_ = height;
	queue_resize();
	}

Gtk::SizeRequestMode HeightClip::get_request_mode_vfunc() const
	{
	return Gtk::SizeRequestMode::HEIGHT_FOR_WIDTH;
	}

void HeightClip::measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
                               int& minimum_baseline, int& natural_baseline) const
	{
	// Width never for a height: a wrapping label asked how wide it must be
	// to fit the eased height answers with lines wider than the pane.
	bool across = orientation==Gtk::Orientation::HORIZONTAL;
	child_.measure(orientation, across ? -1 : for_size, minimum, natural,
	               minimum_baseline, natural_baseline);
	minimum_baseline = natural_baseline = -1;
	// Nor the whole text on one line as the width it would like, which a
	// list row would grant; it fills whatever width it is given anyway.
	if(across) natural = minimum;
	if(orientation==Gtk::Orientation::VERTICAL && height_>=0)
		minimum = natural = height_;
	}

void HeightClip::size_allocate_vfunc(int width, int height, int)
	{
	// The child at its own height for this width; what is below the set
	// height is clipped.
	int min = 0, nat = 0, mb, nb;
	child_.measure(Gtk::Orientation::VERTICAL, width, min, nat, mb, nb);
	child_.size_allocate(Gtk::Allocation(0, 0, width, std::max(height, min)), -1);
	}

InfoBlock::InfoBlock(CoverStore& covers, int portrait_px)
	: Gtk::Box(Gtk::Orientation::HORIZONTAL, 12),
	  body_(Gtk::Orientation::VERTICAL, 4),
	  links_(Gtk::Orientation::HORIZONTAL, 6),
	  clip_(text_),
	  wiki_("", "Wikipedia"),
	  allmusic_("", "AllMusic"),
	  more_("more")
	{
	add_css_class("info-block");
	if(portrait_px>0) {
		portrait_ = std::make_unique<CoverImage>(covers, portrait_px);
		portrait_->set_valign(Gtk::Align::START);
		append(*portrait_);
		}
	text_.set_wrap(true);
	text_.set_xalign(0);
	text_.set_yalign(0);
	text_.set_selectable(true);
	// Out of the focus chain: a selectable label selects all of itself when
	// focus lands on it, as it did when a sliding strip hid the pane that
	// had focus. Still selectable with the mouse.
	text_.set_focusable(false);
	set_clamped(true);
	body_.append(clip_);
	more_.add_css_class("flat");
	// At the right edge, as the web client's toggle, apart from the links.
	more_.set_hexpand(true);
	more_.set_halign(Gtk::Align::END);
	more_.signal_clicked().connect([this]() {
		bool expand = text_.get_lines()>0;
		int from = clip_.get_height();
		// The far end measured at the width the text has now.
		set_clamped(!expand);
		int min = 0, to = 0, mb, nb;
		text_.measure(Gtk::Orientation::VERTICAL, clip_.get_width(), min, to, mb, nb);
		// Collapsing keeps the whole text until the end, so it is not
		// cut off before the space closes.
		if(!expand) set_clamped(false);
		more_.set_label(expand ? "less" : "more");
		animate(from, to);
		});
	// The text is a bigger target than the toggle. Capture, as the label
	// claims its own clicks for selecting; a click that ends a selection is
	// left alone, or copying a sentence would fold the bio.
	auto click = Gtk::GestureClick::create();
	click->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	click->signal_released().connect([this](int, double, double) {
		int start, end;
		if(!more_.get_visible() || text_.get_selection_bounds(start, end)) return;
		more_.activate();
		});
	clip_.add_controller(click);
	links_.add_css_class("info-links");
	links_.append(wiki_);
	links_.append(allmusic_);
	links_.append(more_);
	body_.append(links_);
	pending_.set_text("Looking this up…");
	pending_.set_xalign(0);
	pending_.add_css_class("dim-label");
	body_.append(pending_);
	body_.set_hexpand(true);
	append(body_);
	clear();
	}

InfoBlock::~InfoBlock()
	{
	timer_.disconnect();
	if(tick_ && gobj()) remove_tick_callback(tick_);
	}

void InfoBlock::set_clamped(bool clamped)
	{
	text_.set_lines(clamped ? clamp_lines : -1);
	text_.set_ellipsize(clamped ? Pango::EllipsizeMode::END : Pango::EllipsizeMode::NONE);
	}

void InfoBlock::animate(int from, int to)
	{
	if(tick_) remove_tick_callback(tick_);
	from_  = from;
	to_    = to;
	start_ = 0;
	clip_.set_height(from);
	tick_ = add_tick_callback([this](const Glib::RefPtr<Gdk::FrameClock>& clock) {
		gint64 now = clock->get_frame_time();
		if(!start_) start_ = now;
		double t = std::min(1.0, (now-start_)/250000.0);
		// Ease out, as GTK's own transitions do.
		double e = 1 - (1-t)*(1-t)*(1-t);
		if(t<1) {
			clip_.set_height(int(from_ + (to_-from_)*e));
			return true;
			}
		tick_ = 0;
		// At rest the text decides the height again, so a resize of the
		// pane rewraps it as usual.
		if(more_.get_label()=="more") set_clamped(true);
		clip_.set_height(-1);
		return false;
		});
	}

void InfoBlock::clear()
	{
	++gen_;
	timer_.disconnect();
	api_.reset();
	set_visible(false);
	if(portrait_) portrait_->show_cover("");
	}

void InfoBlock::load(std::shared_ptr<ApiClient> api, const std::string& endpoint,
                     const std::string& id, const std::string& answer,
                     const std::string& text_key, const std::string& portrait_id)
	{
	clear();
	api_      = std::move(api);
	endpoint_ = endpoint;
	id_       = id;
	answer_   = answer;
	text_key_ = text_key;
	tries_    = 0;
	if(portrait_) portrait_->show_cover(portrait_id);
	request();
	}

void InfoBlock::request()
	{
	unsigned gen = gen_;
	api_->call(endpoint_, {{"id", id_}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		// Older servers have no such call; the block then stays away.
		if(!r.ok) return;
		const auto& info = jsub(r.body, answer_);
		std::string text   = jstr(info, text_key_);
		std::string wiki   = jstr(info, "wikiUrl");
		std::string am     = jstr(info, "allMusicUrl");
		const auto& res    = jsub(info, "resolving");
		bool resolving     = res.is_boolean() && res.get<bool>() && tries_<max_tries;
		bool nothing       = text.empty() && wiki.empty() && am.empty();

		if(!(tries_>0 && resolving && nothing)) {
			text_.set_text(text);
			clip_.set_visible(!text.empty());
			// Collapsed at once: a new artist's text does not fold up.
			if(tick_) remove_tick_callback(tick_);
			tick_ = 0;
			set_clamped(true);
			clip_.set_height(-1);
			more_.set_label("more");
			// A guess at whether six lines hold it; a label cannot say.
			more_.set_visible(text.size()>400);
			wiki_.set_uri(wiki);
			wiki_.set_visible(!wiki.empty());
			allmusic_.set_uri(am);
			allmusic_.set_visible(!am.empty());
			links_.set_visible(!wiki.empty() || !am.empty() || more_.get_visible());
			pending_.set_visible(resolving);
			set_visible(portrait_ || !nothing || resolving);
			}
		if(resolving) {
			timer_ = Glib::signal_timeout().connect_seconds([this]() {
				++tries_;
				request();
				return false;
				}, poll_seconds);
			}
		});
	}

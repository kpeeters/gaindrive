#include "panestrip.hh"

#include <gdkmm/frameclock.h>
#include <gtkmm/root.h>
#include <glibmm/main.h>

#include <algorithm>
#include <cmath>

namespace {
	// The width below which a pane is too cramped to read. The web's
	// thresholds (1100 and 650 for the strip) were too late here, with
	// GTK's wider type and padding.
	const int comfortable_pane = 420;

	const double slide_us = 280000;

	// CSS's cubic-bezier(0.4, 0, 0.2, 1), the web client's easing: x(s) is
	// solved for s by bisection, then y(s) is the progress.
	double ease(double t)
		{
		auto bez = [](double a, double b, double s) {
			return 3*a*s*(1-s)*(1-s) + 3*b*s*s*(1-s) + s*s*s;
			};
		double lo = 0, hi = 1;
		for(int i=0; i<30; ++i) {
			double mid = (lo+hi)/2;
			if(bez(0.4, 0.2, mid)<t) lo = mid;
			else                     hi = mid;
			}
		return bez(0.0, 1.0, (lo+hi)/2);
		}
}

PaneStrip::PaneStrip()
	{
	// What is off to either side is not drawn over the sidebar.
	set_overflow(Gtk::Overflow::HIDDEN);
	set_hexpand(true);
	set_vexpand(true);
	}

PaneStrip::~PaneStrip()
	{
	if(tick_id_) remove_tick_callback(tick_id_);
	for(auto p: panes_)
		p->unparent();
	}

void PaneStrip::add(Gtk::Widget& pane)
	{
	pane.set_parent(*this);
	panes_.push_back(&pane);
	}

int PaneStrip::depth() const
	{
	return depth_;
	}

int PaneStrip::count() const
	{
	return count_;
	}

int PaneStrip::leftmost() const
	{
	return std::max(0, depth_ - (count_-1));
	}

Gtk::SizeRequestMode PaneStrip::get_request_mode_vfunc() const
	{
	return Gtk::SizeRequestMode::CONSTANT_SIZE;
	}

void PaneStrip::measure_vfunc(Gtk::Orientation orientation, int, int& minimum, int& natural,
                              int& minimum_baseline, int& natural_baseline) const
	{
	// One pane is the least the strip needs; all of them side by side is
	// what it would like.
	minimum = natural = 0;
	minimum_baseline = natural_baseline = -1;
	for(auto p: panes_) {
		int min = 0, nat = 0, mb = 0, nb = 0;
		p->measure(orientation, -1, min, nat, mb, nb);
		minimum = std::max(minimum, min);
		if(orientation==Gtk::Orientation::HORIZONTAL) natural += nat;
		else                                          natural = std::max(natural, nat);
		}
	natural = std::max(natural, minimum);
	}

int PaneStrip::count_for(int width) const
	{
	// As many as fit at a comfortable width, and never narrower than a
	// pane can go.
	int widest = comfortable_pane;
	for(auto p: panes_) {
		int min = 0, nat = 0, mb = 0, nb = 0;
		p->measure(Gtk::Orientation::HORIZONTAL, -1, min, nat, mb, nb);
		widest = std::max(widest, min);
		}
	return std::clamp(width/widest, 1, int(panes_.size()));
	}

void PaneStrip::size_allocate_vfunc(int width, int height, int)
	{
	if(panes_.empty()) return;
	count_ = count_for(width);
	if(!tick_id_) offset_ = leftmost();
	double pw = double(width)/count_;
	for(size_t i=0; i<panes_.size(); ++i) {
		auto p = panes_[i];
		if(!p->get_child_visible()) continue;
		int min = 0, nat = 0, mb = 0, nb = 0;
		p->measure(Gtk::Orientation::HORIZONTAL, -1, min, nat, mb, nb);
		int x = int(std::lround((double(i) - offset_) * pw));
		p->size_allocate(Gtk::Allocation(x, 0, std::max(int(pw), min), height), -1);
		}
	// Told afterwards: what listens may change widgets, which an allocation
	// must not.
	if((count_!=reported_count_ || leftmost()!=reported_leftmost_) && !layout_queued_) {
		layout_queued_ = true;
		// A member, not a lambda, so it is dropped if this goes first.
		Glib::signal_idle().connect_once(sigc::mem_fun(*this, &PaneStrip::report_layout));
		}
	}

void PaneStrip::report_layout()
	{
	layout_queued_     = false;
	reported_count_    = count_;
	reported_leftmost_ = leftmost();
	show_range();
	signal_layout.emit();
	}

void PaneStrip::show_range()
	{
	// Only what is on screen takes part, so focus cannot wander off to a
	// pane nobody can see. Everything while it slides.
	int left = leftmost();
	auto root  = get_root();
	auto focus = root ? root->get_focus() : nullptr;
	for(size_t i=0; i<panes_.size(); ++i) {
		bool on = tick_id_ || (int(i)>=left && int(i)<left+count_);
		// Focus in a pane about to go goes to the active one, rather than
		// to wherever GTK would move it next.
		if(!on && focus && (focus==panes_[i] || focus->is_ancestor(*panes_[i]))) {
			panes_[size_t(depth_)]->child_focus(Gtk::DirectionType::TAB_FORWARD);
			focus = nullptr;
			}
		if(panes_[i]->get_child_visible()!=on) panes_[i]->set_child_visible(on);
		}
	queue_allocate();
	}

void PaneStrip::slide_to(int depth)
	{
	depth = std::clamp(depth, 0, int(panes_.size())-1);
	int before = leftmost();
	depth_ = depth;
	if(leftmost()==before && !tick_id_) return;
	from_  = offset_;
	start_ = 0;
	if(!tick_id_)
		tick_id_ = add_tick_callback(sigc::mem_fun(*this, &PaneStrip::tick));
	show_range();
	}

bool PaneStrip::tick(const Glib::RefPtr<Gdk::FrameClock>& clock)
	{
	gint64 now = clock->get_frame_time();
	if(!start_) start_ = now;
	double t = std::min(1.0, (now-start_)/slide_us);
	double to = leftmost();
	offset_ = from_ + (to-from_)*ease(t);
	queue_allocate();
	if(t<1) return true;
	offset_  = to;
	tick_id_ = 0;
	show_range();
	reported_leftmost_ = -1;   // reported by the next allocation
	return false;
	}

NarrowBox::NarrowBox(Gtk::Orientation orientation, int threshold)
	: Gtk::Box(orientation), threshold_(threshold)
	{
	}

bool NarrowBox::narrow() const
	{
	return narrow_;
	}

void NarrowBox::size_allocate_vfunc(int width, int height, int baseline)
	{
	Gtk::Box::size_allocate_vfunc(width, height, baseline);
	if((width<threshold_)!=narrow_ && !queued_) {
		queued_ = true;
		Glib::signal_idle().connect_once(sigc::mem_fun(*this, &NarrowBox::check));
		}
	}

void NarrowBox::check()
	{
	queued_ = false;
	// Measured again: the width may have moved back meanwhile.
	bool now = get_width()<threshold_;
	if(now==narrow_) return;
	narrow_ = now;
	signal_narrow.emit(narrow_);
	}

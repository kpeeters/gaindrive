#pragma once

#include <gtkmm/box.h>
#include <gtkmm/widget.h>
#include <sigc++/signal.h>

#include <vector>

// The artists | albums | tracks strip, as the web client's: three panes when
// there is room, two or one when not, and then the active pane (`depth`) is
// always the rightmost one shown, the strip sliding to it. The pane count
// follows the strip's own width, so it is also right beside a sidebar.
class PaneStrip : public Gtk::Widget {
	public:
		PaneStrip();
		~PaneStrip() override;

		// In order, left to right. Not owned.
		void add(Gtk::Widget& pane);

		// Animated, unless nothing would move.
		void slide_to(int depth);
		int  depth() const;
		// Panes shown, and the index of the leftmost of them.
		int  count() const;
		int  leftmost() const;

		// The count or the leftmost pane changed; on the main loop, after the
		// allocation that changed it.
		sigc::signal<void()> signal_layout;

	protected:
		Gtk::SizeRequestMode get_request_mode_vfunc() const override;
		void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
		                   int& minimum_baseline, int& natural_baseline) const override;
		void size_allocate_vfunc(int width, int height, int baseline) override;

	private:
		int  count_for(int width) const;
		void show_range();
		void report_layout();
		bool tick(const Glib::RefPtr<Gdk::FrameClock>& clock);

		std::vector<Gtk::Widget*> panes_;
		int    depth_=0, count_=3;
		// Where the strip is, in panes from the left, while it slides.
		double offset_=0, from_=0;
		gint64 start_=0;
		guint  tick_id_=0;
		bool   layout_queued_=false;
		int    reported_count_=-1, reported_leftmost_=-1;
};

// A box that says when its width crosses `threshold`, for the window's
// narrow layout. Told after the allocation, on the main loop, so a listener
// may change widgets.
class NarrowBox : public Gtk::Box {
	public:
		NarrowBox(Gtk::Orientation orientation, int threshold);

		bool narrow() const;
		sigc::signal<void(bool narrow)> signal_narrow;

	protected:
		void size_allocate_vfunc(int width, int height, int baseline) override;

	private:
		int  threshold_;
		bool narrow_=false, queued_=false;

		void check();
};

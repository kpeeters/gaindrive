#include "isearch.hh"
#include "panes.hh"

#include <gdk/gdk.h>

ISearch::ISearch()
	: Gtk::Box(Gtk::Orientation::HORIZONTAL, 8)
	{
	add_css_class("isearch");
	label_.add_css_class("isearch-label");
	text_label_.add_css_class("isearch-text");
	text_label_.set_xalign(0);
	text_label_.set_hexpand(true);
	text_label_.set_ellipsize(Pango::EllipsizeMode::END);
	note_.add_css_class("dim-label");
	// The mode's keys are nowhere else on screen.
	hint_.set_text("Enter keeps it, Esc goes back");
	hint_.add_css_class("dim-label");
	hint_.add_css_class("caption");
	append(label_);
	append(text_label_);
	append(note_);
	append(hint_);
	set_visible(false);
	}

bool ISearch::on() const
	{
	return pane_!=nullptr;
	}

void ISearch::start(Pane& pane, bool back)
	{
	end(false);
	pane_    = &pane;
	text_    = "";
	origin_  = cursor_ = pane.cursor();
	back_    = back;
	wrapped_ = failing_ = false;
	draw();
	}

void ISearch::end(bool restore)
	{
	if(!pane_) return;
	if(restore && origin_>=0) pane_->focus_row(origin_);
	pane_->set_highlight("");
	pane_ = nullptr;
	text_ = "";
	draw();
	}

int ISearch::from_cursor(int offset) const
	{
	if(cursor_>=0) return cursor_+offset;
	return back_ ? pane_->size()-1 : 0;
	}

void ISearch::step(int from)
	{
	if(text_.empty()) {
		// Nothing typed yet is not a failure, and must not paint the pane.
		failing_ = wrapped_ = false;
		pane_->set_highlight("");
		draw();
		return;
		}
	bool wrapped = false;
	int  hit = pane_->find(text_, from, back_, wrapped);
	failing_ = hit<0;
	if(hit>=0) {
		wrapped_ = wrapped;
		cursor_  = hit;
		pane_->focus_row(hit);
		}
	// Even when the cursor stays, so the rows that do match show while a
	// longer string is typed towards them.
	pane_->set_highlight(text_);
	draw();
	}

bool ISearch::key(guint keyval, Gdk::ModifierType state)
	{
	if(!pane_) return false;
	using M = Gdk::ModifierType;
	bool ctrl  = (state & M::CONTROL_MASK)==M::CONTROL_MASK;
	bool other = (state & (M::ALT_MASK | M::SUPER_MASK | M::META_MASK))!=M(0);
	bool shift = (state & M::SHIFT_MASK)==M::SHIFT_MASK;
	auto lower = gdk_keyval_to_lower(keyval);

	if(keyval==GDK_KEY_Escape) {
		end(true);
		return true;
		}
	if(keyval==GDK_KEY_Return || keyval==GDK_KEY_KP_Enter) {
		// Left to the list, so the same press opens the row found; but not
		// when failing, as the cursor is then on a row that does not match.
		bool failing = failing_;
		end(false);
		return failing;
		}
	// Turned round rather than restarted: the string is kept, so an
	// overshoot costs one press.
	if(ctrl && !other && !shift && (lower==GDK_KEY_s || lower==GDK_KEY_r)) {
		back_ = lower==GDK_KEY_r;
		step(from_cursor(back_ ? -1 : 1));
		return true;
		}
	if(ctrl || other) {
		end(false);
		return false;
		}
	if(keyval==GDK_KEY_BackSpace) {
		// From the origin: a shorter string can only match more, and going
		// on from here would skip the earlier rows it now matches.
		if(!text_.empty()) text_.erase(text_.size()-1);
		cursor_ = origin_;
		step(from_cursor(0));
		return true;
		}
	auto c = gdk_keyval_to_unicode(keyval);
	if(c && g_unichar_isprint(c)) {
		text_ += Glib::ustring(1, gunichar(c)).lowercase();
		// From the row itself, so one that still matches keeps the cursor.
		step(from_cursor(0));
		return true;
		}
	end(false);
	return false;
	}

void ISearch::draw()
	{
	set_visible(pane_!=nullptr);
	if(!pane_) return;
	if(failing_) add_css_class("failing");
	else         remove_css_class("failing");
	// Both halves in one line, as emacs says it.
	label_.set_text(std::string(failing_ ? "Failing I-search" : "I-search")
	                + (back_ ? " backward:" : ":"));
	text_label_.set_text(text_);
	note_.set_text(wrapped_ && !failing_ ? "wrapped" : "");
	}

#include "panes.hh"
#include "apiclient.hh"
#include "coverstore.hh"

#include <gtk/gtk.h>
#include <gtkmm/version.h>
#include <gtkmm/gestureclick.h>
#include <gtkmm/listitem.h>
#include <gtkmm/root.h>
#include <gtkmm/signallistitemfactory.h>

#include <algorithm>
#include <cmath>

// ---- RowWidget ----------------------------------------------------------

RowWidget::RowWidget(CoverStore *covers, int cover_px)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  item_(Gtk::Orientation::HORIZONTAL, 10),
	  text_(Gtk::Orientation::VERTICAL),
	  title_line_(Gtk::Orientation::HORIZONTAL, 4),
	  sub_line_(Gtk::Orientation::HORIZONTAL, 4)
	{
	// The padding here rather than on the list's row, so that an extra row
	// can do without.
	add_css_class("pane-row");
	heading_.set_xalign(0);
	heading_.add_css_class("row-heading");
	append(heading_);

	if(covers && cover_px>0) {
		cover_ = Gtk::make_managed<CoverImage>(*covers, cover_px);
		item_.append(*cover_);
		}
	// The web's red triangle before the number of what plays now.
	mark_.set_from_icon_name("gd-play-symbolic");
	mark_.add_css_class("playing-mark");
	item_.append(mark_);
	lead_.set_width_chars(3);
	lead_.set_xalign(1);
	lead_.add_css_class("dim-label");
	lead_.add_css_class("numeric");
	item_.append(lead_);

	title_.set_xalign(0);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	title_.add_css_class("row-title");
	subtitle_.set_xalign(0);
	subtitle_.set_ellipsize(Pango::EllipsizeMode::END);
	subtitle_.add_css_class("dim-label");
	text_.set_hexpand(true);
	text_.set_valign(Gtk::Align::CENTER);
	for(auto mark: {&title_video_, &sub_video_}) {
		mark->set_from_icon_name("gd-movie-symbolic");
		mark->add_css_class("video-mark");
		mark->set_visible(false);
		}
	sub_video_.add_css_class("dim-label");
	watched_.add_css_class("watch-mark");
	// Drawn rather than cut from the font: Material Symbols has only six
	// steps of this pie, and here any fraction costs nothing.
	progress_.set_content_width(16);
	progress_.set_content_height(16);
	progress_.set_valign(Gtk::Align::CENTER);
	progress_.add_css_class("watch-mark");
	progress_.set_draw_func([this](const Cairo::RefPtr<Cairo::Context>& cr, int w, int h) {
		auto c = progress_.get_color();
		cr->set_source_rgba(c.get_red(), c.get_green(), c.get_blue(), c.get_alpha());
		// The check's circle spans 800 of the glyph's 960 units; the
		// same here, so the two marks are one size.
		double x = w/2.0, y = h/2.0, r = std::min(w, h)/2.0*800/960;
		const double ring = 1.5, gap = 1, top = -M_PI/2;
		cr->set_line_width(ring);
		cr->arc(x, y, r-ring/2, 0, 2*M_PI);
		cr->stroke();
		cr->move_to(x, y);
		cr->arc(x, y, r-ring-gap, top, top+2*M_PI*progress_fraction_);
		cr->close_path();
		cr->fill();
		});
	title_line_.append(title_video_);
	title_line_.append(title_);
	sub_line_.append(sub_video_);
	sub_line_.append(subtitle_);
	text_.append(title_line_);
	text_.append(sub_line_);
	item_.append(text_);

	// The actions before the time, as on the web, so the times line up
	// at the edge.
	for(auto b: {&star, &list, &queue}) {
		b->add_css_class("flat");
		b->add_css_class("row-action");
		b->set_valign(Gtk::Align::CENTER);
		b->set_visible(false);
		item_.append(*b);
		}
	// Right by the time. A row action, but one that stays up once there
	// is something to show, as a set star does.
	watch_box_.append(watched_);
	watch_box_.append(progress_);
	watch.set_child(watch_box_);
	watch.add_css_class("flat");
	watch.add_css_class("row-action");
	watch.set_valign(Gtk::Align::CENTER);
	watch.set_visible(false);
	item_.append(watch);
	trail_.add_css_class("dim-label");
	trail_.add_css_class("numeric");
	item_.append(trail_);
	queue.set_icon_name("gd-add-symbolic");
	queue.set_tooltip_text("Add to queue");
	// After the time, which an album row does not have; apart from the
	// others, so a delete is not one slip away from a star.
	for(auto b: {&promote, &remove_upload}) {
		b->add_css_class("flat");
		b->set_valign(Gtk::Align::CENTER);
		b->set_visible(false);
		item_.append(*b);
		}
	promote.set_icon_name("gd-library-add-symbolic");
	promote.set_tooltip_text("Move to shared library");
	remove_upload.set_icon_name("gd-delete-symbolic");
	remove_upload.set_tooltip_text("Delete from your uploads");
	append(item_);
	}

RowWidget::~RowWidget()
	{
	drop_extra();
	if(registry) registry->erase(this);
	}

void RowWidget::show_extra(Gtk::Widget& w)
	{
	set_playing(false);
	heading_.set_visible(false);
	item_.set_visible(false);
	if(extra_==&w) return;
	drop_extra();
	// A recycled row may still hold it, if GTK bound this one first.
	if(auto other = dynamic_cast<RowWidget*>(w.get_parent()))
		other->drop_extra();
	extra_ = &w;
	append(w);
	// No padding of its own, so it takes no room while the widget hides.
	add_css_class("extra");
	}

void RowWidget::drop_extra()
	{
	if(!extra_) return;
	remove(*extra_);
	extra_ = nullptr;
	remove_css_class("extra");
	}

void RowWidget::show_heading(const std::string& text)
	{
	drop_extra();
	set_playing(false);
	heading_.set_text(text);
	heading_.set_visible(true);
	item_.set_visible(false);
	}

void RowWidget::round_cover()
	{
	if(cover_) cover_->set_round(true);
	}

void RowWidget::show_item(const ItemRef& cover, const std::string& lead,
                          const std::string& title, const std::string& subtitle,
                          const std::string& trail)
	{
	drop_extra();
	heading_.set_visible(false);
	item_.set_visible(true);
	show_actions(false, false, ListButton::none, false);
	show_upload_actions(false, false);
	if(cover_) {
		cover_->set_round(false);
		cover_->show_cover(cover);
		}
	lead_.set_text(lead);
	lead_.set_visible(!lead.empty());
	// Numbered rows keep the slot, so the numbers stay put as the track
	// changes; set_playing adds it to the others.
	mark_.set_visible(!lead.empty());
	title_.set_text(title);
	subtitle_.set_text(subtitle);
	subtitle_.set_visible(!subtitle.empty());
	sub_line_.set_visible(!subtitle.empty());
	show_video_mark(VideoMark::none, "");
	watch.set_visible(false);
	trail_.set_text(trail);
	}

void RowWidget::show_video_mark(VideoMark where, const std::string& tooltip)
	{
	title_video_.set_visible(where==VideoMark::title);
	sub_video_.set_visible(where==VideoMark::subtitle);
	title_video_.set_tooltip_text(tooltip);
	sub_video_.set_tooltip_text(tooltip);
	// An album with no year or count still has a line for its mark.
	if(where==VideoMark::subtitle) sub_line_.set_visible(true);
	}

void RowWidget::show_watch(bool watched, double progress)
	{
	bool started = !watched && progress>0;
	watch.set_visible(true);
	watch.set_tooltip_text(watched ? "Mark as not watched"
		: started ? std::to_string(std::lround(progress*100)) + "% watched - mark as watched"
		:           "Mark as watched");
	if(watched || started) watch.add_css_class("marked");
	else                   watch.remove_css_class("marked");
	watched_.set_from_icon_name(watched ? "gd-check-circle-symbolic"
	                                    : "gd-check-circle-outline-symbolic");
	watched_.set_visible(!started);
	progress_.set_visible(started);
	if(!started || progress==progress_fraction_) return;
	progress_fraction_ = progress;
	progress_.queue_draw();
	}

void RowWidget::show_actions(bool show_star, bool starred, ListButton mode, bool show_queue)
	{
	star.set_visible(show_star);
	star.set_icon_name(starred ? "gd-star-filled-symbolic" : "gd-star-symbolic");
	star.set_tooltip_text(starred ? "Unstar" : "Star");
	if(starred) star.add_css_class("starred");
	else        star.remove_css_class("starred");
	list_mode = mode;
	list.set_visible(mode!=ListButton::none);
	if(mode==ListButton::add) {
		list.set_icon_name("gd-playlist-add-symbolic");
		list.set_tooltip_text("Add to playlist");
		}
	else if(mode==ListButton::remove) {
		list.set_icon_name("gd-close-symbolic");
		list.set_tooltip_text("Remove from playlist");
		}
	queue.set_visible(show_queue);
	// Actions show on hover, as on the web, so a long list is not a
	// column of icons; a set star stays.
	if(show_star || show_queue) add_css_class("hover-actions");
	else                        remove_css_class("hover-actions");
	}

void RowWidget::show_upload_actions(bool show_remove, bool show_promote)
	{
	remove_upload.set_visible(show_remove);
	promote.set_visible(show_promote);
	}

void RowWidget::set_playing(bool on)
	{
	if(on) add_css_class("playing");
	else   remove_css_class("playing");
	if(!lead_.get_visible()) mark_.set_visible(on);
	}

void RowWidget::highlight(const Glib::ustring& needle)
	{
	for(auto label: {&title_, &subtitle_}) {
		Pango::AttrList attrs;
		auto raw   = label->get_text();
		auto lower = raw.lowercase();
		// Pango counts in bytes, and lowercasing does not keep the byte
		// length of every character; such a label keeps its match unpainted
		// rather than painting the wrong span.
		if(!needle.empty() && lower.bytes()==raw.bytes()) {
			const auto& hay = lower.raw();
			const auto& n   = needle.raw();
			for(auto at = hay.find(n); at!=std::string::npos; at = hay.find(n, at+n.size())) {
				// The web's accent, on which only white reads.
				auto bg = Pango::Attribute::create_attr_background(0xa3a3, 0x1616, 0x2323);
				auto fg = Pango::Attribute::create_attr_foreground(0xffff, 0xffff, 0xffff);
				for(Pango::Attribute *a: {static_cast<Pango::Attribute*>(&bg), static_cast<Pango::Attribute*>(&fg)}) {
					a->set_start_index(at);
					a->set_end_index(at+n.size());
					attrs.insert(*a);
					}
				}
			}
		// Always set: a recycled row would otherwise keep another row's marks.
		label->set_attributes(attrs);
		}
	}

// ---- Pane ---------------------------------------------------------------

Pane::Pane(CoverStore& covers, int cover_px, bool boxed)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  covers_(covers),
	  header_(Gtk::Orientation::HORIZONTAL, 6),
	  top_(Gtk::Orientation::VERTICAL),
	  bottom_(Gtk::Orientation::VERTICAL),
	  scroll_box_(Gtk::Orientation::VERTICAL)
	{
	add_css_class("pane");
	title_.set_xalign(0);
	title_.set_hexpand(true);
	title_.set_ellipsize(Pango::EllipsizeMode::END);
	title_.add_css_class("pane-title");
	header_.add_css_class("pane-header");
	back_.add_css_class("flat");
	back_.add_css_class("back-link");
	back_.set_valign(Gtk::Align::CENTER);
	back_.set_visible(false);
	back_.signal_clicked().connect([this]() { signal_back.emit(); });
	header_.append(back_);
	header_.append(title_);
	append(header_);

	empty_.set_wrap(true);
	empty_.set_xalign(0);
	empty_.add_css_class("dim-label");
	empty_.add_css_class("pane-empty");
	empty_.set_visible(false);
	append(empty_);

	widgets_   = std::make_shared<std::set<RowWidget*>>();
	store_     = Gio::ListStore<ListRow>::create();
	selection_ = Gtk::SingleSelection::create(store_);
	selection_->set_autoselect(false);
	selection_->set_can_unselect(true);
	selection_->unselect_all();

	auto factory = Gtk::SignalListItemFactory::create();
	factory->signal_setup().connect([this, cover_px](const Glib::RefPtr<Gtk::ListItem>& li) {
		auto w = Gtk::make_managed<RowWidget>(&covers_, cover_px);
		w->star.signal_clicked().connect([this, w]() {
			if(!w->row) return;
			const auto& r = *w->row;
			if(r.kind==ListRow::Kind::song)
				signal_star.emit(r.kind, r.song.ref, !r.song.starred);
			else if(r.kind==ListRow::Kind::album)
				signal_star.emit(r.kind, r.album.ref, !r.album.starred);
			});
		w->watch.signal_clicked().connect([this, w]() {
			if(w->row && w->row->kind==ListRow::Kind::song)
				signal_watched.emit({w->row->song.ref}, !w->row->song.watched);
			});
		w->list.signal_clicked().connect([this, w]() {
			if(!w->row) return;
			if(w->list_mode==RowWidget::ListButton::remove)
				remove_from_list(*w->row);
			else
				signal_pick_playlist.emit(w->row->song, w->list);
			});
		w->queue.signal_clicked().connect([this, w]() {
			if(w->row) signal_enqueue.emit(w->row->song);
			});
		w->remove_upload.signal_clicked().connect([this, w]() {
			if(w->row) upload_action(*w->row, false);
			});
		w->promote.signal_clicked().connect([this, w]() {
			if(w->row) upload_action(*w->row, true);
			});
		w->registry = widgets_;
		widgets_->insert(w);
		li->set_child(*w);
		});
	factory->signal_bind().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
		auto row = std::dynamic_pointer_cast<ListRow>(li->get_item());
		auto w   = dynamic_cast<RowWidget*>(li->get_child());
		if(!row || !w) return;
		bool item = row->kind!=ListRow::Kind::heading && row->kind!=ListRow::Kind::extra;
		li->set_activatable(item);
		li->set_selectable(item);
		w->row = row;
		bind(*w, *row);
		w->set_playing(is_playing(*row));
		w->highlight(needle_);
		});
	factory->signal_unbind().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
		if(auto w = dynamic_cast<RowWidget*>(li->get_child())) {
			w->row.reset();
			w->drop_extra();
			}
		});
	list_.set_model(selection_);
	list_.set_factory(factory);

	// Selecting is what opens the next pane, as a click on a web row does;
	// Enter selects too.
	selection_->signal_selection_changed().connect([this](guint, guint) {
		auto row = std::dynamic_pointer_cast<ListRow>(selection_->get_selected_item());
		if(row) activated(*row);
		});
	list_.signal_activate().connect([this](guint pos) {
		// Enter on the row already selected opens it again, which a narrow
		// strip needs after going back.
		if(pos==selection_->get_selected()) {
			if(auto row = std::dynamic_pointer_cast<ListRow>(selection_->get_selected_item()))
				activated(*row);
			}
		else
			selection_->set_selected(pos);
		});
	// The same for a click: it does not change the selection, so nothing
	// else would notice it. In the capture phase, before the row's own
	// handling, and never for one of the row's buttons.
	auto again = Gtk::GestureClick::create();
	again->set_propagation_phase(Gtk::PropagationPhase::CAPTURE);
	again->signal_released().connect([this](int, double x, double y) {
		auto selected = std::dynamic_pointer_cast<ListRow>(selection_->get_selected_item());
		if(!selected) return;
		for(auto w = list_.pick(x, y); w && w!=&list_; w = w->get_parent()) {
			if(dynamic_cast<Gtk::Button*>(w)) return;
			if(auto rw = dynamic_cast<RowWidget*>(w)) {
				if(rw->row==selected) activated(*selected);
				return;
				}
			}
		});
	list_.add_controller(again);

	scroll_.set_vexpand(true);
	scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
	if(boxed) {
		scroll_box_.append(top_);
		scroll_box_.append(list_);
		scroll_box_.append(bottom_);
		scroll_.set_child(scroll_box_);
		}
	else
		scroll_.set_child(list_);
	append(scroll_);
	}

void Pane::clear()
	{
	++gen_;
	set_title("");
	set_empty("");
	set_rows({});
	}

void Pane::star_changed(ListRow::Kind kind, const ItemRef& ref, bool on)
	{
	for(guint i=0; i<store_->get_n_items(); ++i) {
		auto r = store_->get_item(i);
		if(kind==ListRow::Kind::song && r->kind==kind && r->song.ref.id==ref.id
		   && r->song.ref.server==ref.server)
			r->song.starred = on;
		if(kind==ListRow::Kind::album && r->kind==kind && r->album.ref.id==ref.id
		   && r->album.ref.server==ref.server)
			r->album.starred = on;
		}
	rebind_all();
	}

void Pane::watch_progress(const ItemRef& song, double pos)
	{
	bool changed = false;
	for(guint i=0; i<store_->get_n_items(); ++i) {
		auto r = store_->get_item(i);
		if((r->kind==ListRow::Kind::song || r->kind==ListRow::Kind::chapter)
		   && r->song.ref.id==song.id && r->song.ref.server==song.server)
			changed |= note_watch(r->song, pos);
		}
	if(changed) rebind_all();
	}

void Pane::watched_changed(const std::vector<ItemRef>& songs, bool on)
	{
	for(guint i=0; i<store_->get_n_items(); ++i) {
		auto r = store_->get_item(i);
		if(r->kind!=ListRow::Kind::song && r->kind!=ListRow::Kind::chapter) continue;
		for(const auto& ref: songs)
			if(r->song.ref.id==ref.id && r->song.ref.server==ref.server)
				set_watched(r->song, on);
		}
	rebind_all();
	}

void Pane::set_playing(const ItemRef& song)
	{
	playing_ = song;
	rebind_all();
	}

void Pane::rebind_all()
	{
	// A copy: binding may lead GTK to create or destroy row widgets.
	auto widgets = *widgets_;
	for(auto w: widgets)
		if(w->row && widgets_->count(w)) {
			auto row = w->row;
			bind(*w, *row);
			w->set_playing(is_playing(*row));
			w->highlight(needle_);
			}
	}

bool Pane::is_playing(const ListRow& row) const
	{
	return row.kind==ListRow::Kind::song && !playing_.id.empty()
	    && row.song.ref.id==playing_.id && row.song.ref.server==playing_.server;
	}

void Pane::unselect()
	{
	selection_->unselect_all();
	}

void Pane::report(const ApiResult& r)
	{
	signal_failed.emit(r);
	}

void Pane::set_back(const std::string& label)
	{
	back_.set_label("\u2190 " + label);
	back_.set_visible(!label.empty());
	}

void Pane::set_title(const std::string& t)
	{
	title_.set_text(t);
	}

void Pane::set_empty(const std::string& text)
	{
	empty_.set_text(text);
	empty_.set_visible(!text.empty());
	}

void Pane::set_rows(const std::vector<Glib::RefPtr<ListRow>>& rows)
	{
	selection_->unselect_all();
	store_->splice(0, store_->get_n_items(), rows);
	scroll_.get_vadjustment()->set_value(0);
	// An empty list is a pane being cleared before its load.
	if(restore_match_ && !rows.empty()) apply_restore();
	}

void Pane::restore(std::function<bool(const ListRow&)> match, std::function<void(bool)> done)
	{
	restore_match_ = std::move(match);
	restore_done_  = std::move(done);
	if(store_->get_n_items()>0) apply_restore();
	}

void Pane::cancel_restore()
	{
	restore_match_ = nullptr;
	restore_done_  = nullptr;
	}

bool Pane::restoring() const
	{
	return bool(restore_match_);
	}

void Pane::apply_restore()
	{
	// Taken first: selecting activates the row, which may start another
	// restore elsewhere, or here.
	auto match = std::move(restore_match_);
	auto done  = std::move(restore_done_);
	cancel_restore();
	for(guint i=0; i<store_->get_n_items(); ++i) {
		auto row = store_->get_item(i);
		if(!match(*row)) continue;
		// Before the selection, so a restore the next pane is to make is
		// already waiting when this row's activation saves the view.
		if(done) done(true);
		selection_->set_selected(i);
#if GTKMM_CHECK_VERSION(4,12,0)
		list_.scroll_to(i, Gtk::ListScrollFlags::NONE);
#endif
		return;
		}
	if(done) done(false);
	}

void Pane::scroll_to(std::function<bool(const ListRow&)> match)
	{
	for(guint i=0; i<store_->get_n_items(); ++i)
		if(match(*store_->get_item(i))) {
#if GTKMM_CHECK_VERSION(4,12,0)
			list_.scroll_to(i, Gtk::ListScrollFlags::NONE);
#endif
			return;
			}
	}

int Pane::size() const
	{
	return store_->get_n_items();
	}

int Pane::cursor() const
	{
	// The focus sits on GTK's row widget, whose child is ours, or on one of
	// our row's buttons, inside it.
	auto root  = get_root();
	auto focus = root ? root->get_focus() : nullptr;
	for(auto w = focus; w && w!=&list_; w = w->get_parent()) {
		auto rw = dynamic_cast<const RowWidget*>(w);
		if(!rw) rw = dynamic_cast<const RowWidget*>(w->get_first_child());
		if(rw && rw->row) {
			auto [found, pos] = store_->find(rw->row);
			if(found) return pos;
			}
		}
	auto sel = selection_->get_selected();
	return sel==GTK_INVALID_LIST_POSITION ? -1 : int(sel);
	}

int Pane::find(const Glib::ustring& needle, int start, bool back, bool& wrapped) const
	{
	// `start` may be one past either end; the double modulo folds it back,
	// and comparing with the raw value is what reports that step as a wrap.
	int n = size();
	for(int k=0; k<n; ++k) {
		int i = (((back ? start-k : start+k) % n) + n) % n;
		if(search_text(*store_->get_item(i)).raw().find(needle.raw())!=std::string::npos) {
			wrapped = back ? i>start : i<start;
			return i;
			}
		}
	return -1;
	}

void Pane::focus_row(int pos)
	{
#if GTK_CHECK_VERSION(4,12,0)
	gtk_list_view_scroll_to(list_.gobj(), pos, GTK_LIST_SCROLL_FOCUS, nullptr);
#endif
	}

void Pane::set_highlight(const Glib::ustring& needle)
	{
	if(needle==needle_) return;
	needle_ = needle;
	rebind_all();
	}

Glib::ustring Pane::search_text(const ListRow& row) const
	{
	std::string t;
	switch(row.kind) {
		case ListRow::Kind::artist:   t = row.artist.name; break;
		case ListRow::Kind::album:
			t = row.album.title + " " + row.album.artist;
			if(row.album.year) t += " " + std::to_string(row.album.year);
			break;
		case ListRow::Kind::song:
			t = row.song.title + " " + row.song.artist + " " + row.song.album;
			break;
		case ListRow::Kind::playlist: t = row.playlist.name; break;
		case ListRow::Kind::setting:  t = row.text + " " + row.detail; break;
		case ListRow::Kind::chapter:
			t = row.text.empty() ? "Chapter " + std::to_string(row.index) : row.text;
			break;
		// Headings name groups rather than being something to go to.
		default: break;
		}
	return Glib::ustring(t).lowercase();
	}

void Pane::set_badge(std::function<std::string(const std::string&)> badge)
	{
	badge_ = std::move(badge);
	}

std::string Pane::badges(const ItemRef& ref, const std::vector<ItemRef>& also) const
	{
	if(!badge_ || !row_badges_) return "";
	std::string out = badge_(ref.server);
	for(const auto& r: also) {
		auto b = badge_(r.server);
		if(!b.empty()) out += ", " + b;
		}
	return out;
	}

std::string Pane::heading_on(const std::string& text, const std::string& server) const
	{
	auto b = badge_ ? badge_(server) : "";
	return b.empty() ? text : text + " · " + b;
	}

void Pane::bind_song_hit(RowWidget& w, const Song& s, const std::string& trail)
	{
	std::string sub = s.artist;
	if(!s.album.empty()) sub += (sub.empty() ? "" : " · ") + s.album;
	auto where = badges(s.ref);
	if(!where.empty()) sub += (sub.empty() ? "" : " · ") + where;
	w.show_item({s.ref.server, s.cover_art}, "", s.title, sub, trail);
	if(s.is_video) w.show_video_mark(RowWidget::VideoMark::title, "Video");
	w.show_actions(true, s.starred, RowWidget::ListButton::add, true);
	}

void Pane::bind_album_hit(RowWidget& w, const Album& a)
	{
	std::string sub = a.artist;
	if(a.year) sub += (sub.empty() ? "" : " · ") + std::to_string(a.year);
	auto where = badges(a.ref, a.also);
	if(!where.empty()) sub += (sub.empty() ? "" : " · ") + where;
	w.show_item({a.ref.server, a.cover_art}, "", a.title, sub, "");
	show_album_videos(w, a);
	w.show_actions(true, a.starred, RowWidget::ListButton::none, false);
	}

void Pane::show_album_videos(RowWidget& w, const Album& a)
	{
	// A count in the tooltip, not the line: one bonus documentary does not
	// make a folder of films.
	if(a.video_count>0)
		w.show_video_mark(RowWidget::VideoMark::subtitle,
		                  a.video_count==1 ? "1 video" : std::to_string(a.video_count) + " videos");
	}

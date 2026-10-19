#include "panes.hh"
#include "apiclient.hh"
#include "coverstore.hh"

#include <gtkmm/gestureclick.h>
#include <gtkmm/listitem.h>
#include <gtkmm/signallistitemfactory.h>

// ---- RowWidget ----------------------------------------------------------

RowWidget::RowWidget(CoverStore *covers, int cover_px)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  item_(Gtk::Orientation::HORIZONTAL, 10),
	  text_(Gtk::Orientation::VERTICAL)
	{
	heading_.set_xalign(0);
	heading_.add_css_class("row-heading");
	append(heading_);

	if(covers && cover_px>0) {
		cover_ = Gtk::make_managed<CoverImage>(*covers, cover_px);
		item_.append(*cover_);
		}
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
	text_.append(title_);
	text_.append(subtitle_);
	item_.append(text_);

	trail_.add_css_class("dim-label");
	trail_.add_css_class("numeric");
	item_.append(trail_);
	for(auto b: {&star, &list, &queue}) {
		b->add_css_class("flat");
		b->add_css_class("row-action");
		b->set_valign(Gtk::Align::CENTER);
		b->set_visible(false);
		item_.append(*b);
		}
	queue.set_icon_name("gd-add-symbolic");
	queue.set_tooltip_text("Add to queue");
	append(item_);
	}

RowWidget::~RowWidget()
	{
	if(registry) registry->erase(this);
	}

void RowWidget::show_heading(const std::string& text)
	{
	set_playing(false);
	heading_.set_text(text);
	heading_.set_visible(true);
	item_.set_visible(false);
	}

void RowWidget::show_item(const std::string& cover_id, const std::string& lead,
                          const std::string& title, const std::string& subtitle,
                          const std::string& trail)
	{
	heading_.set_visible(false);
	item_.set_visible(true);
	show_actions(false, false, ListButton::none, false);
	if(cover_) cover_->show_cover(cover_id);
	lead_.set_text(lead);
	lead_.set_visible(!lead.empty());
	title_.set_text(title);
	subtitle_.set_text(subtitle);
	subtitle_.set_visible(!subtitle.empty());
	trail_.set_text(trail);
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
	}

void RowWidget::set_playing(bool on)
	{
	if(on) add_css_class("playing");
	else   remove_css_class("playing");
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
		w->registry = widgets_;
		widgets_->insert(w);
		li->set_child(*w);
		});
	factory->signal_bind().connect([this](const Glib::RefPtr<Gtk::ListItem>& li) {
		auto row = std::dynamic_pointer_cast<ListRow>(li->get_item());
		auto w   = dynamic_cast<RowWidget*>(li->get_child());
		if(!row || !w) return;
		bool item = row->kind!=ListRow::Kind::heading;
		li->set_activatable(item);
		li->set_selectable(item);
		w->row = row;
		bind(*w, *row);
		w->set_playing(is_playing(*row));
		});
	factory->signal_unbind().connect([](const Glib::RefPtr<Gtk::ListItem>& li) {
		if(auto w = dynamic_cast<RowWidget*>(li->get_child()))
			w->row.reset();
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
	}

void Pane::bind_song_hit(RowWidget& w, const Song& s, const std::string& trail)
	{
	std::string sub = s.artist;
	if(!s.album.empty()) sub += (sub.empty() ? "" : " · ") + s.album;
	w.show_item(s.cover_art, "", s.title, sub, trail);
	w.show_actions(true, s.starred, RowWidget::ListButton::add, true);
	}

void Pane::bind_album_hit(RowWidget& w, const Album& a)
	{
	std::string sub = a.artist;
	if(a.year) sub += (sub.empty() ? "" : " · ") + std::to_string(a.year);
	w.show_item(a.cover_art, "", a.title, sub, "");
	w.show_actions(true, a.starred, RowWidget::ListButton::none, false);
	}

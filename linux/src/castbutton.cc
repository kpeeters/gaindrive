#include "castbutton.hh"
#include "playback.hh"

CastButton::CastButton(Playback& playback)
	: playback_(playback),
	  box_(Gtk::Orientation::VERTICAL, 8),
	  searching_(Gtk::Orientation::HORIZONTAL, 8),
	  searching_text_("Looking for receivers…"),
	  stop_("Stop casting")
	{
	add_css_class("flat");
	set_valign(Gtk::Align::CENTER);
	set_icon_name("gd-cast-symbolic");
	set_tooltip_text("Cast");

	heading_.set_xalign(0);
	heading_.add_css_class("heading");
	box_.append(heading_);

	searching_.append(spinner_);
	searching_text_.add_css_class("dim-label");
	searching_.append(searching_text_);
	box_.append(searching_);

	list_.set_selection_mode(Gtk::SelectionMode::NONE);
	list_.add_css_class("navigation-sidebar");
	list_.signal_row_activated().connect([this](Gtk::ListBoxRow *row) {
		popover_.popdown();
		playback_.cast_to(size_t(row->get_index()));
		});
	box_.append(list_);

	stop_.signal_clicked().connect([this]() {
		popover_.popdown();
		playback_.stop_casting();
		});
	box_.append(stop_);

	box_.set_margin(6);
	box_.set_size_request(260, -1);
	popover_.set_child(box_);
	set_popover(popover_);
	popover_.signal_show().connect(sigc::mem_fun(*this, &CastButton::opened));

	auto& caster = playback_.caster();
	caster.signal_devices.connect(sigc::mem_fun(*this, &CastButton::rebuild));
	caster.signal_session.connect(sigc::mem_fun(*this, &CastButton::session_changed));
	session_changed();
	// Now rather than on the first open, so the popover has receivers to
	// show at once; opening it scans again, for ones that came and went.
	caster.discover();
	}

void CastButton::opened()
	{
	// Every time: receivers come and go, and a scan is a few seconds.
	playback_.caster().discover();
	rebuild();
	}

void CastButton::rebuild()
	{
	auto& caster = playback_.caster();
	bool active = caster.active();
	heading_.set_text(active ? "Casting to " + caster.device_name() : "Cast to");
	// The row always keeps its line, the stopped spinner included, so the
	// popover does not change height when a scan ends.
	if(caster.discovering()) spinner_.start();
	else                     spinner_.stop();

	while(auto row = list_.get_first_child())
		list_.remove(*row);
	for(const auto& d: caster.devices()) {
		auto row  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
		auto name = Gtk::make_managed<Gtk::Label>(d.name);
		name->set_xalign(0);
		row->append(*name);
		// Model and address as on the web client, so a receiver found on the
		// wrong network shows as such before it fails to answer.
		std::string sub = d.model;
		if(!d.address.empty()) sub += (sub.empty() ? "" : " · ") + d.address;
		if(!sub.empty()) {
			auto info = Gtk::make_managed<Gtk::Label>(sub);
			info->set_xalign(0);
			info->add_css_class("dim-label");
			row->append(*info);
			}
		list_.append(*row);
		}
	list_.set_visible(!caster.devices().empty());
	if(caster.discovering())
		searching_text_.set_text("Looking for receivers…");
	else if(caster.devices().empty())
		searching_text_.set_text("No receivers found.");
	else
		searching_text_.set_text("");
	stop_.set_visible(active);
	}

void CastButton::session_changed()
	{
	bool active = playback_.caster().active();
	set_icon_name(active ? "gd-cast-connected-symbolic" : "gd-cast-symbolic");
	if(active) add_css_class("casting");
	else       remove_css_class("casting");
	rebuild();
	}

#include "tunebutton.hh"
#include "playback.hh"
#include "wiimpanel.hh"

#include <cmath>

TunePanel::TunePanel(Playback& playback, AppSettings& settings)
	: Gtk::Box(Gtk::Orientation::VERTICAL, 8),
	  playback_(playback),
	  volume_(Gtk::Orientation::HORIZONTAL, 6),
	  heading_("Volume"),
	  fixed_("This receiver has a fixed volume."),
	  wiim_(std::make_unique<WiimPanel>(settings))
	{
	add_css_class("tune-panel");
	set_halign(Gtk::Align::END);
	set_valign(Gtk::Align::END);
	set_size_request(260, -1);
	set_visible(false);

	heading_.set_xalign(0);
	heading_.add_css_class("heading");
	append(heading_);

	// Steps of one percent only: a slider on a remote amplifier is one
	// careless drag away from full volume.
	down_.set_icon_name("gd-volume-down-symbolic");
	down_.set_tooltip_text("Quieter");
	up_.set_icon_name("gd-volume-up-symbolic");
	up_.set_tooltip_text("Louder");
	level_.add_css_class("numeric");
	level_.set_width_chars(5);
	for(auto b: {&down_, &up_}) b->add_css_class("flat");
	volume_.append(down_);
	volume_.append(level_);
	volume_.append(up_);
	volume_.set_halign(Gtk::Align::CENTER);
	append(volume_);
	fixed_.set_xalign(0);
	fixed_.add_css_class("dim-label");
	append(fixed_);
	down_.signal_clicked().connect([this]() { playback_.caster().volume_step(-1); });
	up_.signal_clicked().connect([this]() { playback_.caster().volume_step(1); });

	append(*wiim_);

	auto& caster = playback_.caster();
	caster.signal_session.connect(sigc::mem_fun(*this, &TunePanel::session_changed));
	caster.signal_volume.connect(sigc::mem_fun(*this, &TunePanel::volume_changed));
	session_changed();
	}

TunePanel::~TunePanel() = default;

void TunePanel::opened()
	{
	// Every time: the device's own buttons and app change it meanwhile.
	auto& caster = playback_.caster();
	if(caster.is_wiim())
		wiim_->open(caster.device().id, caster.device().address);
	volume_changed();
	}

void TunePanel::session_changed()
	{
	// Read when the panel next opens, but laid out already, so the panel
	// does not change size as it appears.
	wiim_->close();
	wiim_->set_visible(playback_.caster().is_wiim());
	volume_changed();
	}

void TunePanel::volume_changed()
	{
	auto& caster = playback_.caster();
	bool known = caster.active() && caster.volume_known();
	// Blank rather than a guess until the receiver states its level, which
	// it does once it has been given something to play.
	level_.set_text(known ? std::to_string(std::lround(caster.volume()*100)) + "%" : "");
	bool usable = known && !caster.volume_fixed();
	down_.set_sensitive(usable);
	up_.set_sensitive(usable);
	fixed_.set_visible(known && caster.volume_fixed());
	}

TuneButton::TuneButton(Playback& playback, AppSettings& settings)
	: playback_(playback),
	  panel_(std::make_unique<TunePanel>(playback, settings))
	{
	add_css_class("flat");
	set_valign(Gtk::Align::CENTER);
	set_icon_name("gd-equalizer-symbolic");
	set_tooltip_text("Volume and equaliser");
	signal_toggled().connect([this]() {
		panel_->set_visible(get_active());
		if(get_active()) panel_->opened();
		});
	auto session = [this]() {
		bool active = playback_.caster().active();
		if(!active) set_active(false);
		set_visible(active);
		};
	playback_.caster().signal_session.connect(session);
	session();
	}

TuneButton::~TuneButton() = default;

TunePanel& TuneButton::panel()
	{
	return *panel_;
	}

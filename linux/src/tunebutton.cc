#include "tunebutton.hh"
#include "localeq.hh"
#include "playback.hh"
#include "settings.hh"
#include "wiimpanel.hh"

#include <cmath>

TunePanel::TunePanel(Playback& playback, AppSettings& settings)
	: Gtk::Box(Gtk::Orientation::VERTICAL, 8),
	  playback_(playback),
	  cast_(Gtk::Orientation::VERTICAL, 8),
	  volume_(Gtk::Orientation::HORIZONTAL, 6),
	  heading_("Volume"),
	  fixed_("This receiver has a fixed volume."),
	  wiim_(std::make_unique<WiimPanel>(settings)),
	  local_(std::make_unique<LocalEqPanel>(playback, settings))
	{
	add_css_class("tune-panel");
	set_halign(Gtk::Align::END);
	set_valign(Gtk::Align::END);
	set_size_request(260, -1);
	set_visible(false);

	heading_.set_xalign(0);
	heading_.add_css_class("heading");
	cast_.append(heading_);

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
	cast_.append(volume_);
	fixed_.set_xalign(0);
	fixed_.add_css_class("dim-label");
	cast_.append(fixed_);
	down_.signal_clicked().connect([this]() { playback_.caster().volume_step(-1); });
	up_.signal_clicked().connect([this]() { playback_.caster().volume_step(1); });

	cast_.append(*wiim_);
	append(cast_);
	append(*local_);
	local_->signal_switched.connect([this](bool on) { signal_eq_switched.emit(on); });

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

void TunePanel::set_narrow(bool narrow)
	{
	wiim_->set_narrow(narrow);
	local_->set_narrow(narrow);
	set_size_request(narrow ? -1 : 260, -1);
	}

void TunePanel::session_changed()
	{
	// Read when the panel next opens, but laid out already, so the panel
	// does not change size as it appears.
	wiim_->close();
	wiim_->set_visible(playback_.caster().is_wiim());
	// The local equaliser does nothing to what a receiver plays.
	bool casting = playback_.caster().active();
	cast_.set_visible(casting);
	local_->set_visible(!casting);
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
	  settings_(settings),
	  panel_(std::make_unique<TunePanel>(playback, settings))
	{
	add_css_class("flat");
	set_valign(Gtk::Align::CENTER);
	set_icon_name("gd-equalizer-symbolic");
	signal_toggled().connect([this]() {
		panel_->set_visible(get_active());
		if(get_active()) panel_->opened();
		});
	panel_->signal_eq_switched.connect([this](bool) { update(); });
	playback_.caster().signal_session.connect(sigc::mem_fun(*this, &TuneButton::update));
	update();
	}

void TuneButton::update()
	{
	bool casting = playback_.caster().active();
	set_tooltip_text(casting ? "Volume and equaliser" : "Equaliser");
	// As the web's active class: the local equaliser is shaping the sound.
	if(!casting && settings_.eq_on) add_css_class("eq-on");
	else                            remove_css_class("eq-on");
	}

TuneButton::~TuneButton() = default;

TunePanel& TuneButton::panel()
	{
	return *panel_;
	}

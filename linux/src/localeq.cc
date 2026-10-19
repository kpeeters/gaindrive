#include "localeq.hh"
#include "playback.hh"
#include "player.hh"
#include "settings.hh"

#include <glibmm/main.h>
#include <gtkmm/stringlist.h>

#include <algorithm>

namespace {
	// The web client's presets, in band order.
	struct Preset { const char *name; std::vector<int> gains; };
	const std::vector<Preset> builtin_presets = {
		{"Flat",         { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
		{"Bass boost",   { 6, 5, 3, 1, 0, 0, 0, 0, 0, 0}},
		{"Treble boost", { 0, 0, 0, 0, 0, 1, 2, 4, 5, 6}},
		{"Loudness",     { 6, 5, 3, 0,-2,-2, 0, 3, 5, 6}},
		{"Vocal",        {-3,-2, 0, 2, 4, 4, 3, 1, 0,-1}},
		{"Rock",         { 4, 3, 1,-1,-2, 0, 2, 4, 5, 5}},
	};

	const unsigned save_ms = 500;

	std::string db_label(int v)
		{
		if(v>0) return "+" + std::to_string(v);
		if(v<0) return "−" + std::to_string(-v);
		return "0";
		}

	std::string band_label(int hz)
		{
		return hz>=1000 ? std::to_string(hz/1000) + "k" : std::to_string(hz);
		}

	// The stored curve, padded or cut to the bands there are.
	std::vector<int> curve_of(const AppSettings& s)
		{
		auto g = s.eq_gains;
		g.resize(eq_bands.size(), 0);
		return g;
		}
}

LocalEqPanel::LocalEqPanel(Playback& playback, AppSettings& settings)
	: Gtk::Box(Gtk::Orientation::VERTICAL, 6),
	  playback_(playback),
	  settings_(settings),
	  top_(Gtk::Orientation::HORIZONTAL, 6),
	  bands_(Gtk::Orientation::HORIZONTAL, 4),
	  buttons_(Gtk::Orientation::HORIZONTAL, 6),
	  save_row_(Gtk::Orientation::HORIZONTAL, 6),
	  title_("Equaliser"),
	  save_("Save as…"),
	  delete_("Delete"),
	  save_go_("Save")
	{
	title_.set_xalign(0);
	title_.add_css_class("heading");
	status_.set_xalign(1);
	status_.set_hexpand(true);
	status_.set_ellipsize(Pango::EllipsizeMode::END);
	status_.add_css_class("dim-label");
	on_.set_valign(Gtk::Align::CENTER);
	top_.append(title_);
	top_.append(status_);
	top_.append(on_);
	append(top_);

	presets_.set_model(Gtk::StringList::create({}));
	append(presets_);

	for(int hz: eq_bands) {
		auto col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
		auto val = Gtk::make_managed<Gtk::Label>("0");
		val->add_css_class("numeric");
		val->add_css_class("caption");
		// Room for "−12", so a column does not change width with its value.
		val->set_width_chars(3);
		auto fader = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::VERTICAL);
		fader->set_range(-eq_range, eq_range);
		fader->set_increments(1, 3);
		fader->set_round_digits(0);
		fader->set_draw_value(false);
		// Up is louder.
		fader->set_inverted(true);
		fader->set_size_request(-1, 160);
		fader->set_halign(Gtk::Align::CENTER);
		fader->set_tooltip_text(hz>=1000 ? std::to_string(hz/1000) + " kHz"
		                                 : std::to_string(hz) + " Hz");
		auto lab = Gtk::make_managed<Gtk::Label>(band_label(hz));
		lab->add_css_class("caption");
		lab->add_css_class("dim-label");
		col->append(*val);
		col->append(*fader);
		col->append(*lab);
		col->set_hexpand(true);
		bands_.append(*col);
		faders_.push_back(fader);
		readouts_.push_back(val);
		fader->signal_value_changed().connect([this, fader, val]() {
			val->set_text(db_label(int(fader->get_value())));
			if(!syncing_) faders_moved();
			});
		}
	// Equal columns whatever their labels say, as the WiiM's.
	bands_.set_homogeneous(true);
	bands_.set_size_request(460, -1);
	append(bands_);

	save_.add_css_class("flat");
	delete_.add_css_class("flat");
	buttons_.append(save_);
	buttons_.append(delete_);
	append(buttons_);
	save_name_.set_placeholder_text("Name");
	save_name_.set_hexpand(true);
	save_row_.append(save_name_);
	save_row_.append(save_go_);
	save_row_.set_visible(false);
	append(save_row_);

	on_.property_active().signal_changed().connect([this]() {
		if(syncing_) return;
		settings_.eq_on = on_.get_active();
		settings_.save();
		apply();
		render();
		signal_switched.emit(settings_.eq_on);
		});
	presets_.property_selected().signal_changed().connect([this]() {
		if(!syncing_) choose_preset();
		});
	save_.signal_clicked().connect([this]() {
		save_row_.set_visible(!save_row_.get_visible());
		if(save_row_.get_visible()) {
			save_name_.set_text("");
			save_name_.grab_focus();
			}
		});
	save_go_.signal_clicked().connect(sigc::mem_fun(*this, &LocalEqPanel::save_slot));
	save_name_.signal_activate().connect(sigc::mem_fun(*this, &LocalEqPanel::save_slot));
	delete_.signal_clicked().connect(sigc::mem_fun(*this, &LocalEqPanel::delete_slot));

	apply();
	render();
	}

LocalEqPanel::~LocalEqPanel()
	{
	// A drag that ended just before the window closed.
	if(save_timer_.connected()) {
		save_timer_.disconnect();
		settings_.save();
		}
	}

void LocalEqPanel::apply()
	{
	auto gains = curve_of(settings_);
	playback_.set_eq(settings_.eq_on, gains);
	int boost = settings_.eq_on ? std::max(0, *std::max_element(gains.begin(), gains.end())) : 0;
	status_.set_text("Preamp " + db_label(-boost) + " dB");
	}

void LocalEqPanel::render()
	{
	syncing_ = true;
	on_.set_active(settings_.eq_on);
	auto gains = curve_of(settings_);
	for(size_t i=0; i<faders_.size(); ++i) {
		faders_[i]->set_value(gains[i]);
		readouts_[i]->set_text(db_label(gains[i]));
		}
	// What a switched-off equaliser is not applying goes inert, the preset
	// menu included, as on the web.
	bool on = settings_.eq_on;
	presets_.set_sensitive(on);
	bands_.set_sensitive(on);
	save_.set_sensitive(on);
	save_row_.set_sensitive(on);

	std::vector<std::string> entries = {"Custom"};
	for(const auto& p: builtin_presets) entries.push_back(p.name);
	for(const auto& [name, curve]: settings_.eq_slots) entries.push_back(name);
	// Only when the list changed: a new model rebuilds the menu.
	if(entries!=entries_) {
		entries_ = std::move(entries);
		std::vector<Glib::ustring> labels(entries_.begin(), entries_.end());
		presets_.set_model(Gtk::StringList::create(labels));
		}
	show_preset();
	syncing_ = false;
	}

std::string LocalEqPanel::matching_slot() const
	{
	auto gains = curve_of(settings_);
	for(const auto& [name, curve]: settings_.eq_slots)
		if(curve==gains) return name;
	return "";
	}

void LocalEqPanel::show_preset()
	{
	// The first entry whose curve this is, built-ins first, as on the web.
	auto gains = curve_of(settings_);
	guint index = 0;
	for(size_t i=0; i<builtin_presets.size() && index==0; ++i)
		if(builtin_presets[i].gains==gains) index = guint(i+1);
	std::string hit = matching_slot();
	if(index==0 && !hit.empty())
		for(size_t i=1+builtin_presets.size(); i<entries_.size(); ++i)
			if(entries_[i]==hit) index = guint(i);
	bool was = syncing_;
	syncing_ = true;
	presets_.set_selected(index);
	syncing_ = was;
	delete_.set_sensitive(settings_.eq_on && !hit.empty());
	}

void LocalEqPanel::choose_preset()
	{
	guint i = presets_.get_selected();
	if(i==0 || i>=entries_.size()) return;
	if(i<=builtin_presets.size())
		settings_.eq_gains = builtin_presets[i-1].gains;
	else {
		auto slot = settings_.eq_slots.find(entries_[i]);
		if(slot==settings_.eq_slots.end()) return;
		settings_.eq_gains = slot->second;
		}
	settings_.save();
	apply();
	render();
	}

void LocalEqPanel::faders_moved()
	{
	std::vector<int> gains;
	for(auto f: faders_) gains.push_back(int(f->get_value()));
	if(gains==curve_of(settings_)) return;
	// The sound follows the hand; the file is written once it rests.
	settings_.eq_gains = gains;
	apply();
	show_preset();
	save_timer_.disconnect();
	save_timer_ = Glib::signal_timeout().connect([this]() {
		settings_.save();
		return false;
		}, save_ms);
	}

void LocalEqPanel::save_slot()
	{
	std::string name = save_name_.get_text();
	name.erase(0, name.find_first_not_of(" \t"));
	name.erase(name.find_last_not_of(" \t")+1);
	if(name.empty()) return;
	// A saved curve under a built-in's name would leave that preset
	// unreachable behind two entries of the same name.
	for(const auto& p: builtin_presets)
		if(name==p.name) {
			status_.set_text("A built-in preset has that name");
			return;
			}
	settings_.eq_slots[name] = curve_of(settings_);
	settings_.save();
	save_row_.set_visible(false);
	apply();
	render();
	}

void LocalEqPanel::delete_slot()
	{
	std::string hit = matching_slot();
	if(hit.empty()) return;
	settings_.eq_slots.erase(hit);
	settings_.save();
	render();
	}

void LocalEqPanel::set_narrow(bool narrow)
	{
	// Shorter faders, and no minimum width, so the card fits a narrow
	// window, as the web client's does.
	for(auto f: faders_) f->set_size_request(-1, narrow ? 88 : 160);
	bands_.set_size_request(narrow ? -1 : 460, -1);
	}

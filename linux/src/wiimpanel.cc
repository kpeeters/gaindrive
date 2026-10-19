#include "wiimpanel.hh"
#include "settings.hh"
#include "uithread.hh"
#include "wiimeq.hh"
#include "workpool.hh"

#include <glibmm/main.h>
#include <gtkmm/stringlist.h>

#include <algorithm>
#include <optional>

namespace {
	// The presets WiiM's own documentation lists, for when EQGetList fails,
	// so the menu is never empty. The fetched list comes first: firmware
	// versions differ, and owners create presets on the device.
	const std::vector<std::string> fallback_presets = {
		"Flat", "Acoustic", "Bass Booster", "Bass Reducer", "Classical", "Dance",
		"Deep", "Electronic", "Hip-Hop", "Jazz", "Latin", "Loudness", "Lounge",
		"Piano", "Pop", "R&B", "Rock", "Small Speakers", "Spoken Word",
		"Treble Booster", "Treble Reducer", "Vocal Booster"
	};

	// A fader is written once it has rested this long: a LAN round trip per
	// step of a drag would queue commands behind each other on the device.
	const unsigned commit_ms = 400;

	// The distance from flat: how 0..99 maps to decibels is unverified, so no
	// dB figure is shown.
	std::string offset_label(int v)
		{
		int d = v - wiimeq::LEVEL_FLAT;
		if(d>0) return "+" + std::to_string(d);
		if(d<0) return "−" + std::to_string(-d);
		return "0";
		}
}

WiimPanel::WiimPanel(AppSettings& settings)
	: Gtk::Box(Gtk::Orientation::VERTICAL, 6),
	  settings_(settings),
	  pool_(std::make_unique<WorkPool>(1)),
	  top_(Gtk::Orientation::HORIZONTAL, 6),
	  bands_(Gtk::Orientation::HORIZONTAL, 4),
	  save_row_(Gtk::Orientation::HORIZONTAL, 6),
	  buttons_(Gtk::Orientation::HORIZONTAL, 6),
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

	for(const auto& band: wiimeq::BANDS) {
		auto col = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
		auto val = Gtk::make_managed<Gtk::Label>("0");
		val->add_css_class("numeric");
		val->add_css_class("caption");
		// Room for "−49", so a column does not change width with its value.
		val->set_width_chars(3);
		auto fader = Gtk::make_managed<Gtk::Scale>(Gtk::Orientation::VERTICAL);
		fader->set_range(wiimeq::LEVEL_MIN, wiimeq::LEVEL_MAX);
		fader->set_increments(1, 5);
		fader->set_round_digits(0);
		fader->set_draw_value(false);
		// Up is louder.
		fader->set_inverted(true);
		fader->set_size_request(-1, 160);
		fader->set_halign(Gtk::Align::CENTER);
		std::string label = band.label;
		fader->set_tooltip_text(label.back()=='k' ? label.substr(0, label.size()-1) + " kHz"
		                                          : label + " Hz");
		auto lab = Gtk::make_managed<Gtk::Label>(band.label);
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
			val->set_text(offset_label(int(fader->get_value())));
			if(syncing_) return;
			commit_timer_.disconnect();
			commit_timer_ = Glib::signal_timeout().connect([this]() {
				commit_bands();
				return false;
				}, commit_ms);
			});
		}
	// Equal columns whatever their labels say, and wide enough that the
	// faders are not on top of each other.
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
		bool on = on_.get_active();
		run([on](const std::string& a) { return wiimeq::set_on(a, on); }, "");
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
	save_go_.signal_clicked().connect(sigc::mem_fun(*this, &WiimPanel::save_slot));
	save_name_.signal_activate().connect(sigc::mem_fun(*this, &WiimPanel::save_slot));
	delete_.signal_clicked().connect(sigc::mem_fun(*this, &WiimPanel::delete_slot));

	set_visible(false);
	}

WiimPanel::~WiimPanel()
	{
	commit_timer_.disconnect();
	}

void WiimPanel::open(const std::string& device_id, const std::string& address)
	{
	unsigned gen = ++gen_;
	commit_timer_.disconnect();
	device_id_  = device_id;
	address_    = address;
	have_state_ = false;
	save_row_.set_visible(false);
	status_.set_text("Reading…");
	set_sensitive(false);
	set_visible(true);
	pool_->submit([this, gen, address]() {
		// The state read is the reachability test; a failed preset list is
		// then about the list, and the documented names stand in.
		auto st    = wiimeq::state(address);
		auto names = wiimeq::presets(address);
		on_ui([this, gen, st, names]() {
			if(gen!=gen_) return;
			if(!st) {
				status_.set_text("Not answering");
				return;
				}
			device_presets_ = names && !names->empty() ? *names : fallback_presets;
			state_      = {st->on, st->preset, st->bands};
			have_state_ = true;
			render();
			});
		});
	}

void WiimPanel::close()
	{
	++gen_;
	commit_timer_.disconnect();
	have_state_ = false;
	set_visible(false);
	}

void WiimPanel::run(std::function<bool(const std::string&)> cmd, std::string preset)
	{
	if(!have_state_) return;
	// Not made insensitive meanwhile: a fader that goes insensitive in the
	// middle of a drag can miss its button release, which leaves GTK's grab
	// in place and the popover unable to close. The commands queue in order.
	unsigned gen = gen_;
	pool_->submit([this, gen, address=address_, cmd, preset]() {
		std::optional<wiimeq::State> st;
		if(cmd(address)) st = wiimeq::state(address);
		on_ui([this, gen, st, preset]() {
			if(gen!=gen_) return;
			if(!st) {
				// What the device last said is put back, faders included.
				render();
				status_.set_text("Not answering");
				return;
				}
			// The device keeps naming the last preset loaded after its bands
			// have moved on, so its own name is believed only on a fresh read;
			// after a change the curve decides the menu (show_preset).
			state_ = {st->on, preset.empty() ? state_.preset : preset, st->bands};
			render();
			});
		});
	}

void WiimPanel::render()
	{
	syncing_ = true;
	set_sensitive(true);
	on_.set_active(state_.on);
	bool degraded = state_.bands.size()!=faders_.size();
	bands_.set_visible(!degraded);
	buttons_.set_visible(!degraded);
	if(degraded) save_row_.set_visible(false);
	status_.set_text(degraded ? "No band values" : "");
	// Not while a change is still settling: the answer to an earlier one
	// would pull the fader from under the pointer.
	if(!degraded && !commit_timer_.connected())
		for(size_t i=0; i<faders_.size(); ++i) {
			faders_[i]->set_value(state_.bands[i]);
			readouts_[i]->set_text(offset_label(state_.bands[i]));
			}
	// What a switched-off equaliser is not applying goes inert; the menu
	// stays live, because choosing a preset is how it comes back on.
	bands_.set_sensitive(state_.on);
	save_.set_sensitive(state_.on);
	save_row_.set_sensitive(state_.on);

	std::vector<std::string> entries = {"Custom"};
	entries.insert(entries.end(), device_presets_.begin(), device_presets_.end());
	auto dev = settings_.wiim_slots.find(device_id_);
	if(dev!=settings_.wiim_slots.end())
		for(const auto& [name, curve]: dev->second)
			entries.push_back(name);
	// Only when the list changed: a new model rebuilds the menu, which is
	// better not done under the user's hands after every command.
	if(entries!=entries_) {
		entries_ = std::move(entries);
		std::vector<Glib::ustring> labels(entries_.begin(), entries_.end());
		presets_.set_model(Gtk::StringList::create(labels));
		}
	show_preset();
	syncing_ = false;
	}

std::string WiimPanel::matching_slot() const
	{
	auto dev = settings_.wiim_slots.find(device_id_);
	if(dev==settings_.wiim_slots.end() || state_.bands.empty()) return "";
	for(const auto& [name, curve]: dev->second)
		if(curve==state_.bands) return name;
	return "";
	}

void WiimPanel::show_preset()
	{
	// A saved curve is recognised by its bands; a device preset only by the
	// name, as the device does not publish its presets' curves.
	std::string hit  = matching_slot();
	std::string name = hit;
	if(name.empty() && std::count(device_presets_.begin(), device_presets_.end(), state_.preset))
		name = state_.preset;
	guint index = 0;
	// Saved names come after the device's, so a search from the back finds
	// a slot before a device preset of the same name.
	for(size_t i=entries_.size(); i-->1; )
		if(entries_[i]==name) {
			index = guint(i);
			break;
			}
	bool was = syncing_;
	syncing_ = true;
	presets_.set_selected(index);
	syncing_ = was;
	delete_.set_sensitive(state_.on && !hit.empty());
	}

void WiimPanel::choose_preset()
	{
	guint i = presets_.get_selected();
	if(i==0 || i>=entries_.size()) return;
	std::string name = entries_[i];
	// The device's presets end where the saved ones begin.
	if(i>device_presets_.size()) {
		auto curve = settings_.wiim_slots[device_id_][name];
		// A saved curve also engages the equaliser, as loading a device
		// preset does: a curve on a switched-off equaliser does nothing.
		run([curve](const std::string& a) {
			if(!wiimeq::set_bands(a, curve)) return false;
			auto st = wiimeq::state(a);
			return st && (st->on || wiimeq::set_on(a, true));
			}, "");
		}
	else
		run([name](const std::string& a) { return wiimeq::load_preset(a, name); }, name);
	}

void WiimPanel::commit_bands()
	{
	if(state_.bands.size()!=faders_.size()) return;
	std::vector<int> bands;
	for(auto f: faders_) bands.push_back(int(f->get_value()));
	if(bands==state_.bands) return;
	run([bands](const std::string& a) { return wiimeq::set_bands(a, bands); }, "");
	}

void WiimPanel::save_slot()
	{
	std::string name = save_name_.get_text();
	name.erase(0, name.find_first_not_of(" \t"));
	name.erase(name.find_last_not_of(" \t")+1);
	if(name.empty() || state_.bands.empty()) return;
	// A saved curve under a device preset's name would leave that preset
	// unreachable behind two identical entries.
	if(std::count(device_presets_.begin(), device_presets_.end(), name)) {
		status_.set_text("A device preset has that name");
		return;
		}
	settings_.wiim_slots[device_id_][name] = state_.bands;
	settings_.save();
	save_row_.set_visible(false);
	render();
	}

void WiimPanel::delete_slot()
	{
	std::string hit = matching_slot();
	if(hit.empty()) return;
	auto& slots = settings_.wiim_slots[device_id_];
	slots.erase(hit);
	if(slots.empty()) settings_.wiim_slots.erase(device_id_);
	settings_.save();
	render();
	}

void WiimPanel::set_narrow(bool narrow)
	{
	// Shorter faders, and no minimum width, so the card fits a narrow
	// window, as the web client's does.
	for(auto f: faders_) f->set_size_request(-1, narrow ? 88 : 160);
	bands_.set_size_request(narrow ? -1 : 460, -1);
	}

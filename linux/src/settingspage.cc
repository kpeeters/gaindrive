#include "settingspage.hh"
#include "clients.hh"
#include "serverconfig.hh"
#include "settings.hh"

#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/image.h>
#include <gtkmm/listbox.h>
#include <gtkmm/stringlist.h>
#include <gtkmm/switch.h>

SettingsPage::SettingsPage(AppSettings& settings, const std::vector<ServerConfig>& servers,
                           const Clients& clients)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  settings_(settings),
	  servers_(servers),
	  clients_(clients),
	  header_(Gtk::Orientation::HORIZONTAL),
	  content_(Gtk::Orientation::VERTICAL)
	{
	add_css_class("pane");
	title_.set_xalign(0);
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
	content_.add_css_class("settings-content");
	scroll_.set_child(content_);
	scroll_.set_vexpand(true);
	scroll_.set_policy(Gtk::PolicyType::NEVER, Gtk::PolicyType::AUTOMATIC);
	append(scroll_);
	}

void SettingsPage::set_back(const std::string& label)
	{
	back_.set_label("← " + label);
	back_.set_visible(!label.empty());
	}

void SettingsPage::clear()
	{
	title_.set_text("");
	while(auto child = content_.get_first_child())
		content_.remove(*child);
	}

void SettingsPage::show(const std::string& key)
	{
	clear();
	if(key=="playback")        show_playback();
	else if(key=="appearance") show_appearance();
	else if(key=="servers")    show_servers();
	else                       show_about();
	}

Gtk::ListBox* SettingsPage::add_group(const char *title)
	{
	if(title) {
		auto l = Gtk::make_managed<Gtk::Label>(title);
		l->set_xalign(0);
		l->add_css_class("settings-group-title");
		content_.append(*l);
		}
	auto list = Gtk::make_managed<Gtk::ListBox>();
	list->set_selection_mode(Gtk::SelectionMode::NONE);
	list->set_show_separators(true);
	list->add_css_class("settings-group");
	content_.append(*list);
	return list;
	}

Gtk::Box* SettingsPage::add_row(Gtk::ListBox *group, const std::string& title,
                                const std::string& subtitle, Gtk::Widget *suffix)
	{
	auto box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
	auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
	text->set_hexpand(true);
	text->set_valign(Gtk::Align::CENTER);
	auto t = Gtk::make_managed<Gtk::Label>(title);
	t->set_xalign(0);
	t->set_wrap(true);
	text->append(*t);
	if(!subtitle.empty()) {
		auto s = Gtk::make_managed<Gtk::Label>(subtitle);
		s->set_xalign(0);
		s->set_wrap(true);
		s->add_css_class("dim-label");
		s->add_css_class("settings-subtitle");
		text->append(*s);
		}
	box->append(*text);
	if(suffix) {
		suffix->set_valign(Gtk::Align::CENTER);
		box->append(*suffix);
		}
	group->append(*box);
	return box;
	}

void SettingsPage::add_switch_row(Gtk::ListBox *group, const std::string& title,
                                  const std::string& subtitle, bool on,
                                  std::function<void(bool)> changed)
	{
	auto sw = Gtk::make_managed<Gtk::Switch>();
	sw->set_active(on);
	// A click anywhere on the row flips it, as on GNOME's own.
	auto row = dynamic_cast<Gtk::ListBoxRow*>(add_row(group, title, subtitle, sw)->get_parent());
	if(row) {
		row->set_activatable(true);
		group->signal_row_activated().connect([sw, row](Gtk::ListBoxRow *r) {
			// activate() rather than set_active(): only that slides the knob.
			if(r==row) sw->activate();
			});
		}
	sw->property_active().signal_changed().connect([sw, changed]() { changed(sw->get_active()); });
	}

void SettingsPage::add_hint(const std::string& text)
	{
	auto l = Gtk::make_managed<Gtk::Label>(text);
	l->set_xalign(0);
	l->set_wrap(true);
	l->add_css_class("dim-label");
	l->add_css_class("settings-hint");
	content_.append(*l);
	}

void SettingsPage::show_playback()
	{
	title_.set_text("Playback");

	std::vector<Glib::ustring> caps = {"Original"};
	for(int k: quality_caps) caps.push_back("Up to " + std::to_string(k) + " kbps");
	auto quality = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(caps));
	guint selected = 0;
	for(size_t i=0; i<quality_caps.size(); ++i)
		if(quality_caps[i]==settings_.quality_cap) selected = guint(i+1);
	quality->set_selected(selected);

	std::vector<Glib::ustring> fmts;
	guint fmt_selected = 0;
	for(size_t i=0; i<transcode_formats.size(); ++i) {
		fmts.push_back(transcode_formats[i].label);
		if(settings_.format==transcode_formats[i].param) fmt_selected = guint(i);
		}
	auto format = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(fmts));
	format->set_selected(fmt_selected);

	auto group = add_group("Streaming");
	add_row(group, "Quality", "", quality);
	auto format_row = add_row(group, "Convert to", "", format)->get_parent();
	format_row->set_visible(settings_.quality_cap!=0);
	add_hint("A file that fits under the limit is sent as it is; anything else is "
	         "converted. Takes effect on the next track.");

	auto cast = add_group("Casting");
	add_switch_row(cast, "Cast at original quality",
	               "A receiver fetches from the server itself, so the limit above need not "
	               "apply to it. A file it cannot play is sent as FLAC either way.",
	               settings_.cast_original, [this](bool on) {
		settings_.cast_original = on;
		settings_.save();
		});
	add_switch_row(cast, "Keep video here when casting to a speaker",
	               "A speaker or amplifier that cannot show a picture is sent the soundtrack. "
	               "With this on the film also plays here, silent and kept in step with it, "
	               "which means streaming it to this computer as well. Takes effect on the "
	               "next film.",
	               settings_.cast_local_video, [this](bool on) {
		settings_.cast_local_video = on;
		settings_.save();
		});

	quality->property_selected().signal_changed().connect([this, quality, format_row]() {
		guint i = quality->get_selected();
		settings_.quality_cap = (i==0 || i>quality_caps.size()) ? 0 : quality_caps[i-1];
		settings_.save();
		format_row->set_visible(settings_.quality_cap!=0);
		signal_changed.emit();
		});
	format->property_selected().signal_changed().connect([this, format]() {
		guint i = format->get_selected();
		if(i<transcode_formats.size()) settings_.format = transcode_formats[i].param;
		settings_.save();
		signal_changed.emit();
		});
	}

void SettingsPage::show_appearance()
	{
	title_.set_text("Appearance");

	std::vector<Glib::ustring> theme_labels;
	guint theme_selected = 0;
	for(size_t i=0; i<themes.size(); ++i) {
		theme_labels.push_back(themes[i].label);
		if(settings_.theme==themes[i].param) theme_selected = guint(i);
		}
	auto theme = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(theme_labels));
	theme->set_selected(theme_selected);

	std::vector<Glib::ustring> size_labels;
	guint size_selected = 0;
	for(size_t i=0; i<subtitle_sizes.size(); ++i) {
		size_labels.push_back(subtitle_sizes[i].label);
		if(settings_.subtitle_size==subtitle_sizes[i].param) size_selected = guint(i);
		}
	auto size = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(size_labels));
	size->set_selected(size_selected);

	auto group = add_group(nullptr);
	add_row(group, "Theme", "Auto leaves it to the desktop's GTK settings.", theme);
	add_row(group, "Subtitle size", "Relative to the height of the picture, so subtitles "
	        "grow with the window and in fullscreen.", size);

	theme->property_selected().signal_changed().connect([this, theme]() {
		guint i = theme->get_selected();
		if(i<themes.size()) settings_.theme = themes[i].param;
		settings_.save();
		signal_theme.emit();
		signal_changed.emit();
		});
	size->property_selected().signal_changed().connect([this, size]() {
		guint i = size->get_selected();
		if(i<subtitle_sizes.size()) settings_.subtitle_size = subtitle_sizes[i].param;
		settings_.save();
		});
	}

void SettingsPage::show_servers()
	{
	title_.set_text("Servers");
	auto group = add_group("Servers");
	for(const auto& s: servers_) {
		bool active = clients_.get(s.key)!=nullptr;
		auto box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
		auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 2);
		text->set_hexpand(true);
		// The name reads as the row's title, and is edited in place. Saved
		// when the field is left or Enter pressed, not per letter: each save
		// reloads the lists that show the name.
		auto name = Gtk::make_managed<Gtk::Entry>();
		name->add_css_class("name-entry");
		name->set_text(s.name);
		name->set_placeholder_text(server_host(s.url));
		name->set_tooltip_text("The name album rows and the server picker show");
		auto saved = std::make_shared<std::string>(s.name);
		auto commit = [this, name, saved, key=s.key]() {
			std::string t = name->get_text();
			if(t==*saved) return;
			*saved = t;
			signal_rename_server.emit(key, t);
			};
		name->signal_activate().connect(commit);
		auto focus = Gtk::EventControllerFocus::create();
		focus->signal_leave().connect(commit);
		name->add_controller(focus);
		auto url = Gtk::make_managed<Gtk::Label>(s.url);
		url->set_xalign(0);
		url->set_ellipsize(Pango::EllipsizeMode::END);
		url->add_css_class("settings-subtitle");
		// The user ellipsized, not the state: Bandcamp's user is a long key.
		auto who = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 0);
		auto user = Gtk::make_managed<Gtk::Label>(s.user);
		user->set_xalign(0);
		user->set_ellipsize(Pango::EllipsizeMode::MIDDLE);
		user->add_css_class("dim-label");
		user->add_css_class("settings-subtitle");
		auto state = Gtk::make_managed<Gtk::Label>(active ? " · connected" : " · login refused");
		state->add_css_class("settings-subtitle");
		state->add_css_class(active ? "dim-label" : "error");
		who->append(*user);
		who->append(*state);
		text->append(*name);
		text->append(*url);
		text->append(*who);
		box->append(*text);
		std::string key = s.key;
		if(!active) {
			auto login = Gtk::make_managed<Gtk::Button>("Log in again");
			login->set_valign(Gtk::Align::CENTER);
			login->signal_clicked().connect([this, key]() { signal_login_server.emit(key); });
			box->append(*login);
			}
		auto remove = Gtk::make_managed<Gtk::Button>();
		remove->set_icon_name("gd-delete-symbolic");
		remove->set_tooltip_text("Remove server");
		remove->add_css_class("flat");
		remove->add_css_class("remove-server");
		remove->set_valign(Gtk::Align::CENTER);
		remove->signal_clicked().connect([this, key]() { signal_remove_server.emit(key); });
		box->append(*remove);
		group->append(*box);
		}
	// The last row adds one, as GNOME's lists do; no floating button.
	auto add = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
	auto plus = Gtk::make_managed<Gtk::Image>();
	plus->set_from_icon_name("gd-add-symbolic");
	add->append(*plus);
	add->append(*Gtk::make_managed<Gtk::Label>("Add server"));
	group->append(*add);
	if(auto row = dynamic_cast<Gtk::ListBoxRow*>(add->get_parent())) {
		row->set_activatable(true);
		}
	group->signal_row_activated().connect([this, add](Gtk::ListBoxRow *row) {
		if(row==add->get_parent()) signal_add_server.emit();
		});
	add_hint("Removing a server forgets its login.");

	auto library = add_group("Library");
	add_switch_row(library, "Show an album once",
	               "When several servers have it, the copy on the first server is opened.",
	               settings_.merge_albums, [this](bool on) {
		settings_.merge_albums = on;
		settings_.save();
		signal_merge.emit();
		});
	}

void SettingsPage::show_about()
	{
	title_.set_text("About");
	auto group = add_group(nullptr);
	add_row(group, "GainDrive " GAINDRIVE_VERSION,
	        "Free software under the GNU GPL, version 3 or later.", nullptr);
	}

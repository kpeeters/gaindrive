#include "settingspage.hh"
#include "clients.hh"
#include "serverconfig.hh"
#include "settings.hh"

#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/entry.h>
#include <gtkmm/eventcontrollerfocus.h>
#include <gtkmm/stringlist.h>

SettingsPage::SettingsPage(AppSettings& settings, const std::vector<ServerConfig>& servers,
                           const Clients& clients)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  settings_(settings),
	  servers_(servers),
	  clients_(clients),
	  header_(Gtk::Orientation::HORIZONTAL),
	  content_(Gtk::Orientation::VERTICAL, 8)
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
	content_.set_margin_start(12);
	content_.set_margin_end(12);
	append(content_);
	}

void SettingsPage::set_back(const std::string& label)
	{
	back_.set_label("\u2190 " + label);
	back_.set_visible(!label.empty());
	}

void SettingsPage::clear()
	{
	title_.set_text("");
	while(auto child = content_.get_first_child())
		content_.remove(*child);
	}

Gtk::Label& SettingsPage::add_label(const std::string& text, const char *css)
	{
	auto l = Gtk::make_managed<Gtk::Label>(text);
	l->set_xalign(0);
	l->set_wrap(true);
	if(css) l->add_css_class(css);
	content_.append(*l);
	return *l;
	}

void SettingsPage::show(const std::string& key)
	{
	clear();
	if(key=="playback")        show_playback();
	else if(key=="appearance") show_appearance();
	else if(key=="servers")    show_servers();
	else                       show_about();
	}

Gtk::Box* SettingsPage::add_row(const char *label, Gtk::Widget& w)
	{
	auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
	auto l = Gtk::make_managed<Gtk::Label>(label);
	l->set_xalign(0);
	l->set_hexpand(true);
	box->append(*l);
	box->append(w);
	content_.append(*box);
	return box;
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

	add_row("Quality", *quality);
	auto format_row = add_row("Convert to", *format);
	format_row->set_visible(settings_.quality_cap!=0);
	add_label("A file that fits under the limit is sent as it is; anything else is "
	          "converted. Takes effect on the next track.", "dim-label");

	auto cast_orig = Gtk::make_managed<Gtk::CheckButton>("Cast at original quality");
	cast_orig->set_active(settings_.cast_original);
	content_.append(*cast_orig);
	add_label("A receiver fetches from the server itself, so the limit above need not "
	          "apply to it. A file it cannot play is sent as FLAC either way.", "dim-label");
	cast_orig->signal_toggled().connect([this, cast_orig]() {
		settings_.cast_original = cast_orig->get_active();
		settings_.save();
		});

	auto local_video = Gtk::make_managed<Gtk::CheckButton>("Keep video here when casting to a speaker");
	local_video->set_active(settings_.cast_local_video);
	content_.append(*local_video);
	add_label("A speaker or amplifier that cannot show a picture is sent the soundtrack. "
	          "With this on the film also plays here, silent and kept in step with it, "
	          "which means streaming it to this computer as well. Takes effect on the "
	          "next film.", "dim-label");
	local_video->signal_toggled().connect([this, local_video]() {
		settings_.cast_local_video = local_video->get_active();
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
	add_row("Theme", *theme);
	add_label("Auto leaves it to the desktop's GTK settings.", "dim-label");

	std::vector<Glib::ustring> size_labels;
	guint size_selected = 0;
	for(size_t i=0; i<subtitle_sizes.size(); ++i) {
		size_labels.push_back(subtitle_sizes[i].label);
		if(settings_.subtitle_size==subtitle_sizes[i].param) size_selected = guint(i);
		}
	auto size = Gtk::make_managed<Gtk::DropDown>(Gtk::StringList::create(size_labels));
	size->set_selected(size_selected);
	add_row("Subtitle size", *size);
	add_label("Relative to the height of the picture, so subtitles grow with the "
	          "window and in fullscreen.", "dim-label");

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
	for(const auto& s: servers_) {
		auto box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
		auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
		text->set_hexpand(true);
		auto url  = Gtk::make_managed<Gtk::Label>(s.url);
		url->set_xalign(0);
		url->set_ellipsize(Pango::EllipsizeMode::END);
		bool active = clients_.get(s.key)!=nullptr;
		auto user = Gtk::make_managed<Gtk::Label>(s.user + (active ? " · connected" : " · login refused"));
		user->set_xalign(0);
		user->add_css_class("dim-label");
		// Saved when the field is left or Enter pressed, not per letter:
		// each save reloads the lists that show the name.
		auto name = Gtk::make_managed<Gtk::Entry>();
		name->set_text(s.name);
		name->set_placeholder_text(server_host(s.url));
		name->set_tooltip_text("The name album rows and the server picker show");
		auto saved = std::make_shared<std::string>(s.name);
		auto commit = [this, name, saved, key=s.key]() {
			std::string text = name->get_text();
			if(text==*saved) return;
			*saved = text;
			signal_rename_server.emit(key, text);
			};
		name->signal_activate().connect(commit);
		auto focus = Gtk::EventControllerFocus::create();
		focus->signal_leave().connect(commit);
		name->add_controller(focus);
		text->append(*name);
		text->append(*url);
		text->append(*user);
		auto remove = Gtk::make_managed<Gtk::Button>("Remove");
		remove->set_valign(Gtk::Align::CENTER);
		remove->add_css_class("destructive-action");
		std::string key = s.key;
		remove->signal_clicked().connect([this, key]() { signal_remove_server.emit(key); });
		box->append(*text);
		if(!active) {
			auto login = Gtk::make_managed<Gtk::Button>("Log in again");
			login->set_valign(Gtk::Align::CENTER);
			login->signal_clicked().connect([this, key]() { signal_login_server.emit(key); });
			box->append(*login);
			}
		box->append(*remove);
		content_.append(*box);
		}
	auto add = Gtk::make_managed<Gtk::Button>("Add server");
	add->set_halign(Gtk::Align::START);
	add->signal_clicked().connect([this]() { signal_add_server.emit(); });
	content_.append(*add);
	add_label("Removing a server forgets its login. Log in again to add it back.", "dim-label");

	auto merge = Gtk::make_managed<Gtk::CheckButton>("Show an album once when several servers have it");
	merge->set_active(settings_.merge_albums);
	merge->signal_toggled().connect([this, merge]() {
		settings_.merge_albums = merge->get_active();
		settings_.save();
		signal_merge.emit();
		});
	content_.append(*merge);
	add_label("The copy on the server listed first is the one opened.", "dim-label");
	}

void SettingsPage::show_about()
	{
	title_.set_text("About");
	add_label("GainDrive " GAINDRIVE_VERSION, "heading");
	add_label("Free software under the GNU GPL, version 3 or later.", "dim-label");
	}

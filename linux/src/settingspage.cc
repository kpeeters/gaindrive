#include "settingspage.hh"
#include "apiclient.hh"
#include "serverconfig.hh"
#include "settings.hh"

#include <gtkmm/button.h>
#include <gtkmm/checkbutton.h>
#include <gtkmm/stringlist.h>

SettingsPage::SettingsPage(AppSettings& settings, const std::vector<ServerConfig>& servers)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  settings_(settings),
	  servers_(servers),
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

void SettingsPage::show(const std::string& key, std::shared_ptr<ApiClient> api)
	{
	clear();
	if(key=="playback")        show_playback();
	else if(key=="appearance") show_appearance();
	else if(key=="servers")    show_servers(api);
	else                       show_about(api);
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

void SettingsPage::show_servers(std::shared_ptr<ApiClient> api)
	{
	title_.set_text("Servers");
	for(const auto& s: servers_) {
		auto box  = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
		auto text = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
		text->set_hexpand(true);
		auto url  = Gtk::make_managed<Gtk::Label>(s.url);
		url->set_xalign(0);
		url->set_ellipsize(Pango::EllipsizeMode::END);
		bool active = api && api->session().server_key==s.key;
		auto user = Gtk::make_managed<Gtk::Label>(s.user + (active ? " · connected" : ""));
		user->set_xalign(0);
		user->add_css_class("dim-label");
		text->append(*url);
		text->append(*user);
		auto remove = Gtk::make_managed<Gtk::Button>("Remove");
		remove->set_valign(Gtk::Align::CENTER);
		remove->add_css_class("destructive-action");
		std::string key = s.key;
		remove->signal_clicked().connect([this, key]() { signal_remove_server.emit(key); });
		box->append(*text);
		box->append(*remove);
		content_.append(*box);
		}
	add_label("Removing a server forgets its login. Log in again to add it back.", "dim-label");
	}

void SettingsPage::show_about(std::shared_ptr<ApiClient> api)
	{
	title_.set_text("About");
	add_label("GainDrive " GAINDRIVE_VERSION, "heading");
	if(api) {
		add_label("Server: " + api->session().base_url);
		add_label("Logged in as " + api->session().user);
		}
	add_label("Free software under the GNU GPL, version 3 or later.", "dim-label");
	}

#include "settingspage.hh"
#include "apiclient.hh"
#include "settings.hh"

#include <gtkmm/stringlist.h>

SettingsPage::SettingsPage(AppSettings& settings)
	: Gtk::Box(Gtk::Orientation::VERTICAL),
	  settings_(settings),
	  header_(Gtk::Orientation::HORIZONTAL),
	  content_(Gtk::Orientation::VERTICAL, 8)
	{
	add_css_class("pane");
	title_.set_xalign(0);
	title_.add_css_class("pane-title");
	header_.add_css_class("pane-header");
	header_.append(title_);
	append(header_);
	content_.set_margin_start(12);
	content_.set_margin_end(12);
	append(content_);
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
	if(key=="playback") show_playback();
	else                show_about(api);
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

	auto row = [this](const char *label, Gtk::Widget& w) {
		auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::HORIZONTAL, 12);
		auto l = Gtk::make_managed<Gtk::Label>(label);
		l->set_xalign(0);
		l->set_hexpand(true);
		box->append(*l);
		box->append(w);
		content_.append(*box);
		return box;
		};
	row("Quality", *quality);
	auto format_row = row("Convert to", *format);
	format_row->set_visible(settings_.quality_cap!=0);
	add_label("A file that fits under the limit is sent as it is; anything else is "
	          "converted. Takes effect on the next track.", "dim-label");

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

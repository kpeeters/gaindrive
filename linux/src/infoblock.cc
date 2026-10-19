#include "infoblock.hh"
#include "apiclient.hh"
#include "coverstore.hh"
#include "jsonread.hh"

#include <glibmm/main.h>

namespace {
	const unsigned poll_seconds = 15;
	const unsigned max_tries    = 40;
	const int      clamp_lines  = 6;
}

InfoBlock::InfoBlock(CoverStore& covers, int portrait_px)
	: Gtk::Box(Gtk::Orientation::HORIZONTAL, 12),
	  body_(Gtk::Orientation::VERTICAL, 4),
	  links_(Gtk::Orientation::HORIZONTAL, 0),
	  wiki_("", "Wikipedia"),
	  allmusic_("", "AllMusic"),
	  more_("more")
	{
	add_css_class("info-block");
	if(portrait_px>0) {
		portrait_ = std::make_unique<CoverImage>(covers, portrait_px);
		portrait_->set_valign(Gtk::Align::START);
		append(*portrait_);
		}
	text_.set_wrap(true);
	text_.set_xalign(0);
	text_.set_selectable(true);
	// Out of the focus chain: a selectable label selects all of itself when
	// focus lands on it, as it did when a sliding strip hid the pane that
	// had focus. Still selectable with the mouse.
	text_.set_focusable(false);
	text_.set_lines(clamp_lines);
	text_.set_ellipsize(Pango::EllipsizeMode::END);
	body_.append(text_);
	more_.add_css_class("flat");
	more_.signal_clicked().connect([this]() {
		bool expand = text_.get_lines()>0;
		text_.set_lines(expand ? -1 : clamp_lines);
		text_.set_ellipsize(expand ? Pango::EllipsizeMode::NONE : Pango::EllipsizeMode::END);
		more_.set_label(expand ? "less" : "more");
		});
	links_.append(wiki_);
	links_.append(allmusic_);
	links_.append(more_);
	body_.append(links_);
	pending_.set_text("Looking this up…");
	pending_.set_xalign(0);
	pending_.add_css_class("dim-label");
	body_.append(pending_);
	body_.set_hexpand(true);
	append(body_);
	clear();
	}

InfoBlock::~InfoBlock()
	{
	timer_.disconnect();
	}

void InfoBlock::clear()
	{
	++gen_;
	timer_.disconnect();
	api_.reset();
	set_visible(false);
	if(portrait_) portrait_->show_cover("");
	}

void InfoBlock::load(std::shared_ptr<ApiClient> api, const std::string& endpoint,
                     const std::string& id, const std::string& answer,
                     const std::string& text_key, const std::string& portrait_id)
	{
	clear();
	api_      = std::move(api);
	endpoint_ = endpoint;
	id_       = id;
	answer_   = answer;
	text_key_ = text_key;
	tries_    = 0;
	if(portrait_) portrait_->show_cover(portrait_id);
	request();
	}

void InfoBlock::request()
	{
	unsigned gen = gen_;
	api_->call(endpoint_, {{"id", id_}}, [this, gen](const ApiResult& r) {
		if(gen!=gen_) return;
		// Older servers have no such call; the block then stays away.
		if(!r.ok) return;
		const auto& info = jsub(r.body, answer_);
		std::string text   = jstr(info, text_key_);
		std::string wiki   = jstr(info, "wikiUrl");
		std::string am     = jstr(info, "allMusicUrl");
		const auto& res    = jsub(info, "resolving");
		bool resolving     = res.is_boolean() && res.get<bool>() && tries_<max_tries;
		bool nothing       = text.empty() && wiki.empty() && am.empty();

		if(!(tries_>0 && resolving && nothing)) {
			text_.set_text(text);
			text_.set_visible(!text.empty());
			text_.set_lines(clamp_lines);
			text_.set_ellipsize(Pango::EllipsizeMode::END);
			more_.set_label("more");
			// A guess at whether six lines hold it; a label cannot say.
			more_.set_visible(text.size()>400);
			wiki_.set_uri(wiki);
			wiki_.set_visible(!wiki.empty());
			allmusic_.set_uri(am);
			allmusic_.set_visible(!am.empty());
			links_.set_visible(!wiki.empty() || !am.empty() || more_.get_visible());
			pending_.set_visible(resolving);
			set_visible(portrait_ || !nothing || resolving);
			}
		if(resolving) {
			timer_ = Glib::signal_timeout().connect_seconds([this]() {
				++tries_;
				request();
				return false;
				}, poll_seconds);
			}
		});
	}

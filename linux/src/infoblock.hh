#pragma once

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/label.h>
#include <gtkmm/linkbutton.h>

#include <memory>
#include <string>

class ApiClient;
class CoverImage;
class CoverStore;

// An artist's biography or an album's notes, with Wikipedia and AllMusic
// links, from getArtistInfo2 or getAlbumInfo2. The server looks these up in
// the background and says `resolving` meanwhile, so the block asks again
// every 15 seconds for up to ten minutes, as the web client does.
class InfoBlock : public Gtk::Box {
	public:
		// With portrait_px>0, a picture of `portrait_id` beside the text.
		InfoBlock(CoverStore& covers, int portrait_px);
		~InfoBlock() override;

		// `answer` is the response's key, `text_key` the field holding the text.
		void load(std::shared_ptr<ApiClient> api, const std::string& endpoint,
		          const std::string& id, const std::string& answer,
		          const std::string& text_key, const std::string& portrait_id);
		void clear();

	private:
		void request();

		std::unique_ptr<CoverImage> portrait_;
		Gtk::Box                    body_, links_;
		Gtk::Label                  text_, pending_;
		Gtk::LinkButton             wiki_, allmusic_;
		Gtk::Button                 more_;

		std::shared_ptr<ApiClient>  api_;
		std::string                 endpoint_, id_, answer_, text_key_;
		unsigned                    gen_=0, tries_=0;
		sigc::connection            timer_;
};

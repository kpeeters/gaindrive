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

// Its child at a set height, clipped, or at the child's own height for the
// width given. A ScrolledWindow clips too, but asks its child's height with
// no width, which for wrapped text is the wrong number of lines.
class HeightClip : public Gtk::Widget {
	public:
		explicit HeightClip(Gtk::Widget& child);
		~HeightClip() override;

		// -1 follows the child again.
		void set_height(int height);

	protected:
		Gtk::SizeRequestMode get_request_mode_vfunc() const override;
		void measure_vfunc(Gtk::Orientation orientation, int for_size, int& minimum, int& natural,
		                   int& minimum_baseline, int& natural_baseline) const override;
		void size_allocate_vfunc(int width, int height, int baseline) override;

	private:
		Gtk::Widget& child_;
		int          height_=-1;
};

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
		void set_clamped(bool clamped);
		void animate(int from, int to);

		std::unique_ptr<CoverImage> portrait_;
		Gtk::Box                    body_, links_;
		Gtk::Label                  text_, pending_;
		// Clips the text to a height eased from clamped to full and back,
		// which a label's own line limit would change at once.
		HeightClip                  clip_;
		guint                       tick_=0;
		gint64                      start_=0;
		int                         from_=0, to_=0;
		Gtk::LinkButton             wiki_, allmusic_;
		Gtk::Button                 more_;

		std::shared_ptr<ApiClient>  api_;
		std::string                 endpoint_, id_, answer_, text_key_;
		unsigned                    gen_=0, tries_=0;
		sigc::connection            timer_;
};

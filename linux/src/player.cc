#include "player.hh"
#include "stamp.hh"

#include <gst/gst.h>
#include <gtk/gtk.h>

#include <algorithm>
#include <cmath>
#include <iostream>

namespace {
	// GstPlayFlags lives in a plugin header, not the core API.
	const guint flag_video       = 1<<0;
	const guint flag_audio       = 1<<1;
	const guint flag_soft_volume = 1<<4;
	const guint flag_buffering   = 1<<8;
	const guint flag_deinterlace = 1<<9;

	// gtk4paintablesink, inside a glsinkbin when GTK draws with GL, which
	// is how that sink's own examples set it up: the GL upload then happens
	// in GStreamer's GL context rather than as a copy in ours.
	GstElement *make_video_sink(GdkPaintable *& paintable)
		{
		GstElement *sink = gst_element_factory_make("gtk4paintablesink", nullptr);
		if(!sink) {
			std::cout << stamp() << "GStreamer has no gtk4paintablesink: films will play without picture" << std::endl;
			return nullptr;
			}
		g_object_get(sink, "paintable", &paintable, nullptr);
		GdkGLContext *gl = nullptr;
		if(g_object_class_find_property(G_OBJECT_GET_CLASS(paintable), "gl-context"))
			g_object_get(paintable, "gl-context", &gl, nullptr);
		if(!gl) return sink;
		g_object_unref(gl);
		GstElement *bin = gst_element_factory_make("glsinkbin", nullptr);
		if(!bin) {
			std::cout << stamp() << "GStreamer has no glsinkbin; video without GL upload" << std::endl;
			return sink;
			}
		g_object_set(bin, "sink", sink, nullptr);
		return bin;
		}

	// The web client's Q for its peaking bands.
	const double eq_q = 1.41;

	// equalizer-nbands then a volume for headroom, as one element for
	// playbin's audio-filter. Null, with nothing changed, when either is
	// missing: music then plays without an equaliser.
	GstElement *make_eq(GstElement *& eq, GstElement *& preamp)
		{
		eq     = gst_element_factory_make("equalizer-nbands", nullptr);
		preamp = gst_element_factory_make("volume", nullptr);
		if(!eq || !preamp) {
			std::cout << stamp() << "GStreamer has no equalizer-nbands (gst-plugins-good): "
			          << "playing without an equaliser" << std::endl;
			if(eq)     gst_object_unref(eq);
			if(preamp) gst_object_unref(preamp);
			eq = preamp = nullptr;
			return nullptr;
			}
		g_object_set(eq, "num-bands", guint(eq_bands.size()), nullptr);
		for(guint i=0; i<eq_bands.size(); ++i) {
			GObject *band = gst_child_proxy_get_child_by_index(GST_CHILD_PROXY(eq), i);
			double f = eq_bands[i];
			const char *type = i==0 ? "low-shelf" : i+1==eq_bands.size() ? "high-shelf" : "peak";
			gst_util_set_object_arg(band, "type", type);
			g_object_set(band, "freq", f, "gain", 0.0, nullptr);
			if(std::string(type)=="peak")
				g_object_set(band, "bandwidth", f/eq_q, nullptr);
			g_object_unref(band);
			}
		GstElement *bin = gst_bin_new("eq");
		gst_bin_add_many(GST_BIN(bin), eq, preamp, nullptr);
		gst_element_link(eq, preamp);
		GstPad *sink = gst_element_get_static_pad(eq, "sink");
		GstPad *src  = gst_element_get_static_pad(preamp, "src");
		gst_element_add_pad(bin, gst_ghost_pad_new("sink", sink));
		gst_element_add_pad(bin, gst_ghost_pad_new("src", src));
		gst_object_unref(sink);
		gst_object_unref(src);
		return bin;
		}
}

const std::vector<int> eq_bands = { 32, 64, 125, 250, 500, 1000, 2000, 4000, 8000, 16000 };

Player::Player()
	{
	playbin_ = gst_element_factory_make("playbin3", "player");
	if(!playbin_) {
		std::cout << stamp() << "playbin3 is missing, falling back to playbin" << std::endl;
		playbin_ = gst_element_factory_make("playbin", "player");
		}
	if(!playbin_) {
		std::cout << stamp() << "GStreamer has no playbin: is gstreamer1.0-plugins-base installed?" << std::endl;
		return;
		}
	g_object_set(playbin_, "flags", flag_audio | flag_soft_volume | flag_buffering, nullptr);
	if(GstElement *vsink = make_video_sink(paintable_))
		g_object_set(playbin_, "video-sink", vsink, nullptr);
	// Always in place, flat when off, so switching it never relinks
	// anything under a playing stream.
	if(GstElement *eq = make_eq(eq_, preamp_))
		g_object_set(playbin_, "audio-filter", eq, nullptr);
	g_signal_connect(playbin_, "about-to-finish", G_CALLBACK(&Player::about_to_finish), this);
	g_signal_connect(playbin_, "source-setup", G_CALLBACK(&Player::source_setup), this);

	GstBus *bus = gst_element_get_bus(playbin_);
	watch_ = gst_bus_add_watch(bus, &Player::bus_message, this);
	gst_object_unref(bus);
	}

Player::~Player()
	{
	if(!playbin_) return;
	gst_element_set_state(playbin_, GST_STATE_NULL);
	g_source_remove(watch_);
	gst_object_unref(playbin_);
	if(paintable_) g_object_unref(paintable_);
	}

void Player::play_uri(const std::string& uri, double start, bool video)
	{
	if(!playbin_) {
		signal_error.emit("GStreamer's playbin is not installed.");
		return;
		}
	{
	std::lock_guard<std::mutex> lock(mtx_);
	next_uri_.clear();
	advance_pending_ = false;
	}
	gst_element_set_state(playbin_, GST_STATE_READY);
	// No text flag even for films: subtitles are drawn by the video view,
	// from the server's WebVTT, the same way whichever stream is playing.
	guint flags = flag_audio | flag_soft_volume | flag_buffering;
	if(video && paintable_) flags |= flag_video | flag_deinterlace;
	g_object_set(playbin_, "flags", flags, "uri", uri.c_str(), nullptr);
	target_playing_ = true;
	buffering_      = false;
	pending_seek_   = start;
	seek_target_    = start>0 ? start : -1;
	last_position_  = start;
	// To an offset by way of PAUSED, so nothing is heard from the beginning.
	gst_element_set_state(playbin_, start>0 ? GST_STATE_PAUSED : GST_STATE_PLAYING);
	}

void Player::set_next_uri(const std::string& uri)
	{
	std::lock_guard<std::mutex> lock(mtx_);
	next_uri_ = uri;
	}

void Player::pause()
	{
	if(!playbin_) return;
	target_playing_ = false;
	gst_element_set_state(playbin_, GST_STATE_PAUSED);
	}

void Player::resume()
	{
	if(!playbin_) return;
	target_playing_ = true;
	if(!buffering_)
		gst_element_set_state(playbin_, GST_STATE_PLAYING);
	}

void Player::stop()
	{
	if(!playbin_) return;
	{
	std::lock_guard<std::mutex> lock(mtx_);
	next_uri_.clear();
	advance_pending_ = false;
	}
	target_playing_ = false;
	pending_seek_   = 0;
	seek_target_    = -1;
	last_position_  = 0;
	gst_element_set_state(playbin_, GST_STATE_NULL);
	if(playing_) {
		playing_ = false;
		signal_state.emit(false);
		}
	}

void Player::seek(double seconds)
	{
	if(!playbin_) return;
	seek_target_ = seconds;
	if(!gst_element_seek_simple(playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		gint64(seconds*GST_SECOND))) {
		std::cout << stamp() << "GStreamer refused a seek to " << seconds << " s" << std::endl;
		seek_target_ = -1;
		}
	}

void Player::set_eq(bool on, const std::vector<int>& gains)
	{
	if(!eq_) return;
	// Headroom for the largest boost, so a boosted band cannot clip; cuts
	// need none.
	int boost = 0;
	for(size_t i=0; i<eq_bands.size(); ++i) {
		int g = on && i<gains.size() ? gains[i] : 0;
		boost = std::max(boost, g);
		GObject *band = gst_child_proxy_get_child_by_index(GST_CHILD_PROXY(eq_), guint(i));
		g_object_set(band, "gain", double(g), nullptr);
		g_object_unref(band);
		}
	g_object_set(preamp_, "volume", std::pow(10.0, -boost/20.0), nullptr);
	}

void Player::set_volume(double level)
	{
	if(playbin_) g_object_set(playbin_, "volume", std::clamp(level, 0.0, 1.0), nullptr);
	}

double Player::volume() const
	{
	double v = 1;
	if(playbin_) g_object_get(playbin_, "volume", &v, nullptr);
	return v;
	}

double Player::position() const
	{
	if(seek_target_>=0) return seek_target_;
	gint64 pos = 0;
	if(playbin_ && gst_element_query_position(playbin_, GST_FORMAT_TIME, &pos))
		last_position_ = double(pos)/GST_SECOND;
	return last_position_;
	}

GdkPaintable *Player::paintable() const
	{
	return paintable_;
	}

bool Player::playing() const
	{
	return playing_;
	}

bool Player::seekable() const
	{
	if(!playbin_) return false;
	gboolean seekable = FALSE;
	GstQuery *q = gst_query_new_seeking(GST_FORMAT_TIME);
	if(gst_element_query(playbin_, q))
		gst_query_parse_seeking(q, nullptr, &seekable, nullptr, nullptr);
	gst_query_unref(q);
	return seekable;
	}

void Player::about_to_finish(GstElement *playbin, gpointer self)
	{
	// On a streaming thread, and the URI must be set before returning for the
	// switch to be gapless, which is why the next URI is decided in advance.
	auto p = static_cast<Player *>(self);
	std::lock_guard<std::mutex> lock(p->mtx_);
	if(p->next_uri_.empty()) return;
	g_object_set(playbin, "uri", p->next_uri_.c_str(), nullptr);
	p->next_uri_.clear();
	p->advance_pending_ = true;
	}

void Player::source_setup(GstElement *, GstElement *source, gpointer)
	{
	// For the server's log, which otherwise shows GStreamer's own name.
	if(g_object_class_find_property(G_OBJECT_GET_CLASS(source), "user-agent"))
		g_object_set(source, "user-agent", "gaindrive-player/" GAINDRIVE_VERSION, nullptr);
	}

gboolean Player::bus_message(GstBus *, GstMessage *msg, gpointer self)
	{
	auto p = static_cast<Player *>(self);
	switch(GST_MESSAGE_TYPE(msg)) {
		case GST_MESSAGE_STREAM_START: {
			bool advanced = false;
			{
			std::lock_guard<std::mutex> lock(p->mtx_);
			advanced = p->advance_pending_;
			p->advance_pending_ = false;
			}
			if(advanced) p->signal_advanced.emit();
			break;
			}
		case GST_MESSAGE_EOS:
			p->target_playing_ = false;
			// So a press of play starts over rather than resuming at the end.
			p->last_position_  = 0;
			gst_element_set_state(p->playbin_, GST_STATE_READY);
			p->signal_ended.emit();
			break;
		case GST_MESSAGE_ERROR: {
			GError *err = nullptr;
			gchar *debug = nullptr;
			gst_message_parse_error(msg, &err, &debug);
			std::string text = err ? err->message : "unknown error";
			std::cout << stamp() << "GStreamer error: " << text
			          << (debug ? std::string(" (") + debug + ")" : "") << std::endl;
			g_clear_error(&err);
			g_free(debug);
			p->stop();
			p->signal_error.emit(text);
			break;
			}
		case GST_MESSAGE_ASYNC_DONE:
			// A flushing seek ends here; the next answer is the new place.
			p->seek_target_ = -1;
			if(p->pending_seek_>0) {
				double s = p->pending_seek_;
				p->pending_seek_ = 0;
				p->seek(s);
				if(p->target_playing_ && !p->buffering_)
					gst_element_set_state(p->playbin_, GST_STATE_PLAYING);
				}
			break;
		case GST_MESSAGE_BUFFERING: {
			// The usual dance for a network stream: hold at PAUSED until the
			// buffer is full, then go back to what the user asked for.
			gint percent = 0;
			gst_message_parse_buffering(msg, &percent);
			if(percent<100 && !p->buffering_) {
				p->buffering_ = true;
				if(p->target_playing_)
					gst_element_set_state(p->playbin_, GST_STATE_PAUSED);
				}
			else if(percent>=100 && p->buffering_) {
				p->buffering_ = false;
				// A start offset not yet applied keeps it at PAUSED.
				if(p->target_playing_ && p->pending_seek_==0)
					gst_element_set_state(p->playbin_, GST_STATE_PLAYING);
				}
			break;
			}
		case GST_MESSAGE_STATE_CHANGED: {
			if(GST_MESSAGE_SRC(msg)!=GST_OBJECT(p->playbin_)) break;
			GstState old_state, new_state, pending;
			gst_message_parse_state_changed(msg, &old_state, &new_state, &pending);
			bool now = new_state==GST_STATE_PLAYING;
			// While buffering, PAUSED is ours, not the user's: the button
			// should keep saying what they asked for.
			if(!now && p->buffering_ && p->target_playing_) break;
			if(now!=p->playing_) {
				p->playing_ = now;
				p->signal_state.emit(now);
				}
			break;
			}
		default:
			break;
		}
	return TRUE;
	}

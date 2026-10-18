#include "player.hh"
#include "stamp.hh"

#include <gst/gst.h>

#include <iostream>

namespace {
	// GstPlayFlags lives in a plugin header, not the core API.
	const guint flag_audio       = 1<<1;
	const guint flag_soft_volume = 1<<4;
	const guint flag_buffering   = 1<<8;
}

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
	// Audio only: a cover embedded in a file would otherwise surface as a
	// video stream, and playbin would open a window for it.
	g_object_set(playbin_, "flags", flag_audio | flag_soft_volume | flag_buffering, nullptr);
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
	}

void Player::play_uri(const std::string& uri, double start)
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
	g_object_set(playbin_, "uri", uri.c_str(), nullptr);
	target_playing_ = true;
	buffering_      = false;
	pending_seek_   = start;
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
	gst_element_set_state(playbin_, GST_STATE_NULL);
	if(playing_) {
		playing_ = false;
		signal_state.emit(false);
		}
	}

void Player::seek(double seconds)
	{
	if(!playbin_) return;
	gst_element_seek_simple(playbin_, GST_FORMAT_TIME,
		GstSeekFlags(GST_SEEK_FLAG_FLUSH | GST_SEEK_FLAG_KEY_UNIT),
		gint64(seconds*GST_SECOND));
	}

double Player::position() const
	{
	gint64 pos = 0;
	if(!playbin_ || !gst_element_query_position(playbin_, GST_FORMAT_TIME, &pos))
		return 0;
	return double(pos)/GST_SECOND;
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

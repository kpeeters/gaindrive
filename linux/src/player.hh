#pragma once

#include <sigc++/signal.h>

#include <mutex>
#include <string>

typedef struct _GstElement GstElement;
typedef struct _GstBus     GstBus;
typedef struct _GstMessage GstMessage;
typedef struct _GdkPaintable GdkPaintable;

// Local output: one playbin3, for music and films alike. Knows URIs, not songs;
// Playback decides what plays. All signals are emitted on the main loop.
// Created on the main thread, after GTK, which the video sink needs.
class Player {
	public:
		Player();
		~Player();

		// Starting `start` seconds in, for a resumed track. Pictures only
		// for a film: a cover embedded in a music file is a video stream
		// too.
		void play_uri(const std::string& uri, double start=0, bool video=false);
		// What to switch to, without a gap, when the current stream ends.
		// Empty for nothing.
		void set_next_uri(const std::string& uri);
		void pause();
		void resume();
		void stop();
		void seek(double seconds);

		double position() const;   // seconds into the current stream
		bool   playing() const;
		// False for a stream the server is converting as it sends: it has no
		// Range support, so the only way to move in it is a new request.
		bool   seekable() const;

		// What the films are drawn on; null when GStreamer has no
		// gtk4paintablesink, and then a film plays as sound only.
		GdkPaintable *paintable() const;

		// The stream given to set_next_uri has started.
		sigc::signal<void()>                   signal_advanced;
		// The last stream ended with nothing after it.
		sigc::signal<void()>                   signal_ended;
		sigc::signal<void(bool playing)>       signal_state;
		sigc::signal<void(const std::string&)> signal_error;

	private:
		static void     about_to_finish(GstElement *playbin, void *self);
		static void     source_setup(GstElement *playbin, GstElement *source, void *self);
		// gboolean and gpointer, spelled without glib's header.
		static int      bus_message(GstBus *bus, GstMessage *msg, void *self);

		GstElement   *playbin_=nullptr;
		GdkPaintable *paintable_=nullptr;
		unsigned    watch_=0;
		// Whether the user wants sound; buffering pauses underneath it.
		bool        target_playing_=false;
		bool        buffering_=false;
		bool        playing_=false;
		// A start offset, applied once the stream has prerolled.
		double      pending_seek_=0;
		// Where a seek is going, reported as the position until it lands:
		// meanwhile GStreamer answers nothing, or 0, and the seek bar would
		// jump to the start and back. -1 when no seek is under way.
		double      seek_target_=-1;
		// The last position GStreamer did answer, for the moments it
		// cannot, such as while buffering.
		mutable double last_position_=0;

		// Shared with about-to-finish, which runs on a streaming thread.
		std::mutex  mtx_;
		std::string next_uri_;
		bool        advance_pending_=false;
};

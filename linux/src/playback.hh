#pragma once

#include "caster.hh"
#include "model.hh"
#include "player.hh"

#include <sigc++/connection.h>

#include <memory>
#include <vector>

class ApiClient;
class Clients;
class WorkPool;
struct AppSettings;

// The queue and what plays from it, with the same semantics as the web
// client: playing a track loads its whole album, everything after it is an
// automatic tail, and enqueueing by hand replaces that tail. It plays here, or
// on a Cast receiver while one is connected.
class Playback {
	public:
		// Each track goes to its own server's client in `clients`.
		Playback(const AppSettings& settings, WorkPool& pool, const Clients& clients);
		~Playback();

		// A server is about to log out: its tracks leave the queue, and
		// playback stops if one of them is playing.
		void forget_server(const std::string& key);

		// `offset` seconds into the track, for a resumed one.
		void play(const std::vector<Song>& songs, size_t start, double offset=0);
		void enqueue(const Song& song);
		void jump(size_t index);
		void remove(size_t index);

		void toggle();
		void next();
		void prev();
		void seek(double seconds);
		void stop();

		const std::vector<Song>& queue() const;
		// -1 when nothing is loaded.
		long        index() const;
		const Song *current() const;
		bool        playing() const;
		double      position() const;

		// The local equaliser and volume; see Player.
		void        set_eq(bool on, const std::vector<int>& gains);
		void        set_volume(double level);
		double      volume() const;

		// What a film is drawn on; see Player::paintable().
		GdkPaintable *paintable() const;

		Caster&     caster();
		const AppSettings& settings() const { return settings_; }
		// Moves what is playing to a receiver, and back.
		void        cast_to(size_t device);
		void        stop_casting();
		// For the track info: what the receiver was given. `sent` is the
		// server's X-Gaindrive-Sent for a track, or for a film already in
		// words; `type` is the Content-Type the LOAD declared.
		const std::string& cast_sent() const;
		const std::string& cast_type() const;
		// The URL the local player reads the current track from.
		std::string local_url() const;

		// A film is being cast and the receiver shows its picture, so it
		// also draws the subtitles.
		bool        cast_picture() const;
		// A film is being cast to a receiver with no screen, and its
		// picture plays here, silent, following the receiver.
		bool        picture_local() const { return picture_; }
		// A video played as one, rather than as its soundtrack alone.
		bool        shows_picture(const Song& s) const;
		// The audio-only setting changed: a video playing now is reloaded
		// where it is, as the other kind.
		void        video_mode_changed();

		// The current film's subtitles: a 1-based track in getVideoInfo's
		// order, or 0 for none. Sent to a receiver showing the picture, and
		// declared in the next LOAD of this film.
		void        set_caption(int track);

		// Says "stopped" to the server, blocking; for when the window closes
		// and the pool is about to drop anything queued.
		void        report_stopped_now();

		// The current track, or the queue after it, changed.
		sigc::signal<void()>                   signal_track;
		// A track was started on purpose, rather than reached gaplessly.
		sigc::signal<void()>                   signal_started;
		sigc::signal<void(bool playing)>       signal_state;
		sigc::signal<void(const std::string&)> signal_error;
		// A seek within the current track, in seconds; for MPRIS's Seeked.
		sigc::signal<void(double)>             signal_seeked;
		// A playback report sent for `song`, at `pos` seconds: what the
		// server keeps the resume point and watched mark from.
		sigc::signal<void(const ItemRef& song, double pos)> signal_reported;

	private:
		void start(double offset=0);
		void local_start(double offset);
		void cast_start(double offset);
		void cast_ended();
		// Starts or stops the local picture of a cast film, whichever the
		// receiver and the settings now call for.
		void picture_follow(double offset);
		void picture_stop();
		bool picture_sync();
		// Moves the picture ahead of the receiver and holds it there until
		// the sound arrives.
		void picture_ahead(double target);
		void update_next();
		void advanced();
		void ended();
		bool tick();
		void report(const char *state, double pos);
		void scrobble(const Song& s);
		// Null once the song's server has logged out.
		std::shared_ptr<ApiClient> api_for(const Song& s) const;
		std::string stream_url(const Song& s, double offset=0, bool mp3=false) const;
		std::string video_url(const Song& s, bool hls) const;
		void        error(const std::string& e);

		const AppSettings&         settings_;
		WorkPool&                  pool_;
		Player                     player_;
		Caster                     caster_;
		// Bumped by each cast load, so a slow one that is overtaken is dropped.
		unsigned                   cast_gen_=0;
		// The receiver shows the film, as the server reported; otherwise the
		// subtitles are ours to draw.
		bool                       cast_screen_=false;
		std::string                cast_sent_, cast_type_;
		bool                       picture_=false;
		// Told to play, as opposed to held still for the receiver.
		bool                       picture_running_=false;
		// Sought ahead of the receiver, waiting for it to reach the frame.
		bool                       picture_holding_=false;
		// How far ahead, in seconds; grows when a seek outlasts it.
		double                     picture_lead_=0;
		// Ran out before the receiver did; it is not restarted under it.
		bool                       picture_ended_=false;
		// Already fetched as HLS after the stored file failed.
		bool                       picture_hls_=false;
		// The sink offset in force, in seconds.
		double                     picture_offset_=0;
		sigc::connection           picture_timer_;
		std::string                caption_song_;
		int                        caption_track_=0;
		const Clients&             clients_;
		std::vector<Song>          queue_;
		long                       index_=-1;
		size_t                     auto_from_=0;
		bool                       scrobbled_=false;
		// Where in the song the current stream begins: non-zero after a seek
		// in a stream that could not seek, which re-requests from that point.
		double                     local_offset_=0;
		// The one retry after an error has been spent on this track: as MP3
		// for music, as HLS for a film.
		bool                       mp3_retried_=false;
		// The queue entry handed to the player as the gapless successor, so a
		// queue edit can tell whether it has to be replaced.
		std::string                next_id_;
		unsigned                   ticks_=0;
		sigc::connection           timer_;
};

#pragma once

#include "caster.hh"
#include "model.hh"
#include "player.hh"

#include <sigc++/connection.h>

#include <memory>
#include <vector>

class ApiClient;
class WorkPool;
struct AppSettings;

// The queue and what plays from it, with the same semantics as the web
// client: playing a track loads its whole album, everything after it is an
// automatic tail, and enqueueing by hand replaces that tail. It plays here, or
// on a Cast receiver while one is connected.
class Playback {
	public:
		Playback(const AppSettings& settings, WorkPool& pool);
		~Playback();

		// Null on logout, which stops playback.
		void set_api(std::shared_ptr<ApiClient> api);

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
		// Moves what is playing to a receiver, and back.
		void        cast_to(size_t device);
		void        stop_casting();
		// A film is being cast and the receiver shows its picture, so it
		// also draws the subtitles.
		bool        cast_picture() const;

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

	private:
		void start(double offset=0);
		void local_start(double offset);
		void cast_start(double offset);
		void cast_film(double offset);
		void cast_ended();
		void update_next();
		void advanced();
		void ended();
		bool tick();
		void report(const char *state, double pos);
		std::string stream_url(const Song& s, double offset=0, bool mp3=false) const;
		std::string video_url(const Song& s, bool hls) const;
		void        error(const std::string& e);

		const AppSettings&         settings_;
		WorkPool&                  pool_;
		Player                     player_;
		Caster                     caster_;
		// Bumped by each cast load, so a slow one that is overtaken is dropped.
		unsigned                   cast_gen_=0;
		// The film on the receiver is its soundtrack alone, piped from a
		// timeOffset, so a seek reloads from there.
		bool                       cast_soundtrack_=false;
		std::string                caption_song_;
		int                        caption_track_=0;
		std::shared_ptr<ApiClient> api_;
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

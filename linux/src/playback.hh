#pragma once

#include "model.hh"
#include "player.hh"

#include <sigc++/connection.h>

#include <memory>
#include <vector>

class ApiClient;
struct AppSettings;

// The queue and what plays from it, with the same semantics as the web
// client: playing a track loads its whole album, everything after it is an
// automatic tail, and enqueueing by hand replaces that tail.
class Playback {
	public:
		explicit Playback(const AppSettings& settings);
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

		// Says "stopped" to the server, blocking; for when the window closes
		// and the pool is about to drop anything queued.
		void        report_stopped_now();

		// The current track, or the queue after it, changed.
		sigc::signal<void()>                   signal_track;
		sigc::signal<void(bool playing)>       signal_state;
		sigc::signal<void(const std::string&)> signal_error;

	private:
		void start(double offset=0);
		void update_next();
		void advanced();
		void ended();
		bool tick();
		void report(const char *state, double pos);
		std::string stream_url(const Song& s, double offset=0, bool mp3=false) const;
		void        error(const std::string& e);

		const AppSettings&         settings_;
		Player                     player_;
		std::shared_ptr<ApiClient> api_;
		std::vector<Song>          queue_;
		long                       index_=-1;
		size_t                     auto_from_=0;
		bool                       scrobbled_=false;
		// Where in the song the current stream begins: non-zero after a seek
		// in a stream that could not seek, which re-requests from that point.
		double                     local_offset_=0;
		// The one retry as MP3 after an error has been spent on this track.
		bool                       mp3_retried_=false;
		// The queue entry handed to the player as the gapless successor, so a
		// queue edit can tell whether it has to be replaced.
		std::string                next_id_;
		unsigned                   ticks_=0;
		sigc::connection           timer_;
};

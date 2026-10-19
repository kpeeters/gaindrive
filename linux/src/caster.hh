#pragma once

#include <nlohmann/json.hpp>
#include <sigc++/signal.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

class CastManager;
class WorkPool;

// A Cast session held by this client, on the server's CastManager: the
// receiver fetches from the server itself, and this side only sends it
// commands. Everything that blocks runs on a queue of its own, in order, so a
// pause cannot overtake the load it pauses; what the receiver reports comes
// back on the main loop.
class Caster {
	public:
		// CastManager::CastDevice, which cannot be forward declared.
		struct Device {
			std::string id, name, model, address;
			int         port=8009;
			int         capabilities=-1;
		};

		Caster();
		~Caster();

		// A few seconds of mDNS; signal_devices when done.
		void discover();
		bool discovering() const;
		const std::vector<Device>& devices() const;

		void connect(size_t index);
		void disconnect();
		bool active() const;
		// The receiver connected to, while active().
		const Device& device() const;
		const std::string& device_name() const;
		// A WiiM carries its own equaliser beside the Cast receiver.
		bool is_wiim() const;

		struct Load {
			std::string      url, mime;
			double           duration=0;
			// Where in the stream to begin, and where in the song the stream
			// itself begins: a soundtrack piped from a timeOffset starts
			// part way in. Positions here are always the song's.
			double           start=0, base=0;
			nlohmann::json   metadata;
			// Subtitle tracks, declared in every film LOAD, since a track the
			// LOAD did not declare cannot be turned on later.
			nlohmann::json   tracks=nlohmann::json::array();
			std::vector<int> active_tracks;
		};

		// A load is on its way: from here on, what the receiver says about the
		// media it is playing now is stale, its end included.
		void prepare(double start);
		void load(const Load& l);
		// Turns subtitles on (one trackId) or off (none) without a reload.
		void set_tracks(const std::vector<int>& track_ids);
		void pause();
		void resume();
		void seek(double seconds);

		bool   playing() const;
		// True once the current load has started and until it ends.
		bool   loaded() const;
		double position() const;

		// The receiver's volume: unknown until it first reports one, and
		// fixed when nothing may change it.
		bool  volume_known() const;
		bool  volume_fixed() const;
		float volume() const;
		void  volume_step(int percent);

		sigc::signal<void()>                   signal_devices;
		sigc::signal<void()>                   signal_session;
		sigc::signal<void(bool playing)>       signal_state;
		sigc::signal<void()>                   signal_ended;
		sigc::signal<void(const std::string&)> signal_error;
		sigc::signal<void()>                   signal_volume;

	private:
		struct Snapshot;
		void watch();
		void stop_watching();
		void received(const Snapshot& s);

		// By shared_ptr so that queued commands keep it alive; its own
		// destructor waits for the threads it started.
		std::shared_ptr<CastManager> cm_;
		std::unique_ptr<WorkPool>    queue_;
		// Apart from the commands, so a scan does not hold up a connect.
		std::unique_ptr<WorkPool>    scan_;
		std::vector<Device>          devices_;
		bool                         discovering_=false;
		bool                         active_=false;
		Device                       device_;

		// What the receiver last said, and when, to move the position on
		// between its reports.
		std::string                  state_="IDLE";
		double                       time_=0;
		std::chrono::steady_clock::time_point at_;
		bool                         started_=false;
		int                          session_=0, stale_session_=-1;
		int                          notice_seq_=0;
		// Where the current load should start; sent as a SEEK once it plays.
		double                       seek_to_=0;
		double                       base_=0;
		bool                         vol_known_=false, vol_fixed_=false;
		float                        vol_=0;

		std::atomic<bool>            watching_{false};
		std::thread                  watcher_;
};

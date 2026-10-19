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
		const std::string& device_name() const;

		// A load is on its way: from here on, what the receiver says about the
		// media it is playing now is stale, its end included.
		void prepare(double start);
		void load(const std::string& url, const std::string& mime, double duration,
		          double start, const nlohmann::json& metadata);
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
		std::string                  device_name_;

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
		bool                         vol_known_=false, vol_fixed_=false;
		float                        vol_=0;

		std::atomic<bool>            watching_{false};
		std::thread                  watcher_;
};

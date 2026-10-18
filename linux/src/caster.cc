#include "caster.hh"
#include "castmanager.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <algorithm>
#include <cmath>
#include <iostream>

struct Caster::Snapshot {
	std::string state, idle_reason;
	double      time=0;
	int         session=0;
	bool        vol_known=false, vol_fixed=false;
	float       vol=0;
	std::string notice;
	int         notice_seq=0;

	bool operator==(const Snapshot&) const = default;
};

Caster::Caster()
	: cm_(std::make_shared<CastManager>()),
	  queue_(std::make_unique<WorkPool>(1)),
	  scan_(std::make_unique<WorkPool>(1))
	{
	}

Caster::~Caster()
	{
	// The receiver is left playing what it has: closing a sender does not
	// stop a Cast session anywhere else either.
	stop_watching();
	}

void Caster::discover()
	{
	if(discovering_) return;
	discovering_ = true;
	scan_->submit([this, cm=cm_]() {
		CastManager::DiscoverOpts opts;
		opts.verbose = false;
		auto found = cm->discover(opts);
		std::vector<Device> devices;
		for(const auto& d: found)
			devices.push_back({d.id, d.name, d.model, d.address, d.port, d.capabilities});
		std::sort(devices.begin(), devices.end(),
		          [](const Device& a, const Device& b) { return a.name<b.name; });
		on_ui([this, devices=std::move(devices)]() {
			discovering_ = false;
			devices_     = devices;
			signal_devices.emit();
			});
		});
	}

bool Caster::discovering() const
	{
	return discovering_;
	}

const std::vector<Caster::Device>& Caster::devices() const
	{
	return devices_;
	}

void Caster::connect(size_t index)
	{
	if(index>=devices_.size()) return;
	if(active_) disconnect();
	const Device& d = devices_[index];
	CastManager::CastDevice dev;
	dev.id           = d.id;
	dev.name         = d.name;
	dev.model        = d.model;
	dev.address      = d.address;
	dev.port         = d.port;
	dev.capabilities = d.capabilities;
	active_      = true;
	device_name_ = d.name;
	state_       = "IDLE";
	started_     = false;
	session_     = 0;
	stale_session_ = -1;
	vol_known_   = false;
	queue_->submit([cm=cm_, dev]() { cm->start(dev); });
	watching_ = true;
	watcher_  = std::thread(&Caster::watch, this);
	signal_session.emit();
	}

void Caster::disconnect()
	{
	if(!active_) return;
	stop_watching();
	active_  = false;
	started_ = false;
	state_   = "IDLE";
	queue_->submit([cm=cm_]() { cm->stop(); });
	signal_session.emit();
	}

bool Caster::active() const
	{
	return active_;
	}

const std::string& Caster::device_name() const
	{
	return device_name_;
	}

void Caster::prepare(double start)
	{
	stale_session_ = session_;
	started_ = false;
	state_   = "IDLE";
	time_    = start;
	at_      = std::chrono::steady_clock::now();
	}

void Caster::load(const std::string& url, const std::string& mime, double duration,
                  double start, const nlohmann::json& metadata)
	{
	if(!active_) return;
	CastManager::LoadRequest req;
	req.url          = url;
	req.mime         = mime;
	req.duration     = duration;
	req.current_time = float(start);
	req.metadata     = metadata;
	prepare(start);
	queue_->submit([cm=cm_, req]() { cm->load(req); });
	}

void Caster::pause()
	{
	if(!active_) return;
	// Shown at once; the receiver's own report follows within a second.
	time_  = position();
	at_    = std::chrono::steady_clock::now();
	state_ = "PAUSED";
	signal_state.emit(false);
	queue_->submit([cm=cm_]() { cm->cast_pause(); });
	}

void Caster::resume()
	{
	if(!active_) return;
	at_    = std::chrono::steady_clock::now();
	state_ = "PLAYING";
	signal_state.emit(true);
	queue_->submit([cm=cm_]() { cm->cast_play(); });
	}

void Caster::seek(double seconds)
	{
	if(!active_) return;
	time_ = seconds;
	at_   = std::chrono::steady_clock::now();
	queue_->submit([cm=cm_, seconds]() { cm->cast_seek(float(seconds)); });
	}

bool Caster::playing() const
	{
	return active_ && (state_=="PLAYING" || state_=="BUFFERING");
	}

bool Caster::loaded() const
	{
	return active_ && started_;
	}

double Caster::position() const
	{
	if(state_!="PLAYING") return time_;
	std::chrono::duration<double> since = std::chrono::steady_clock::now()-at_;
	return time_ + since.count();
	}

bool Caster::volume_known() const
	{
	return vol_known_;
	}

bool Caster::volume_fixed() const
	{
	return vol_fixed_;
	}

float Caster::volume() const
	{
	return vol_;
	}

void Caster::volume_step(int percent)
	{
	if(!active_ || !vol_known_ || vol_fixed_) return;
	// Rounded to whole percent, so repeated steps do not drift.
	float level = std::clamp(std::round(vol_*100.0f+float(percent))/100.0f, 0.0f, 1.0f);
	vol_ = level;
	signal_volume.emit();
	queue_->submit([cm=cm_, level]() { cm->cast_volume(level); });
	}

void Caster::watch()
	{
	// CastManager's poll loop keeps its status current; this turns changes
	// in it into calls on the main loop.
	Snapshot last;
	last.state = "IDLE";
	while(watching_) {
		auto st  = cm_->wait_status(250);
		auto vol = cm_->volume_state();
		auto n   = cm_->notice();
		Snapshot s;
		s.state       = st.player_state;
		s.idle_reason = st.idle_reason;
		s.time        = st.current_time;
		s.session     = st.media_session_id;
		s.vol_known   = vol.known;
		s.vol_fixed   = vol.fixed;
		s.vol         = vol.level;
		s.notice      = n.text;
		s.notice_seq  = n.seq;
		if(s==last) continue;
		last = s;
		on_ui([this, s]() { received(s); });
		}
	}

void Caster::stop_watching()
	{
	watching_ = false;
	if(watcher_.joinable())
		watcher_.join();
	}

void Caster::received(const Snapshot& s)
	{
	if(!active_) return;
	if(s.vol_known!=vol_known_ || s.vol_fixed!=vol_fixed_ || s.vol!=vol_) {
		vol_known_ = s.vol_known;
		vol_fixed_ = s.vol_fixed;
		vol_       = s.vol;
		signal_volume.emit();
		}
	if(s.notice_seq!=notice_seq_) {
		notice_seq_ = s.notice_seq;
		if(!s.notice.empty()) signal_error.emit(s.notice);
		}

	// The media a load replaces, until the receiver moves on from it.
	if(s.session!=0 && s.session==stale_session_) return;
	session_ = s.session;
	bool was_playing = playing();
	time_ = s.time;
	at_   = std::chrono::steady_clock::now();
	if(s.state=="IDLE") {
		// A fresh load reads IDLE with no reason until the receiver starts;
		// only the end of something that did start counts.
		if(started_ && s.idle_reason=="FINISHED") {
			started_ = false;
			state_   = s.state;
			signal_ended.emit();
			return;
			}
		if(started_ && s.idle_reason=="ERROR") {
			started_ = false;
			state_   = s.state;
			signal_state.emit(false);
			signal_error.emit("The receiver could not play this track.");
			return;
			}
		if(started_ && !s.idle_reason.empty()) {
			// INTERRUPTED or CANCELLED: something else took the receiver.
			std::cout << stamp() << "Cast: idle, " << s.idle_reason << std::endl;
			started_ = false;
			}
		}
	else
		started_ = true;
	state_ = s.state;
	if(playing()!=was_playing)
		signal_state.emit(playing());
	}

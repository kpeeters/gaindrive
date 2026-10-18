#include "playback.hh"
#include "apiclient.hh"
#include "settings.hh"

#include <glibmm/main.h>

#include <cmath>

namespace {
	// "Previous" restarts the track past this point, as everywhere else.
	const double restart_after = 3;
	// The server drops a now-playing entry it has not heard about for five
	// minutes; the web client refreshes it every 30 seconds.
	const unsigned report_every = 30;
}

Playback::Playback(const AppSettings& settings)
	: settings_(settings)
	{
	player_.signal_advanced.connect(sigc::mem_fun(*this, &Playback::advanced));
	player_.signal_ended.connect(sigc::mem_fun(*this, &Playback::ended));
	player_.signal_state.connect([this](bool playing) {
		// Only `playing` is read off the player: a stop, an end and an error
		// leave PLAYING too, and must not reach the server as a pause after
		// their own `stopped`. toggle() reports the real pauses.
		if(playing && current()) report("playing", position());
		signal_state.emit(playing);
		});
	player_.signal_error.connect(sigc::mem_fun(*this, &Playback::error));
	timer_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Playback::tick), 1);
	}

Playback::~Playback()
	{
	timer_.disconnect();
	}

void Playback::set_api(std::shared_ptr<ApiClient> api)
	{
	if(api_ && current()) stop();
	api_ = std::move(api);
	queue_.clear();
	index_ = -1;
	auto_from_ = 0;
	signal_track.emit();
	}

void Playback::play(const std::vector<Song>& songs, size_t start_at, double offset)
	{
	if(start_at>=songs.size()) return;
	queue_     = songs;
	index_     = long(start_at);
	auto_from_ = start_at+1;
	start(offset);
	}

void Playback::enqueue(const Song& song)
	{
	if(auto_from_<queue_.size())
		queue_.erase(queue_.begin()+long(auto_from_), queue_.end());
	queue_.push_back(song);
	auto_from_ = queue_.size();
	update_next();
	signal_track.emit();
	}

void Playback::jump(size_t index)
	{
	if(index>=queue_.size()) return;
	index_ = long(index);
	start();
	}

void Playback::remove(size_t index)
	{
	if(index>=queue_.size() || long(index)==index_) return;
	queue_.erase(queue_.begin()+long(index));
	if(long(index)<index_) --index_;
	if(index<auto_from_) --auto_from_;
	update_next();
	signal_track.emit();
	}

void Playback::toggle()
	{
	if(!current()) return;
	if(player_.playing()) {
		player_.pause();
		report("paused", position());
		}
	else if(player_.position()>0)
		player_.resume();
	else
		// Stopped or ended: play the current track again from the start.
		start();
	}

void Playback::next()
	{
	if(index_+1<long(queue_.size())) {
		++index_;
		start();
		}
	}

void Playback::prev()
	{
	if(!current()) return;
	if(position()>restart_after)
		seek(0);
	else if(index_>0) {
		--index_;
		start();
		}
	}

void Playback::seek(double seconds)
	{
	const Song *s = current();
	if(!s || !api_) return;
	seconds = std::max(0.0, seconds);
	if(local_offset_==0 && player_.seekable()) {
		player_.seek(seconds);
		return;
		}
	// A converted stream cannot seek, so ask for one that starts there.
	// Whole seconds, as timeOffset takes them.
	local_offset_ = std::floor(seconds);
	player_.play_uri(stream_url(*s, seconds, mp3_retried_));
	next_id_.clear();
	update_next();
	}

void Playback::stop()
	{
	if(current()) report("stopped", position());
	player_.stop();
	}

const std::vector<Song>& Playback::queue() const
	{
	return queue_;
	}

long Playback::index() const
	{
	return index_;
	}

const Song *Playback::current() const
	{
	if(index_<0 || index_>=long(queue_.size())) return nullptr;
	return &queue_[size_t(index_)];
	}

bool Playback::playing() const
	{
	return player_.playing();
	}

double Playback::position() const
	{
	return local_offset_ + player_.position();
	}

void Playback::start(double offset)
	{
	const Song *s = current();
	if(!s || !api_) return;
	scrobbled_    = false;
	ticks_        = 0;
	mp3_retried_  = false;
	local_offset_ = 0;
	report("starting", offset);
	// With a quality cap the stream may well be converted, and then it
	// cannot seek: ask for one that starts at the offset instead.
	if(offset>0 && settings_.quality_cap>0) {
		local_offset_ = std::floor(offset);
		player_.play_uri(stream_url(*s, offset));
		}
	else
		player_.play_uri(stream_url(*s), offset);
	next_id_.clear();
	update_next();
	signal_track.emit();
	}

void Playback::error(const std::string& e)
	{
	const Song *s = current();
	// The web client's fallback: what GStreamer cannot decode, the server
	// can convert. Once per track, from where it failed.
	if(s && api_ && !mp3_retried_) {
		mp3_retried_ = true;
		double at = std::floor(position());
		local_offset_ = at;
		player_.play_uri(stream_url(*s, at, true));
		next_id_.clear();
		update_next();
		return;
		}
	if(s) report("stopped", 0);
	signal_error.emit(e);
	}

void Playback::update_next()
	{
	// Recomputed after every queue change: the player cannot ask, because
	// the moment it needs the answer it is on a streaming thread.
	std::string id;
	if(api_ && index_>=0 && index_+1<long(queue_.size()))
		id = queue_[size_t(index_+1)].ref.id;
	if(id==next_id_) return;
	next_id_ = id;
	player_.set_next_uri(id.empty() ? "" : stream_url(queue_[size_t(index_+1)]));
	}

void Playback::advanced()
	{
	// The gapless successor started; the queue catches up with the player.
	if(!current()) return;
	if(!scrobbled_)
		api_->call("scrobble", {{"id", current()->ref.id}, {"submission", "true"}}, [](const ApiResult&) {});
	++index_;
	next_id_.clear();
	scrobbled_    = false;
	ticks_        = 0;
	local_offset_ = 0;
	mp3_retried_  = false;
	report("starting", 0);
	update_next();
	signal_track.emit();
	}

void Playback::ended()
	{
	if(!current()) return;
	if(!scrobbled_) {
		scrobbled_ = true;
		api_->call("scrobble", {{"id", current()->ref.id}, {"submission", "true"}}, [](const ApiResult&) {});
		}
	report("stopped", current()->duration);
	signal_state.emit(false);
	}

bool Playback::tick()
	{
	const Song *s = current();
	if(!s || !api_ || !player_.playing()) return true;
	if(!scrobbled_ && s->duration>0 && position()>=s->duration/2.0) {
		scrobbled_ = true;
		api_->call("scrobble", {{"id", s->ref.id}, {"submission", "true"}}, [](const ApiResult&) {});
		}
	if(++ticks_%report_every==0)
		report("playing", position());
	return true;
	}

void Playback::report(const char *state, double pos)
	{
	const Song *s = current();
	if(!s || !api_) return;
	// ignoreScrobble: play counts come from scrobble, and the server would
	// otherwise count each play twice.
	api_->call("reportPlayback", {
		{"mediaId", s->ref.id}, {"mediaType", "song"}, {"state", state},
		{"positionMs", std::to_string(std::lround(pos*1000))},
		{"ignoreScrobble", "true"}
		}, [](const ApiResult&) {});
	}

void Playback::report_stopped_now()
	{
	const Song *s = current();
	if(!s || !api_ || !player_.playing()) return;
	ApiClient::get_json(api_->session(), "reportPlayback", {
		{"mediaId", s->ref.id}, {"mediaType", "song"}, {"state", "stopped"},
		{"positionMs", std::to_string(std::lround(position()*1000))},
		{"ignoreScrobble", "true"}
		});
	}

std::string Playback::stream_url(const Song& s, double offset, bool mp3) const
	{
	// No `pace`: unlike a Cast receiver, playbin reads steadily as it plays,
	// so the connection never sits idle long enough for a proxy to cut it.
	Params p = {{"id", s.ref.id}};
	if(settings_.quality_cap>0) {
		p.push_back({"maxBitRate", std::to_string(settings_.quality_cap)});
		p.push_back({"format", mp3 ? "mp3" : settings_.format});
		// What may be sent as it stands. Lossy only, as on Android: a token
		// means "acceptable", so naming FLAC under a cap would ask for the
		// lossless file the cap exists to avoid.
		p.push_back({"playable", "adts/aac,mp4/aac,mpeg/mp3,ogg/opus,ogg/vorbis"});
		}
	else if(mp3)
		p.push_back({"format", "mp3"});
	// Without a cap, the original file: GStreamer decodes whatever is stored.
	if(offset>0)
		p.push_back({"timeOffset", std::to_string(long(offset))});
	return api_->url("stream", p);
	}

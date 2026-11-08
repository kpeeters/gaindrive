#include "playback.hh"
#include "apiclient.hh"
#include "clients.hh"
#include "jsonread.hh"
#include "settings.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "videocodecs.hh"
#include "workpool.hh"

#include <glibmm/main.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iostream>

namespace {
	// "Previous" restarts the track past this point, as everywhere else.
	const double restart_after = 3;
	// The local picture of a cast film: inside picture_dead its offset is
	// left alone; beyond picture_hard it is moved by a seek instead. The web
	// client's numbers.
	const double   picture_dead     = 0.03;
	const double   picture_hard     = 1.0;
	// Behind by more than this, the picture is moved rather than made to
	// catch up: a negative offset only works while decoding runs faster
	// than real time, and when it does not the picture stutters for good.
	const double   picture_behind   = 0.1;
	const double   picture_lead_start = 1.0;
	const double   picture_lead_max   = 8.0;
	const unsigned picture_every_ms = 100;
	// The server drops a now-playing entry it has not heard about for five
	// minutes; the web client refreshes it every 30 seconds.
	const unsigned report_every = 30;

	bool flag(const nlohmann::json& j, const std::string& key)
		{
		const auto& v = jsub(j, key);
		return v.is_boolean() && v.get<bool>();
		}

	// What a cast is sent, in the web client's words, from the server's
	// description of it.
	std::string cast_how(const nlohmann::json& c, bool is_video)
		{
		std::string suffix = jstr(c, "sentSuffix"), tier = jstr(c, "tier");
		std::transform(suffix.begin(), suffix.end(), suffix.begin(),
		               [](unsigned char ch) { return std::toupper(ch); });
		int kbps = jint(c, "sentBitRate");
		std::string what = kbps>0 ? suffix + " " + std::to_string(kbps) + " kbps" : suffix;
		std::string how  = flag(c, "audioOnly")
		                     ? (tier=="remux" ? "soundtrack only, copied" : "soundtrack only, re-encoded")
		                 : tier=="copy"   ? "HLS, picture copied"
		                 : tier=="encode" && !is_video ? "converted"
		                 : tier=="encode" ? "HLS, re-encoded"
		                 :                  "as stored";
		return what.empty() ? how : what + " - " + how;
		}
}

Playback::Playback(const AppSettings& settings, WorkPool& pool, const Clients& clients)
	: settings_(settings), pool_(pool), clients_(clients)
	{
	player_.signal_advanced.connect(sigc::mem_fun(*this, &Playback::advanced));
	player_.signal_ended.connect(sigc::mem_fun(*this, &Playback::ended));
	player_.signal_state.connect([this](bool playing) {
		// The picture of a cast film says nothing about the receiver.
		if(picture_) return;
		// Only `playing` is read off the player: a stop, an end and an error
		// leave PLAYING too, and must not reach the server as a pause after
		// their own `stopped`. toggle() reports the real pauses.
		if(playing && current()) report("playing", position());
		signal_state.emit(playing);
		});
	player_.signal_error.connect(sigc::mem_fun(*this, &Playback::error));
	caster_.signal_state.connect([this](bool playing) {
		if(playing && current()) report("playing", position());
		signal_state.emit(playing);
		});
	caster_.signal_ended.connect(sigc::mem_fun(*this, &Playback::cast_ended));
	caster_.signal_error.connect([this](const std::string& e) { signal_error.emit(e); });
	timer_ = Glib::signal_timeout().connect_seconds(sigc::mem_fun(*this, &Playback::tick), 1);
	}

Playback::~Playback()
	{
	timer_.disconnect();
	picture_timer_.disconnect();
	}

void Playback::forget_server(const std::string& key)
	{
	const Song *s = current();
	if(s && s->ref.server==key) {
		stop();
		index_ = -1;
		}
	for(size_t i=queue_.size(); i-- > 0;) {
		if(queue_[i].ref.server!=key) continue;
		queue_.erase(queue_.begin()+long(i));
		if(long(i)<index_) --index_;
		if(i<auto_from_) --auto_from_;
		}
	next_id_.clear();
	update_next();
	signal_track.emit();
	}

std::shared_ptr<ApiClient> Playback::api_for(const Song& s) const
	{
	return clients_.get(s.ref.server);
	}

void Playback::scrobble(const Song& s)
	{
	if(auto api = api_for(s))
		api->call("scrobble", {{"id", s.ref.id}, {"submission", "true"}}, [](const ApiResult&) {});
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
	if(caster_.active()) {
		if(caster_.playing()) {
			caster_.pause();
			report("paused", position());
			}
		else if(caster_.loaded())
			caster_.resume();
		else
			start();
		return;
		}
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
	if(!s || !api_for(*s)) return;
	seconds = std::max(0.0, seconds);
	signal_seeked.emit(seconds);
	// The receiver fetches a converted track or soundtrack from the server's
	// cache, whole, so it seeks in anything.
	if(caster_.active()) {
		caster_.seek(seconds);
		// picture_sync() follows a running picture by itself, but one that
		// ran out has to be fetched again.
		if(picture_ && picture_ended_) {
			picture_stop();
			picture_follow(seconds);
			}
		return;
		}
	// A film is either the stored file or HLS, and both seek.
	if(shows_picture(*s) || (local_offset_==0 && player_.seekable())) {
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
	++cast_gen_;
	picture_stop();
	caster_.disconnect();
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
	return caster_.active() ? caster_.playing() : player_.playing();
	}

GdkPaintable *Playback::paintable() const
	{
	return player_.paintable();
	}

double Playback::position() const
	{
	if(caster_.active()) return caster_.position();
	return local_offset_ + player_.position();
	}

void Playback::set_eq(bool on, const std::vector<int>& gains)
	{
	player_.set_eq(on, gains);
	}

void Playback::set_volume(double level)
	{
	player_.set_volume(level);
	}

double Playback::volume() const
	{
	return player_.volume();
	}

Caster& Playback::caster()
	{
	return caster_;
	}

void Playback::cast_to(size_t device)
	{
	double pos = position();
	if(!caster_.active()) player_.stop();
	caster_.connect(device);
	// Whatever was current carries on there, from where it was.
	if(current()) cast_start(pos);
	}

void Playback::stop_casting()
	{
	if(!caster_.active()) return;
	double pos = position();
	bool was   = caster_.playing();
	++cast_gen_;
	picture_stop();
	caster_.disconnect();
	if(current() && was) {
		local_start(pos);
		next_id_.clear();
		update_next();
		}
	signal_state.emit(was && current());
	}

void Playback::start(double offset)
	{
	const Song *s = current();
	if(!s || !api_for(*s)) return;
	scrobbled_    = false;
	ticks_        = 0;
	mp3_retried_  = false;
	local_offset_ = 0;
	report("starting", offset);
	if(caster_.active())
		cast_start(offset);
	else {
		local_start(offset);
		next_id_.clear();
		update_next();
		}
	signal_track.emit();
	signal_started.emit();
	}

void Playback::local_start(double offset)
	{
	const Song *s = current();
	// With a quality cap the stream may well be converted, and a video's
	// soundtrack always is; then it cannot seek, so ask for one that starts
	// at the offset instead.
	local_offset_ = 0;
	bool pipe = s->is_video ? !shows_picture(*s) : settings_.quality_cap>0;
	if(offset>0 && pipe) {
		local_offset_ = std::floor(offset);
		player_.play_uri(stream_url(*s, offset));
		}
	else
		player_.play_uri(stream_url(*s), offset, shows_picture(*s));
	}

const std::string& Playback::cast_sent() const
	{
	return cast_sent_;
	}

const std::string& Playback::cast_type() const
	{
	return cast_type_;
	}

std::string Playback::local_url() const
	{
	const Song *s = current();
	if(!s || !api_for(*s)) return "";
	if(shows_picture(*s)) return video_url(*s, mp3_retried_);
	return stream_url(*s, local_offset_, mp3_retried_);
	}

bool Playback::cast_picture() const
	{
	const Song *s = current();
	return caster_.active() && s && s->is_video && cast_screen_;
	}

bool Playback::shows_picture(const Song& s) const
	{
	return s.is_video && !settings_.video_audio_only;
	}

void Playback::video_mode_changed()
	{
	const Song *s = current();
	if(!s || !s->is_video || !api_for(*s)) return;
	// The receiver goes on as it is: what it is sent is the server's
	// decision, which this setting is not part of. Only the picture here
	// comes or goes.
	if(caster_.active())
		picture_follow(position());
	else {
		mp3_retried_ = false;
		local_start(position());
		next_id_.clear();
		update_next();
		}
	// For the window, which shows the picture or puts it away.
	signal_started.emit();
	}

void Playback::set_caption(int track)
	{
	const Song *s = current();
	if(!s || !s->is_video) return;
	caption_song_  = s->ref.server + "|" + s->ref.id;
	caption_track_ = track;
	if(cast_picture())
		caster_.set_tracks(track>0 ? std::vector<int>{track} : std::vector<int>{});
	}

void Playback::cast_start(double offset)
	{
	const Song *s = current();
	if(!s || !api_for(*s)) return;
	picture_stop();
	// What the receiver is sent is the server's decision, the one it makes
	// for its own casts, so a track or film reaches a receiver the same way
	// whichever player sends it. Only the receiver and the quality are ours.
	cast_screen_ = false;
	caster_.prepare(offset);
	unsigned gen = ++cast_gen_;

	const auto& dev = caster_.device();
	Params p = {{"id", s->ref.id}, {"deviceId", dev.id},
	            {"capabilities", std::to_string(dev.capabilities)}};
	if(s->is_video) {
		if(caption_song_==s->ref.server + "|" + s->ref.id && caption_track_>0)
			p.push_back({"trackId", std::to_string(caption_track_)});
		}
	else {
		int cap = settings_.cast_original ? 0 : settings_.quality_cap;
		if(cap>0) {
			p.push_back({"maxBitRate", std::to_string(cap)});
			p.push_back({"format", settings_.format});
			}
		}

	pool_.submit([this, api=api_for(*s), song=*s, p, gen, offset]() {
		// The server builds a conversion or a soundtrack before answering,
		// which for a long film is a while.
		auto r = ApiClient::get_json(api->session(), "getCastLoad", p, 300);
		const auto& c = jsub(r.body, "castLoad");
		std::string token = jstr(c, "castToken");

		nlohmann::json meta;
		if(!song.is_video || flag(c, "audioOnly"))
			meta = {{"metadataType", 3}, {"title", song.title}, {"artist", song.artist},
			        {"albumName", song.album}};
		else
			meta = {{"metadataType", 1}, {"title", song.title}, {"subtitle", song.album}};
		if(!token.empty() && !song.cover_art.empty())
			meta["images"] = {{{"url", api->cast_url("getCoverArt",
			                     {{"id", song.cover_art}, {"size", "600"}}, token)}}};

		on_ui([this, r, c, meta, gen, offset, api, p, is_video=song.is_video]() {
			if(gen!=cast_gen_ || !caster_.active()) return;
			if(!r.ok) {
				signal_error.emit(std::string(is_video ? "Cannot cast this film: "
				                                       : "Cannot cast this track: ") + r.error);
				signal_state.emit(false);
				return;
				}
			cast_screen_ = flag(c, "receiverShowsVideo");
			cast_sent_   = cast_how(c, is_video);
			cast_type_   = jstr(c, "contentType");
			Caster::Load l;
			l.url      = jstr(c, "url");
			l.mime     = cast_type_;
			l.duration = jnum(c, "duration");
			l.start    = offset;
			l.metadata = meta;
			l.tracks   = jsub(c, "tracks").is_array() ? jsub(c, "tracks")
			                                         : nlohmann::json::array();
			for(const auto& t: jsub(c, "activeTrackIds"))
				if(t.is_number_integer()) l.active_tracks.push_back(t.get<int>());
			const auto& fb = jsub(c, "fallback");
			if(fb.is_object()) {
				l.fallback = std::make_shared<Caster::Load>();
				l.fallback->url      = jstr(fb, "url");
				l.fallback->mime     = jstr(fb, "contentType");
				l.fallback->duration = jnum(fb, "duration");
				// Runs on the caster's queue once the receiver has refused
				// the film: the server records that and builds the soundtrack.
				Params refused = p;
				refused.push_back({"refused", "true"});
				l.fallback_prepare = [this, api, refused, fb, gen]() {
					auto rr = ApiClient::get_json(api->session(), "getCastLoad", refused, 300);
					if(rr.ok)
						on_ui([this, fb, gen]() {
							if(gen!=cast_gen_) return;
							cast_screen_ = false;
							cast_sent_   = cast_how(fb, true);
							cast_type_   = jstr(fb, "contentType");
							picture_follow(position());
							});
					return rr.ok;
					};
				}
			caster_.load(l);
			picture_follow(offset);
			});
		});
	}

void Playback::cast_ended()
	{
	if(!current()) return;
	if(!scrobbled_) {
		scrobbled_ = true;
		scrobble(*current());
		}
	// No gapless on a receiver: the next track is loaded when this one ends.
	if(index_+1<long(queue_.size())) {
		++index_;
		start();
		return;
		}
	report("stopped", current()->duration);
	signal_state.emit(false);
	}

void Playback::error(const std::string& e)
	{
	const Song *s = current();
	if(picture_) {
		// Only the picture failed; the receiver plays on. Once as HLS, as
		// for a film played here, then the panel saying where it went.
		std::cout << stamp() << "Cast: local picture failed: " << e << std::endl;
		if(s && api_for(*s) && !picture_hls_) {
			picture_hls_     = true;
			picture_running_ = false;
			picture_holding_ = false;
			picture_ended_   = false;
			picture_offset_  = 0;
			player_.play_picture(video_url(*s, true), position());
			}
		else
			picture_stop();
		return;
		}
	// The web client's fallback: what GStreamer cannot decode, the server
	// can convert. Once per track, from where it failed.
	if(s && api_for(*s) && !mp3_retried_ && shows_picture(*s)) {
		mp3_retried_ = true;
		double at = position();
		player_.play_uri(video_url(*s, true), at, true);
		return;
		}
	if(s && api_for(*s) && !mp3_retried_) {
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
	if(caster_.active()) return;
	// Not around a film: the player's flags are set per stream, and a
	// gapless switch would keep the previous one's.
	if(index_>=0 && index_+1<long(queue_.size())
	   && !shows_picture(queue_[size_t(index_)]) && !shows_picture(queue_[size_t(index_+1)]))
		id = queue_[size_t(index_+1)].ref.server + "|" + queue_[size_t(index_+1)].ref.id;
	if(id==next_id_) return;
	next_id_ = id;
	player_.set_next_uri(id.empty() ? "" : stream_url(queue_[size_t(index_+1)]));
	}

void Playback::advanced()
	{
	// The gapless successor started; the queue catches up with the player.
	if(!current()) return;
	if(!scrobbled_)
		scrobble(*current());
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
	if(picture_) {
		// The picture runs out while the receiver still plays the last
		// of the sound, and the receiver owns the advance.
		picture_ended_   = true;
		picture_running_ = false;
		return;
		}
	if(!current()) return;
	if(!scrobbled_) {
		scrobbled_ = true;
		scrobble(*current());
		}
	// Only a film ends with more queued: music goes on gaplessly.
	if(index_+1<long(queue_.size())) {
		report("stopped", current()->duration);
		++index_;
		start();
		return;
		}
	report("stopped", current()->duration);
	signal_state.emit(false);
	}

bool Playback::tick()
	{
	const Song *s = current();
	if(!s || !api_for(*s) || !playing()) return true;
	if(!scrobbled_ && s->duration>0 && position()>=s->duration/2.0) {
		scrobbled_ = true;
		scrobble(*s);
		}
	if(++ticks_%report_every==0)
		report("playing", position());
	return true;
	}

void Playback::report(const char *state, double pos)
	{
	const Song *s = current();
	auto api = s ? api_for(*s) : nullptr;
	if(!api) return;
	// ignoreScrobble: play counts come from scrobble, and the server would
	// otherwise count each play twice.
	api->call("reportPlayback", {
		{"mediaId", s->ref.id}, {"mediaType", "song"}, {"state", state},
		{"positionMs", std::to_string(std::lround(pos*1000))},
		{"ignoreScrobble", "true"}
		}, [](const ApiResult&) {});
	signal_reported.emit(s->ref, pos);
	}

void Playback::report_stopped_now()
	{
	const Song *s = current();
	// A receiver plays on after this window closes.
	auto api = s ? api_for(*s) : nullptr;
	if(!api || !player_.playing() || caster_.active()) return;
	ApiClient::get_json(api->session(), "reportPlayback", {
		{"mediaId", s->ref.id}, {"mediaType", "song"}, {"state", "stopped"},
		{"positionMs", std::to_string(std::lround(position()*1000))},
		{"ignoreScrobble", "true"}
		});
	}

std::string Playback::stream_url(const Song& s, double offset, bool mp3) const
	{
	if(shows_picture(s)) return video_url(s, mp3);
	auto api = api_for(s);
	if(!api) return "";
	// No `pace`: unlike a Cast receiver, playbin reads steadily as it plays,
	// so the connection never sits idle long enough for a proxy to cut it.
	Params p = {{"id", s.ref.id}};
	if(s.is_video) {
		// A format makes the server send the soundtrack alone. No
		// `playable`, which would allow the film as stored; without a cap,
		// Opus and no ceiling, so a soundtrack already in Opus is copied.
		p.push_back({"format", mp3 ? "mp3" : settings_.quality_cap>0 ? settings_.format : "opus"});
		if(settings_.quality_cap>0)
			p.push_back({"maxBitRate", std::to_string(settings_.quality_cap)});
		}
	else if(settings_.quality_cap>0) {
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
	return api->url("stream", p);
	}

std::string Playback::video_url(const Song& s, bool hls) const
	{
	auto api = api_for(s);
	if(!api) return "";
	// No quality cap: it is an audio bitrate, and the web client does not
	// apply it to films either.
	Params p = {{"id", s.ref.id}};
	// The stored file whenever GStreamer decodes it, saying so, since the
	// server otherwise only sends untouched what a browser decodes.
	if(!hls && gst_plays(s.suffix, s.video_codec, s.audio_codec)) {
		std::string declared = s.suffix + ",video/" + s.video_codec;
		if(!s.audio_codec.empty())
			declared += ",audio/" + s.audio_codec;
		p.push_back({"playable", declared});
		return api->url("stream", p);
		}
	if(!hls && s.transcoded_suffix.empty())
		return api->url("stream", p);
	// HLS seeks without the server encoding from the start, unlike a
	// converted stream.view. Opus and FLAC soundtracks are copied rather
	// than converted to AAC, as GStreamer plays both from MP4.
	p.push_back({"audioCodecs", "opus,flac"});
	return api->url("hls", p);
	}

void Playback::picture_follow(double offset)
	{
	const Song *s = current();
	bool want = s && api_for(*s) && caster_.active() && shows_picture(*s) && !cast_screen_
	            && settings_.cast_local_video && player_.paintable();
	if(!want) {
		picture_stop();
		return;
		}
	if(picture_) return;
	picture_         = true;
	picture_running_ = false;
	picture_ended_   = false;
	picture_hls_     = false;
	picture_holding_ = false;
	picture_lead_    = picture_lead_start;
	picture_offset_  = 0;
	// Held still at the offset: picture_sync() starts it once the receiver
	// plays, which for a soundtrack is after the server has built it.
	player_.play_picture(video_url(*s, false), offset);
	picture_timer_ = Glib::signal_timeout().connect(
		sigc::mem_fun(*this, &Playback::picture_sync), picture_every_ms);
	}

void Playback::picture_stop()
	{
	if(!picture_) return;
	picture_timer_.disconnect();
	// Before picture_ is cleared, so the player's own stop does not reach
	// the window as the receiver stopping.
	player_.stop();
	picture_         = false;
	picture_running_ = false;
	picture_holding_ = false;
	}

bool Playback::picture_sync()
	{
	if(!picture_) return false;
	if(picture_ended_) return true;
	if(!caster_.playing()) {
		if(picture_running_) {
			player_.pause();
			picture_running_ = false;
			}
		// Wherever the receiver resumes, it is sought anew.
		picture_holding_ = false;
		return true;
		}
	double delay = 0;
	auto d = settings_.cast_sync_ms.find(caster_.device().id);
	if(d!=settings_.cast_sync_ms.end()) delay = d->second/1000.0;
	double target = std::max(0.0, caster_.position()-delay);
	if(!player_.settled()) return true;
	if(picture_holding_) {
		double err = player_.position()-target;
		if(err<-picture_behind) {
			// The seek took longer than the lead: the sound is already past.
			picture_lead_ = std::min(picture_lead_*2, picture_lead_max);
			std::cout << stamp() << "Cast: picture seek outlasted its lead, now "
			          << picture_lead_ << " s" << std::endl;
			picture_ahead(target);
			}
		else if(err<=picture_dead) {
			picture_holding_ = false;
			picture_running_ = true;
			player_.resume();
			}
		return true;
		}
	if(!picture_running_) {
		// The receiver has started, or resumed.
		picture_ahead(target);
		return true;
		}
	// Buffering.
	if(!player_.playing()) return true;
	// The sink's reported position ignores its ts-offset, so what is on
	// screen is position - offset; the offset that puts `target` there is
	// the difference itself, with no loop to wind up.
	double err = player_.position()-target;
	if(err>picture_hard || err<-picture_behind) {
		std::cout << stamp() << "Cast: picture " << err << " s out, moving it" << std::endl;
		picture_ahead(target);
		}
	else if(std::fabs(err-picture_offset_)>picture_dead) {
		picture_offset_ = err;
		player_.set_picture_offset(err);
		}
	return true;
	}

void Playback::picture_ahead(double target)
	{
	// Holding a frame until the sound reaches it is exact however slow the
	// seek turns out to be; catching up afterwards is not.
	if(picture_running_) {
		player_.pause();
		picture_running_ = false;
		}
	picture_holding_ = true;
	picture_offset_  = 0;
	player_.set_picture_offset(0);
	player_.seek(target+picture_lead_, true);
	}

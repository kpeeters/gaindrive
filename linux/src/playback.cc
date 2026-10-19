#include "playback.hh"
#include "apiclient.hh"
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
	// The server drops a now-playing entry it has not heard about for five
	// minutes; the web client refreshes it every 30 seconds.
	const unsigned report_every = 30;

	// What the Default Media Receiver decodes, of the types the server
	// reports. An allowlist, so an unknown type is converted rather than
	// refused by the receiver.
	bool cast_plays(const std::string& mime)
		{
		for(const char *m: {"audio/flac", "audio/mpeg", "audio/mp4", "audio/aac",
		                    "audio/ogg", "audio/wav"})
			if(mime==m) return true;
		return false;
		}

	std::string mime_of_format(const std::string& format)
		{
		if(format=="opus") return "audio/ogg";
		if(format=="m4a")  return "audio/mp4";
		if(format=="flac") return "audio/flac";
		return "audio/mpeg";
		}
}

Playback::Playback(const AppSettings& settings, WorkPool& pool)
	: settings_(settings), pool_(pool)
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
	if(!s || !api_) return;
	seconds = std::max(0.0, seconds);
	// The receiver fetches a converted track from the server's cache, whole,
	// so it seeks in anything.
	if(caster_.active()) {
		// A receiver cannot seek in a pipe.
		if(s->is_video && cast_soundtrack_)
			cast_film(seconds);
		else
			caster_.seek(seconds);
		return;
		}
	// A film is either the stored file or HLS, and both seek.
	if(s->is_video || (local_offset_==0 && player_.seekable())) {
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
	if(!s || !api_) return;
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
	// With a quality cap the stream may well be converted, and then it
	// cannot seek: ask for one that starts at the offset instead.
	local_offset_ = 0;
	if(offset>0 && settings_.quality_cap>0 && !s->is_video) {
		local_offset_ = std::floor(offset);
		player_.play_uri(stream_url(*s, offset));
		}
	else
		player_.play_uri(stream_url(*s), offset, s->is_video);
	}

bool Playback::cast_picture() const
	{
	const Song *s = current();
	return caster_.active() && s && s->is_video && !cast_soundtrack_;
	}

void Playback::set_caption(int track)
	{
	const Song *s = current();
	if(!s || !s->is_video) return;
	caption_song_  = s->ref.id;
	caption_track_ = track;
	if(cast_picture())
		caster_.set_tracks(track>0 ? std::vector<int>{track} : std::vector<int>{});
	}

void Playback::cast_start(double offset)
	{
	const Song *s = current();
	if(!s || !api_) return;
	if(s->is_video) {
		cast_film(offset);
		return;
		}
	cast_soundtrack_ = false;
	caster_.prepare(offset);
	unsigned gen = ++cast_gen_;

	Params p = {{"id", s->ref.id}};
	std::string mime = s->content_type;
	int cap = settings_.cast_original ? 0 : settings_.quality_cap;
	if(cap>0) {
		p.push_back({"maxBitRate", std::to_string(cap)});
		p.push_back({"format", settings_.format});
		p.push_back({"playable", "adts/aac,mp4/aac,mpeg/mp3,ogg/opus,ogg/vorbis"});
		mime = mime_of_format(settings_.format);
		}
	else if(!cast_plays(s->content_type)) {
		// Lossless, so a lossy original is not given a second generation
		// of loss.
		p.push_back({"format", "flac"});
		mime = "audio/flac";
		}
	// A receiver reads at playback speed and stops once its buffer is full;
	// unpaced, the server sends the whole track at once and the idle
	// connection is then cut by a proxy or by the receiver's own timeout.
	p.push_back({"pace", "true"});

	pool_.submit([this, api=api_, song=*s, p, mime, gen, offset]() mutable {
		std::string token;
		auto r = ApiClient::get_json(api->session(), "getCastToken", {{"id", song.ref.id}});
		if(r.ok)
			token = jstr(r.body, "castToken");
		else
			std::cout << stamp() << "Cast: no cast token (" << r.error
			          << "); sending the account's credentials" << std::endl;
		std::string url = api->cast_url("stream", p, token);

		// Also what a capped track turned out to be: under the cap the
		// original is sent as it is.
		std::string type, error;
		bool ok = ApiClient::probe_stream(url, type, error);
		std::string sent = type.substr(0, type.find(';'));
		if(!sent.empty()) mime = sent;

		nlohmann::json meta = {
			{"metadataType", 3},
			{"title",        song.title},
			{"artist",       song.artist},
			{"albumName",    song.album}
			};
		// Only with a grant, which covers this song's cover.
		if(!token.empty() && !song.cover_art.empty())
			meta["images"] = {{{"url", api->cast_url("getCoverArt",
			                     {{"id", song.cover_art}, {"size", "600"}}, token)}}};

		on_ui([this, ok, error, url, mime, meta, gen, offset, duration=song.duration]() {
			if(gen!=cast_gen_ || !caster_.active()) return;
			if(!ok) {
				std::cout << stamp() << "Cast: stream probe failed: " << error << std::endl;
				signal_error.emit("Cannot cast this track: " + error);
				signal_state.emit(false);
				return;
				}
			Caster::Load l;
			l.url      = url;
			l.mime     = mime;
			l.duration = duration;
			l.start    = offset;
			l.metadata = meta;
			caster_.load(l);
			});
		});
	}

void Playback::cast_film(double offset)
	{
	const Song *s = current();
	if(!s || !api_) return;
	// Bit 0 of the receiver's `ca` record is video_out; one that does not
	// say is taken to have a screen, as the server takes it. A WiiM has none.
	int  ca      = caster_.device().capabilities;
	bool picture = ca<0 || (ca & 1)!=0;
	cast_soundtrack_ = !picture;
	caster_.prepare(offset);
	unsigned gen = ++cast_gen_;

	Params p = {{"id", s->ref.id}};
	std::string mime, endpoint = "stream";
	const char *ext = ".view";
	double base = 0;
	if(picture) {
		std::string suffix = s->suffix;
		std::transform(suffix.begin(), suffix.end(), suffix.begin(),
		               [](unsigned char c) { return std::tolower(c); });
		// The file itself only when the server says a receiver plays it, and
		// in a container a receiver is known to take; anything else, AV1
		// included, as HLS, which a receiver seeks by segment. No format or
		// cap: either would make the server send the soundtrack alone, or
		// re-encode.
		if(s->cast_direct && (suffix=="mp4" || suffix=="m4v" || suffix=="webm"))
			mime = s->content_type;
		else {
			endpoint = "hls";
			ext      = ".m3u8";
			mime     = "application/x-mpegURL";
			}
		}
	else {
		bool copy = settings_.cast_original && !s->soundtrack_format.empty()
		            && cast_plays(s->soundtrack_content_type);
		if(copy) {
			p.push_back({"format", s->soundtrack_format});
			mime = s->soundtrack_content_type;
			}
		else if(settings_.cast_original) {
			p.push_back({"format", "flac"});
			mime = "audio/flac";
			}
		else {
			// "Original" means the file as stored, which a soundtrack is
			// not; the apps send AAC at 160 kbps then.
			bool capped = settings_.quality_cap>0;
			std::string fmt = capped ? settings_.format : "m4a";
			p.push_back({"format", fmt});
			p.push_back({"maxBitRate", std::to_string(capped ? settings_.quality_cap : 160)});
			mime = mime_of_format(fmt);
			}
		// Always, 0 included: the server then pipes the soundtrack rather
		// than building it whole first, which takes minutes for a film. A
		// receiver cannot seek in a pipe, so a seek reloads from there.
		base = std::floor(offset);
		p.push_back({"timeOffset", std::to_string(long(base))});
		}
	p.push_back({"pace", "true"});

	pool_.submit([this, api=api_, song=*s, p, mime, endpoint, ext, gen, offset, base, picture]() {
		std::string token;
		auto r = ApiClient::get_json(api->session(), "getCastToken", {{"id", song.ref.id}});
		if(r.ok)
			token = jstr(r.body, "castToken");
		else
			std::cout << stamp() << "Cast: no cast token (" << r.error
			          << "); sending the account's credentials" << std::endl;
		std::string url = api->cast_url(endpoint, p, token, ext);

		// The grant covers this film's captions too.
		nlohmann::json tracks = nlohmann::json::array();
		if(picture) {
			auto info = ApiClient::get_json(api->session(), "getVideoInfo", {{"id", song.ref.id}});
			const auto& caps = jsub(jsub(info.body, "videoInfo"), "captions");
			if(!info.ok)
				std::cout << stamp() << "Cast: getVideoInfo failed (" << info.error
				          << "); casting without subtitles" << std::endl;
			else if(caps.is_array())
				for(size_t i=0; i<caps.size(); ++i) {
					const auto& c = caps[i];
					std::string name = jstr(c, "name");
					tracks.push_back({
						{"trackId",          int(i+1)},
						{"type",             "TEXT"},
						{"subtype",          "SUBTITLES"},
						{"trackContentId",   api->cast_url("getCaptions",
						                       {{"id", song.ref.id}, {"captionId", jstr(c, "id")}}, token)},
						{"trackContentType", "text/vtt"},
						// Required for a subtitle track; getVideoInfo has
						// none, and this is the code meaning exactly that.
						{"language",         "und"},
						{"name",             name.empty() ? "Subtitles" : name}
						});
					}
			}

		nlohmann::json meta;
		if(picture)
			meta = {{"metadataType", 1}, {"title", song.title}, {"subtitle", song.album}};
		else
			meta = {{"metadataType", 3}, {"title", song.title}, {"artist", song.artist},
			        {"albumName", song.album}};
		if(!token.empty() && !song.cover_art.empty())
			meta["images"] = {{{"url", api->cast_url("getCoverArt",
			                     {{"id", song.cover_art}, {"size", "600"}}, token)}}};

		on_ui([this, url, mime, meta, tracks, gen, offset, base, picture,
		       id=song.ref.id, duration=song.duration]() {
			if(gen!=cast_gen_ || !caster_.active()) return;
			Caster::Load l;
			l.url      = url;
			l.mime     = mime;
			l.metadata = meta;
			l.tracks   = tracks;
			l.base     = base;
			// A pipe starts where it was asked to, so nothing is left to seek.
			l.start    = picture ? offset : 0;
			l.duration = std::max(0.0, duration - base);
			if(picture && caption_song_==id && caption_track_>0 && size_t(caption_track_)<=tracks.size())
				l.active_tracks = {caption_track_};
			caster_.load(l);
			});
		});
	}

void Playback::cast_ended()
	{
	if(!current()) return;
	if(!scrobbled_) {
		scrobbled_ = true;
		api_->call("scrobble", {{"id", current()->ref.id}, {"submission", "true"}}, [](const ApiResult&) {});
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
	// The web client's fallback: what GStreamer cannot decode, the server
	// can convert. Once per track, from where it failed.
	if(s && api_ && !mp3_retried_ && s->is_video) {
		mp3_retried_ = true;
		double at = position();
		player_.play_uri(video_url(*s, true), at, true);
		return;
		}
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
	if(caster_.active()) return;
	// Not around a film: the player's flags are set per stream, and a
	// gapless switch would keep the previous one's.
	if(api_ && index_>=0 && index_+1<long(queue_.size())
	   && !queue_[size_t(index_)].is_video && !queue_[size_t(index_+1)].is_video)
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
	if(!s || !api_ || !playing()) return true;
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
	// A receiver plays on after this window closes.
	if(!s || !api_ || !player_.playing() || caster_.active()) return;
	ApiClient::get_json(api_->session(), "reportPlayback", {
		{"mediaId", s->ref.id}, {"mediaType", "song"}, {"state", "stopped"},
		{"positionMs", std::to_string(std::lround(position()*1000))},
		{"ignoreScrobble", "true"}
		});
	}

std::string Playback::stream_url(const Song& s, double offset, bool mp3) const
	{
	if(s.is_video) return video_url(s, mp3);
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

std::string Playback::video_url(const Song& s, bool hls) const
	{
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
		return api_->url("stream", p);
		}
	if(!hls && s.transcoded_suffix.empty())
		return api_->url("stream", p);
	// HLS seeks without the server encoding from the start, unlike a
	// converted stream.view. Opus and FLAC soundtracks are copied rather
	// than converted to AAC, as GStreamer plays both from MP4.
	p.push_back({"audioCodecs", "opus,flac"});
	return api_->url("hls", p);
	}

#include "apientry.hh"
#include "subsonic.hh"
#include "artistmatch.hh"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string_view>

using namespace tinyxml2;

// What stream.view would actually send for this song, when that differs from
// the stored file.  Mirrors the branch order in Streamer::serve() — a format
// override wins over the per-user bitrate cap, and the cap alone means mp3.
// Empty when the source is served as-is.
struct TranscodeInfo
	{
	std::string_view mime;
	std::string_view suffix;
	int              bitrate;
	};

static std::optional<TranscodeInfo> transcode_target(
	const MediaStore::ChildEntry& c, int max_bitrate,
	const std::string& format = "")
	{
	// Video: the container is what changes, not the audio muxer, so none of
	// the negotiation below applies.  Anything that is not already in a
	// browser-playable container arrives as MP4, whether that took a remux or
	// a full re-encode.  Whether it is *seekable* is a different question and
	// is answered separately by nativeSeek — see song_entry_json().
	if (is_video_ext(c.codec)) {
		// Same predicate serve_video() picks its tier with, so the advertised
		// type cannot disagree with what the stream turns out to be.
		if (video_direct_playable(c.codec, c.video_codec, c.audio_codec))
			return std::nullopt;   // served untouched
		return TranscodeInfo{ VIDEO_MP4_MIME, "mp4", 0 };
		}

	auto source = target_for(c.codec);
	std::optional<Target> wanted;
	if (!format.empty() && format != "raw")
		wanted = target_for(format);
	// Same muxer and encoder is the same audio, whatever the caller spelled it.
	if (wanted && (!source || source->muxer   != wanted->muxer
	                       || source->encoder != wanted->encoder)) {
		// Keep this rule identical to the one in Streamer::serve(); a client
		// that trusts transcodedBitRate and then receives something else has
		// no way to tell which of the two lied.
		int bitrate = (max_bitrate > 0 && max_bitrate < 320) ? max_bitrate : 320;
		return TranscodeInfo{ wanted->mime, wanted->name, bitrate };
		}
	if (max_bitrate <= 0 || c.bitrate <= 0 || c.bitrate <= max_bitrate)
		return std::nullopt;
	return TranscodeInfo{ "audio/mpeg", "mp3", max_bitrate };
	}

// The artist to report for a song: the file's own tag when it is a different
// claim from the folder it sits in, the folder's artist otherwise.
//
// This is the only place both facts are in hand -- a query has just one of
// them, and a client cannot normalise without a second copy of artist_key()'s
// rule (artistmatch.hh) in every language it is written in.  Falling back to
// the folder rather than to nothing also keeps an untagged file from reporting
// no artist at all, which is what a client sees from every other server in
// that case.
static std::string artist_of(const MediaStore::ChildEntry& c)
	{
	if (c.track_artist.empty()) return c.artist;
	std::string tag_key = artist_key(c.track_artist);
	std::string dir_key = artist_key(c.artist);
	// A name made entirely of punctuation keys as nothing; comparing two empty
	// keys would call unrelated artists equal, so fall back to the raw strings.
	if (tag_key.empty() || dir_key.empty())
		return c.track_artist == c.artist ? c.artist : c.track_artist;
	return tag_key == dir_key ? c.artist : c.track_artist;
	}

// Serialises a song ChildEntry into a JSON object.  When max_bitrate causes a
// transcode, also emits transcodedContentType / transcodedSuffix (standard
// Subsonic) and transcodedBitRate (gaindrive extension; ignored by clients
// that don't know it) so the client knows the actual stream format.
nlohmann::json song_entry_json(const MediaStore::ChildEntry& c,
                               int max_bitrate,
                               const std::string& format)
	{
	// Video rows live in the same table and come back through the same
	// queries; the extension is what distinguishes them, so codecs.hh answers
	// this without a dedicated column travelling through every query.
	bool is_video = is_video_ext(c.codec);
	std::string track_artist = artist_of(c);
	nlohmann::json s = {
		{"id",          sid(c.id)},
		{"parent",      sid(c.parent_id)},
		// The album folder is the song's album in ID3 terms; clients asking
		// for tag-based data expect albumId rather than parent.
		{"albumId",     sid(c.parent_id)},
		{"isDir",       false},
		{"type",        is_video ? "video" : "music"},
		{"isVideo",     is_video},
		{"title",       c.title},
		// The track's own artist, as every other Subsonic server reports it;
		// see artist_of().  displayArtist repeats it and displayAlbumArtist
		// carries the folder-derived artist, which is what makes
		// `artist != displayAlbumArtist` a client's whole test for "this track
		// is not by the album's artist".
		//
		// Both OpenSubsonic fields are sent unconditionally, empty included:
		// the spec's rule is that a server supporting an optional field must
		// always return it, so a client can tell "these agree" from "this
		// server does not know about the field".
		{"artist",            track_artist},
		{"displayArtist",     track_artist},
		{"displayAlbumArtist", c.artist},
		{"album",       c.album},
		{"track",       c.track_number},
		{"discNumber",  c.disc_number},
		{"year",        c.year},
		{"genre",       c.genre},
		{"size",        c.file_size},
		{"contentType", std::string(codec_to_mime(c.codec))},
		{"suffix",      c.codec},
		{"duration",    (int)c.duration},
		{"bitRate",     c.bitrate}
		};
	if (c.cover_art_id >= 0) s["coverArt"] = sid(c.cover_art_id);
	if (!c.starred.empty()) s["starred"] = iso8601(c.starred);
	// gaindrive extension. discNumber already carries this number, and every
	// Subsonic client groups by that; this says the grouping is a *season*, so
	// a client that knows about it can head the group "Series 2" rather than
	// "Disc 2". Omitted rather than sent as 0, so its absence means "not an
	// episode, or an entry from a query that does not select it".
	if (c.season > 0) s["season"] = c.season;
	// Omitted rather than sent as 0 when the scan could not probe the file, or
	// when the query that produced this entry does not select the dimensions.
	if (c.width  > 0) s["originalWidth"]  = c.width;
	if (c.height > 0) s["originalHeight"] = c.height;
	if (auto t = transcode_target(c, max_bitrate, format)) {
		s["transcodedContentType"] = std::string(t->mime);
		s["transcodedSuffix"]      = std::string(t->suffix);
		// Video has no meaningful single bitrate to promise — the encode is
		// CRF-driven — so the field is omitted rather than sent as 0.
		if (t->bitrate > 0) s["transcodedBitRate"] = t->bitrate;
		}
	// gaindrive extension.  Says whether the stream this entry would produce
	// carries a Content-Length and answers Range requests, so the client can
	// let the media element seek by itself instead of re-requesting with
	// timeOffset.  transcodedSuffix cannot answer this: it is present for both
	// the remux and re-encode tiers, and those differ precisely here.
	if (is_video) s["nativeSeek"] = video_seeks_natively(c.video_codec,
	                                                     c.audio_codec);
	return s;
	}

// Creates an XML element for a song with the given tag name.
XMLElement* song_entry_xml(XMLDocument& doc,
                           const MediaStore::ChildEntry& c,
                           const char* tag,
                           int max_bitrate,
                           const std::string& format)
	{
	bool  is_video = is_video_ext(c.codec);
	auto* el = doc.NewElement(tag);
	el->SetAttribute("id",          c.id);
	el->SetAttribute("parent",      c.parent_id);
	el->SetAttribute("isDir",       false);
	el->SetAttribute("title",       c.title.c_str());
	// See the JSON entry: the track's own artist, with the folder-derived one
	// beside it, both OpenSubsonic fields always present.
	std::string track_artist = artist_of(c);
	el->SetAttribute("artist",            track_artist.c_str());
	el->SetAttribute("displayArtist",     track_artist.c_str());
	el->SetAttribute("displayAlbumArtist", c.artist.c_str());
	el->SetAttribute("album",       c.album.c_str());
	if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
	el->SetAttribute("track",       c.track_number);
	el->SetAttribute("discNumber",  c.disc_number);
	el->SetAttribute("year",        c.year);
	el->SetAttribute("genre",       c.genre.c_str());
	el->SetAttribute("size",        (int64_t)c.file_size);
	el->SetAttribute("contentType", std::string(codec_to_mime(c.codec)).c_str());
	el->SetAttribute("suffix",      c.codec.c_str());
	el->SetAttribute("duration",    (int)c.duration);
	el->SetAttribute("bitRate",     c.bitrate);
	el->SetAttribute("type",        is_video ? "video" : "music");
	el->SetAttribute("isVideo",     is_video);
	el->SetAttribute("albumId",     c.parent_id);
	if (c.width  > 0) el->SetAttribute("originalWidth",  c.width);
	if (c.height > 0) el->SetAttribute("originalHeight", c.height);
	// See the JSON entry: a gaindrive extension marking discNumber as a season.
	if (c.season > 0) el->SetAttribute("season", c.season);
	if (!c.starred.empty())
		el->SetAttribute("starred", iso8601(c.starred).c_str());
	if (auto t = transcode_target(c, max_bitrate, format)) {
		el->SetAttribute("transcodedContentType", std::string(t->mime).c_str());
		el->SetAttribute("transcodedSuffix",      std::string(t->suffix).c_str());
		if (t->bitrate > 0)
			el->SetAttribute("transcodedBitRate", t->bitrate);
		}
	if (is_video)
		el->SetAttribute("nativeSeek",
		                 video_seeks_natively(c.video_codec, c.audio_codec));
	return el;
	}

Streamer::SongInfo streamer_song(const MediaStore::SongInfo& s,
                                 const std::string& abs,
                                 int64_t size_override)
	{
	Streamer::SongInfo si{ abs, s.codec, s.bitrate, s.duration,
	                       size_override > 0 ? size_override : s.file_size,
	                       s.id, s.file_modified };
	si.is_video    = s.is_video;
	si.width       = s.width;
	si.height      = s.height;
	si.video_codec = s.video_codec;
	si.audio_codec     = s.audio_codec;
	si.audio_container = s.audio_container;
	return si;
	}

// Looks up the authenticated user's max_bitrate so song entries can advertise
// the transcoded* fields.  Must be called only after check_auth has succeeded.
// Returns 0 (= unlimited / no transcode) if the user record can't be read.
int request_max_bitrate(const httplib::Request& req, MediaStore& store)
	{
	auto it = req.params.find("u");
	if (it == req.params.end()) return 0;
	auto u = store.get_user(it->second);
	return u ? u->max_bitrate : 0;
	}

// Builds the full subsonic response body for a playlist with its songs.
std::string playlist_body(const MediaStore::PlaylistInfo& pl, bool use_json,
                          int max_bitrate)
	{
	if (use_json)
		return subsonic_ok_json([&pl, max_bitrate](nlohmann::json& r) {
			nlohmann::json entries = nlohmann::json::array();
			for (auto& c : pl.songs)
				entries.push_back(song_entry_json(c, max_bitrate));
			r["playlist"] = {
				{"id",        sid(pl.id)},
				{"name",      pl.name},
				{"comment",   pl.comment},
				{"owner",     pl.owner},
				{"public",    pl.is_public},
				{"songCount", pl.song_count},
				{"duration",  pl.duration},
				{"created",   iso8601(pl.created)},
				{"changed",   iso8601(pl.updated)},
				{"entry",     entries}
				};
			});
	return subsonic_ok([&pl, max_bitrate](XMLDocument& doc, XMLElement* root) {
		auto* playlist = doc.NewElement("playlist");
		playlist->SetAttribute("id",        pl.id);
		playlist->SetAttribute("name",      pl.name.c_str());
		playlist->SetAttribute("comment",   pl.comment.c_str());
		playlist->SetAttribute("owner",     pl.owner.c_str());
		playlist->SetAttribute("public",    pl.is_public);
		playlist->SetAttribute("songCount", pl.song_count);
		playlist->SetAttribute("duration",  pl.duration);
		playlist->SetAttribute("created",   iso8601(pl.created).c_str());
		playlist->SetAttribute("changed",   iso8601(pl.updated).c_str());
		for (auto& c : pl.songs)
			playlist->InsertEndChild(song_entry_xml(doc, c, "entry", max_bitrate));
		root->InsertEndChild(playlist);
		});
	}

std::string sane_video_size(const std::string& s)
	{
	auto x = s.find('x');
	if (x == std::string::npos || x == 0 || x + 1 == s.size()) return "";
	if (s.size() > 11) return "";
	for (size_t i = 0; i < s.size(); ++i)
		if (i != x && !std::isdigit(static_cast<unsigned char>(s[i]))) return "";
	const int w = to_int(s.substr(0, x), 0);
	const int h = to_int(s.substr(x + 1), 0);
	if (w < 16 || h < 16 || w > 7680 || h > 4320) return "";
	return std::to_string(w) + "x" + std::to_string(h);
	}

// What a client declared it can be sent untouched, from the comma list
// stream.view spells `playable`.
//
// Two token shapes, and the split is by *medium* rather than by anything about
// the file:
//
//  * bare — a video container (every VIDEO_TARGETS name but vob). Video is a
//    container-only declaration by design: the server keeps its own codec test,
//    so declaring `mkv` widens which containers may be served untouched and
//    nothing else.
//  * `container/codec` — audio, always. Both halves are compared against what
//    the scan observed and stored, so there is no bare audio form: `mp3` and
//    `mpeg/mp3` would be two spellings of one thing, which is the class of bug
//    that made a `.oga` and a `.ogg` disagree about the same container.
//
// Validated here rather than in Streamer for the reason sane_video_size() above
// gives: the one caller reachable from outside is the one that checks. What is
// bounded is the whole parameter, at 128 characters — that caps the token count
// and every token length at once, so there are no separate counters to keep
// agreeing with each other. 128 rather than the 64 a container-only list needed:
// a realistic audio declaration runs to about seventy.
//
// An unrecognised token is **dropped, not refused**, matching `size`. A client
// naming something this server has never heard of is asking for nothing, not
// asking wrongly, and a hard error would make adding a format to a client a
// breaking change against every older server.
//
// **Neither half of a pair is checked against a vocabulary.** Both are compared
// for equality with what the scan stored, so a spelling this server does not use
// simply fails to match and the file transcodes as it always did — a better
// failure than two more tables to keep in step with ffprobe's names and with
// whatever container the next format turns out to be. What is checked is only
// that each half is *shaped* like one: non-empty, letters, digits and
// underscores. That keeps the set printable in a log line without log_safe(),
// and drops an "a/b/c" that names nothing.
Playable parse_playable(const std::string& s)
	{
	auto plain = [](const std::string& t) {
		return !t.empty()
		    && std::all_of(t.begin(), t.end(), [](unsigned char c) {
		       	return std::isalnum(c) || c == '_';
		       	});
		};

	Playable out;
	if (s.empty() || s.size() > 128) return out;
	size_t start = 0;
	while (start <= s.size()) {
		size_t      comma = s.find(',', start);
		std::string tok   = s.substr(start, comma == std::string::npos
		                                    ? std::string::npos
		                                    : comma - start);
		// Every column this is compared against is stored lowercased, so that
		// is what a declaration has to be folded to.
		for (char& c : tok)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		size_t slash = tok.find('/');
		if (slash == std::string::npos) {
			if (container_declarable(tok)) out.insert(std::move(tok));
			}
		else if (plain(tok.substr(0, slash)) && plain(tok.substr(slash + 1)))
			out.insert(std::move(tok));
		if (comma == std::string::npos) break;
		start = comma + 1;
		}
	return out;
	}

// Where one of getVideoInfo's caption tracks lives, as the API reports it:
// "sidecar" for the subtitle file beside the video, "container" for a stream
// inside it.  The same two words getChapters already uses for the same
// distinction.
//
// It exists because the answer stopped being the server's business alone. A
// client that demuxes the container itself (see `playable` on
// stream.view) is handed those streams by its own demuxer, so side-loading
// them through getCaptions as well would list every subtitle twice — while the
// sidecar, which no container carries, still has to come from here.
//
// A pure derivation: SIDECAR_CAPTION_INDEX is negative precisely so it can
// never be a stream index, which is the fact this reads.
const char* caption_source(const MediaStore::CaptionTrack& c)
	{
	return c.index < 0 ? "sidecar" : "container";
	}

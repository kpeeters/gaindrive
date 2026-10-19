#include "model.hh"
#include "jsonread.hh"

#include <nlohmann/json.hpp>

#include <glibmm/datetime.h>

#include <algorithm>
#include <cstdio>

Artist parse_artist(const std::string& server, const nlohmann::json& j, bool category)
	{
	Artist a;
	a.ref         = {server, jstr(j, "id")};
	a.name        = jstr(j, "name");
	a.cover_art   = jstr(j, "coverArt");
	a.album_count = jint(j, "albumCount");
	a.category    = category;
	return a;
	}

Album parse_album(const std::string& server, const nlohmann::json& j)
	{
	Album a;
	a.ref         = {server, jstr(j, "id")};
	// OpenSubsonic says `name`; `title` is what getArtist sent before it.
	a.title       = jstr(j, "name");
	if(a.title.empty())
		a.title    = jstr(j, "title");
	a.artist      = jstr(j, "artist");
	a.artist_id   = jstr(j, "artistId");
	if(a.artist_id.empty())
		a.artist_id = jstr(j, "parent");
	a.cover_art   = jstr(j, "coverArt");
	a.starred     = !jstr(j, "starred").empty();
	a.year        = jint(j, "year");
	a.song_count  = jint(j, "songCount");
	a.video_count = jint(j, "videoCount");
	return a;
	}

Song parse_song(const std::string& server, const nlohmann::json& j)
	{
	Song s;
	s.ref                  = {server, jstr(j, "id")};
	s.title                = jstr(j, "title");
	s.artist               = jstr(j, "artist");
	s.display_album_artist = jstr(j, "displayAlbumArtist");
	s.album                = jstr(j, "album");
	s.cover_art            = jstr(j, "coverArt");
	s.track                = jint(j, "track");
	s.disc                 = jint(j, "discNumber");
	if(s.disc==0) s.disc   = 1;
	s.season               = jint(j, "season");
	s.duration             = jint(j, "duration");
	s.album_id             = jstr(j, "albumId");
	if(s.album_id.empty())
		s.album_id          = jstr(j, "parent");
	s.last_played          = jstr(j, "lastPlayed");
	s.content_type         = jstr(j, "contentType");
	s.starred              = !jstr(j, "starred").empty();
	s.is_video             = jsub(j, "isVideo").is_boolean() && jsub(j, "isVideo").get<bool>();
	s.suffix               = jstr(j, "suffix");
	s.video_codec          = jstr(j, "videoCodec");
	s.audio_codec          = jstr(j, "audioCodec");
	s.transcoded_suffix    = jstr(j, "transcodedSuffix");
	return s;
	}

Playlist parse_playlist(const std::string& server, const nlohmann::json& j)
	{
	Playlist p;
	p.ref        = {server, jstr(j, "id")};
	p.name       = jstr(j, "name");
	p.owner      = jstr(j, "owner");
	p.comment    = jstr(j, "comment");
	p.song_count = jint(j, "songCount");
	p.duration   = jint(j, "duration");
	const auto& pub = jsub(j, "public");
	p.is_public  = pub.is_boolean() && pub.get<bool>();
	return p;
	}

std::string format_ago(const std::string& iso8601)
	{
	auto then = Glib::DateTime::create_from_iso8601(iso8601);
	if(!then) return "";
	auto mins = Glib::DateTime::create_now_utc().difference(then)/G_TIME_SPAN_MINUTE;
	if(mins<60) return std::to_string(std::max<long>(0, mins)) + "m ago";
	auto hrs = mins/60;
	if(hrs<24)  return std::to_string(hrs) + "h ago";
	return std::to_string(hrs/24) + "d ago";
	}

std::string plural(int n, const char *one, const char *many)
	{
	return std::to_string(n) + " " + (n==1 ? one : many);
	}

std::string format_duration(int seconds)
	{
	char buf[32];
	if(seconds>=3600)
		std::snprintf(buf, sizeof buf, "%d:%02d:%02d", seconds/3600, (seconds/60)%60, seconds%60);
	else
		std::snprintf(buf, sizeof buf, "%d:%02d", seconds/60, seconds%60);
	return buf;
	}

ListRow::ListRow(Kind k)
	: Glib::ObjectBase(typeid(ListRow)), kind(k)
	{
	}

Glib::RefPtr<ListRow> ListRow::heading(const std::string& text)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::heading));
	r->text = text;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::of(const Artist& a)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::artist));
	r->artist = a;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::of(const Album& a)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::album));
	r->album = a;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::of(const Song& s)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::song));
	r->song = s;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::of(const Playlist& p)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::playlist));
	r->playlist = p;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::chapter(const Song& film, int index, const std::string& name,
                                       double start, int duration)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::chapter));
	r->song     = film;
	r->index    = index;
	r->text     = name;
	r->start    = start;
	r->duration = duration;
	return r;
	}

Glib::RefPtr<ListRow> ListRow::setting(const std::string& key, const std::string& text,
                                       const std::string& detail)
	{
	auto r = Glib::make_refptr_for_instance<ListRow>(new ListRow(Kind::setting));
	r->key    = key;
	r->text   = text;
	r->detail = detail;
	return r;
	}

#include "model.hh"
#include "jsonread.hh"

#include <nlohmann/json.hpp>

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
	a.cover_art   = jstr(j, "coverArt");
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
	return s;
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

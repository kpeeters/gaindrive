#pragma once

#include <glibmm/object.h>
#include <nlohmann/json_fwd.hpp>

#include <string>

// Every id is qualified by the server it came from. Only one server is used
// for now, but Subsonic ids are only unique per server, and retrofitting the
// qualification once several servers are merged would touch everything.
struct ItemRef {
	std::string server;   // ServerConfig::key
	std::string id;
};

struct Artist {
	ItemRef     ref;
	std::string name;
	std::string cover_art;
	int         album_count=0;
	// A categories root lists sections (a radio show, a film collection)
	// rather than musicians.
	bool        category=false;
};

struct Album {
	ItemRef     ref;
	std::string title;
	std::string artist;
	std::string cover_art;
	int         year=0;
	int         song_count=0;
	int         video_count=0;
};

struct Song {
	ItemRef     ref;
	std::string title;
	std::string artist;
	std::string display_album_artist;
	std::string album;
	std::string cover_art;
	int         track=0;
	int         disc=1;
	int         season=0;
	int         duration=0;   // seconds
};

Artist parse_artist(const std::string& server, const nlohmann::json& j, bool category);
Album  parse_album(const std::string& server, const nlohmann::json& j);
Song   parse_song(const std::string& server, const nlohmann::json& j);

// "3:07", or "1:02:03" past the hour.
std::string format_duration(int seconds);

// One row of a pane's list: a heading, or one of the three kinds of item.
// A single class rather than one per kind, because a pane's ListView holds
// headings and items side by side.
class ListRow : public Glib::Object {
	public:
		enum class Kind { heading, artist, album, song };

		static Glib::RefPtr<ListRow> heading(const std::string& text);
		static Glib::RefPtr<ListRow> of(const Artist& a);
		static Glib::RefPtr<ListRow> of(const Album& a);
		static Glib::RefPtr<ListRow> of(const Song& s);

		Kind        kind;
		// The heading's text; for a song, the number shown before it.
		std::string text;
		Artist      artist;
		Album       album;
		Song        song;

	protected:
		explicit ListRow(Kind k);
};

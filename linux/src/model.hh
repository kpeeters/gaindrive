#pragma once

#include <glibmm/object.h>
#include <nlohmann/json_fwd.hpp>

#include <string>
#include <vector>

// Every id is qualified by the server it came from: Subsonic ids are only
// unique per server. Ids inside an item (an album's artist_id, a song's
// album_id and cover_art) are on the item's own server.
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
	// The same artist on the servers after `ref`'s, when merged.
	std::vector<ItemRef> also;
};

struct Album {
	ItemRef     ref;
	std::string title;
	std::string artist;
	std::string artist_id;
	std::string cover_art;
	int         year=0;
	int         song_count=0;
	int         video_count=0;
	bool        starred=false;
	// The server's answer to "may this account edit it", from getAlbum only.
	bool        writable=false;
	// The same album on the servers after `ref`'s, when merged.
	std::vector<ItemRef> also;
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
	int         year=0;
	int         duration=0;   // seconds
	std::string album_id;
	std::string last_played;  // ISO 8601, from getRecentSongs only
	std::string content_type; // of the stored file
	bool        starred=false;
	// Films only. The codecs are ffprobe's names, empty until the server's
	// scan has probed the file; transcoded_suffix is set when a browser
	// would be sent a conversion.
	bool        is_video=false;
	std::string suffix;
	std::string video_codec;
	std::string audio_codec;
	std::string transcoded_suffix;
	// This user's mark and resume point; only the album and playlist
	// listings carry them.
	bool        watched=false;
	long        bookmark_ms=0;

	// Whether the server keeps a resume point and a watched mark: a video,
	// or audio long enough to be a chapter or a mix. Its own rule, mirrored
	// so an ordinary song costs no request.
	bool resumable() const { return is_video || duration>=20*60; }
	// How far it was left, 0..1; 0 once watched or before it is started.
	double watch_progress() const;
};

struct Playlist {
	ItemRef     ref;
	std::string name;
	std::string owner;
	std::string comment;
	int         song_count=0;
	int         duration=0;
	bool        is_public=false;
};

Artist parse_artist(const std::string& server, const nlohmann::json& j, bool category);
Album  parse_album(const std::string& server, const nlohmann::json& j);
Song     parse_song(const std::string& server, const nlohmann::json& j);
Playlist parse_playlist(const std::string& server, const nlohmann::json& j);

// Does to `s` what the server does with a playback report at `pos` seconds,
// so a listing follows playback without a reload. False when nothing changed.
bool note_watch(Song& s, double pos);
// What setWatched does to `s` on the server.
void set_watched(Song& s, bool on);

// "3:07", or "1:02:03" past the hour.
std::string format_duration(int seconds);
// "5m ago", "3h ago", "2d ago", as the web client words it.
std::string format_ago(const std::string& iso8601);
std::string plural(int n, const char *one, const char *many);

// One row of a pane's list: a heading, or one of the three kinds of item.
// A single class rather than one per kind, because a pane's ListView holds
// headings and items side by side.
class ListRow : public Glib::Object {
	public:
		// `extra` is a pane's own widget, given a row so it scrolls with
		// the list.
		enum class Kind { heading, artist, album, song, playlist, setting, chapter, extra };

		static Glib::RefPtr<ListRow> heading(const std::string& text);
		static Glib::RefPtr<ListRow> extra();
		static Glib::RefPtr<ListRow> of(const Artist& a);
		static Glib::RefPtr<ListRow> of(const Album& a);
		static Glib::RefPtr<ListRow> of(const Song& s);
		static Glib::RefPtr<ListRow> of(const Playlist& p);
		// A settings category: `key` names it, `text` and `detail` show it.
		static Glib::RefPtr<ListRow> setting(const std::string& key, const std::string& text,
		                                     const std::string& detail);
		// One chapter of `film`, listed under it: `index` counts from 1 and
		// `text` is the name.
		static Glib::RefPtr<ListRow> chapter(const Song& film, int index, const std::string& name,
		                                     double start, int duration);

		Kind        kind;
		// The heading's text; for a song, the number shown before it.
		std::string text;
		std::string detail;
		std::string key;
		// A song's place in the list it was listed in, for a playlist's
		// remove-by-index.
		int         index=-1;
		// A chapter's place in its film, in seconds.
		double      start=0;
		int         duration=0;
		Artist      artist;
		Album       album;
		Song        song;
		Playlist    playlist;

	protected:
		explicit ListRow(Kind k);
};

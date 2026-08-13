#pragma once

#include <string>
#include <string_view>

// What a video's filename says it is.
//
// Video containers carry no tag anything writes, so the name is the only
// metadata there is — and it is usually a name meant for a torrent tracker
// rather than for a person: "The.Third.Man.1949.1080p.BluRay.x264-GRP".  This
// turns that back into a title and a year, which is what a client should show
// and what an online provider has to be asked with.
//
// Pure string work: no database, no filesystem, no ffmpeg.  That is deliberate
// — these rules get tuned repeatedly against real filenames, and they are worth
// being able to test by piping a list through --video-name-test.
//
// **When nothing matches, the name comes back unchanged** with `cleaned`
// false.  That is what makes this safe to run over a whole mixed library: a
// tidily named collection is a no-op, and a concert folder sitting under a
// musician in the music root is left exactly as it was.
struct VideoName
	{
	std::string title;          // cleaned; the raw name when nothing matched
	int         year    = 0;
	int         season  = 0;    // 0 = not an episode
	int         episode = 0;
	std::string episode_title;  // "Jungles" from ...S01E03.Jungles.1080p
	std::string series_title;   // for an episode: the show
	std::string tmdb_id;        // from an explicit [tmdbid=550] override
	std::string imdb_id;        // from [imdbid=tt0090605]
	bool        cleaned     = false;  // false = nothing matched, name is as-is
	bool        from_folder = false;  // title came from the folder, not the file
	};

// One name, parsed on its own.  `name` is a bare name — no directory, no
// extension.
VideoName parse_video_name(std::string_view name);

// The arbitration a scanner needs.  `folder_name` is the directory holding the
// file and `parent_name` the one above it, which is where the show's name sits
// when the folder is "Season 01".  Falls back to the folder when the filename
// says nothing useful, which is what makes "The Third Man (1949)/title00.mkv"
// work.
VideoName resolve_video_name(std::string_view file_stem,
                             std::string_view folder_name = "",
                             std::string_view parent_name = "");

#include "videoname.hh"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <vector>

// Tokens that mark the end of a title.  Everything from the first of these
// onwards describes the *file* rather than the film, so the title is whatever
// came before.  This is the whole trick, and it is the list that will need
// extending as odd releases turn up — which is what --video-name-test is for.
//
// Kept as one table rather than several so there is one place to add to.
static const std::set<std::string> STOP_WORDS = {
	// source
	"bluray", "blu-ray", "brrip", "bdrip", "bdremux", "remux", "webrip",
	"web-dl", "webdl", "web", "hdtv", "pdtv", "dvdrip", "dvdscr", "dvd",
	"hdrip", "hdcam", "cam", "ts", "telesync", "r5", "vhsrip", "sdtv",
	// video codec
	"x264", "x265", "h264", "h265", "avc", "hevc", "xvid", "divx", "mpeg2",
	"vp9", "av1",
	// audio
	"aac", "aac2", "ac3", "eac3", "dts", "dtshd", "truehd", "atmos", "flac",
	"mp3", "opus", "dd", "ddp", "dd5", "ddp5", "dolby", "lpcm", "pcm",
	// dynamic range / picture
	"hdr", "hdr10", "hdr10plus", "dovi", "dv", "sdr", "10bit", "8bit",
	"imax", "uhd", "hd", "sd", "3d", "hsbs", "sbs",
	// edition and status
	"proper", "repack", "rerip", "internal", "limited", "extended", "unrated",
	"uncut", "remastered", "restored", "criterion", "theatrical", "directors",
	"dc", "final", "complete",
	// language / subs
	"multi", "dual", "dubbed", "subbed", "subs", "sub", "eng", "english",
	"nl", "dutch", "ger", "german", "fre", "french", "spa", "spanish",
	"ita", "italian", "nordic", "retail",
	// release plumbing
	"repost", "nfo", "readnfo", "untouched",
	};

// Rip defaults and part markers: a filename that is only one of these says
// nothing, so the folder above is the better source.  Anchored, because a film
// really can be called "Video" or "The Movie".
static const std::regex UNINFORMATIVE(
	R"(^(video_?ts|vts([\s._-]*\d+)+|title[\s._-]*\d*|track[\s._-]*\d*|)"
	R"(movie|video|main|index|film|stream|playlist|)"
	R"(disc[\s._-]*\d*|dvd[\s._-]*\d*|part[\s._-]*\d*|pt[\s._-]*\d*|)"
	R"(cd[\s._-]*\d*|\d+)$)",
	std::regex::icase);

// A folder that is a season or disc rather than the show itself.
static const std::regex SEASON_FOLDER(
	R"(^(season|series|seizoen|staffel|s|disc|disk|cd)[\s._-]*\d{1,3}$)",
	std::regex::icase);

// A folder that says which *season* it is, with the number captured.  The
// season words only: "Disc 2" and "CD1" are excluded deliberately, and that
// exclusion is the whole point of having a second regex. Above, a folder is
// being classified as "not the show's name", for which a disc counts; here it
// is being asked for a season number, and answering 2 for a two-disc film
// would label it as a series.
static const std::regex SEASON_FOLDER_NUM(
	R"(^(?:season|series|seizoen|staffel|s)[\s._-]*(\d{1,3})$)",
	std::regex::icase);

static std::string to_lower(std::string s)
	{
	std::transform(s.begin(), s.end(), s.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return s;
	}

// A token as the stop-word table wants to see it: lowercased, stripped of
// surrounding punctuation, and cut at a '-' so "x264-GRP" tests as "x264".
// Splitting the token itself on '-' is wrong — "Spider-Man" is one word — but
// testing only the part before the dash is safe, because a real title's first
// hyphenated half is never a stop word.
static std::string stop_key(const std::string& token)
	{
	std::string t = to_lower(token);
	auto dash = t.find('-');
	if (dash != std::string::npos && dash > 0) t = t.substr(0, dash);
	while (!t.empty() && !std::isalnum(static_cast<unsigned char>(t.front())))
		t.erase(0, 1);
	while (!t.empty() && !std::isalnum(static_cast<unsigned char>(t.back())))
		t.pop_back();
	return t;
	}

static bool is_stop_word(const std::string& token)
	{
	std::string t = stop_key(token);
	if (t.empty()) return true;                    // punctuation only
	if (STOP_WORDS.count(t)) return true;
	// Patterns the table cannot hold as literals.
	static const std::regex pat(
		R"(^(\d{3,4}p|\d{3,4}i|4k|\d+bit|[hx]26\d|aac\d|ac3\d|dts\-?hd|)"
		R"(dd\d?|ddp\d?|\d+ch|s\d{1,2}e\d{1,3}|)"
		R"((cd|disc|disk|part|pt|dvd)\d{1,2})$)",
		std::regex::icase);
	return std::regex_match(t, pat);
	}

static bool is_year_token(const std::string& token, int& out)
	{
	static const std::regex year(R"(^(19|20)\d{2}$)");
	std::string t = stop_key(token);
	if (!std::regex_match(t, year)) return false;
	out = std::stoi(t);
	return true;
	}

// Splits on the three characters release names use interchangeably as spaces.
// '-' is deliberately not one of them: it joins a group tag on the end, which
// stop_key() handles, and it also joins real words ("Spider-Man").
static std::vector<std::string> tokenise(const std::string& s)
	{
	std::vector<std::string> out;
	std::string cur;
	// A token of pure punctuation — the lone "-" in "2x05 - Breakage" — is
	// dropped rather than kept. Keeping it would end the title at the dash,
	// since a token with nothing alphanumeric in it reads as a stop word.
	auto flush = [&] {
		if (std::any_of(cur.begin(), cur.end(),
		        [](unsigned char c) { return std::isalnum(c); }))
			out.push_back(cur);
		cur.clear();
		};
	for (char c : s) {
		if (c == '.' || c == '_' || std::isspace(static_cast<unsigned char>(c)))
			flush();
		else cur += c;
		}
	flush();
	return out;
	}

static std::string join(const std::vector<std::string>& tokens, size_t upto)
	{
	std::string out;
	for (size_t i = 0; i < upto && i < tokens.size(); ++i) {
		if (!out.empty()) out += ' ';
		out += tokens[i];
		}
	return out;
	}

// Trailing separators, and the group tag scene releases end with.  The tag is
// only stripped from a name that has more than one word: "X-MEN" on its own
// would otherwise lose half of itself, while "The.Third.Man.1949-GRP" is
// unambiguous.
static std::string tidy(const std::string& s)
	{
	std::string out = s;
	bool multiword = out.find(' ') != std::string::npos;
	if (multiword) {
		static const std::regex group(R"(-[A-Z0-9]{2,}$)");
		out = std::regex_replace(out, group, "");
		}
	static const std::regex edges(R"(^[\s\-_.]+|[\s\-_.]+$)");
	out = std::regex_replace(out, edges, "");
	static const std::regex spaces(R"(\s{2,})");
	out = std::regex_replace(out, spaces, " ");
	return out;
	}

// Title, year and the cut point, for one run of text.  Shared by the whole
// name and by the tail after an episode marker.
static std::string clean_run(const std::string& text, int& year)
	{
	auto tokens = tokenise(text);
	if (tokens.empty()) return "";

	size_t cut = tokens.size();

	// The first stop word ends the title.
	for (size_t i = 0; i < tokens.size(); ++i)
		if (is_stop_word(tokens[i])) { cut = i; break; }

	// Unless it is the *first* word, in which case it is not junk at all —
	// "4K Nature Scenes" and "HD Home Video" are titles that happen to open
	// with a word the table calls release junk. Cutting there would leave
	// nothing, and no name is improved by being emptied.
	if (cut == 0) cut = tokens.size();

	// A bare year ends the title too. The reliable signal is a year sitting
	// immediately before the junk: "Arrival 2016 | 2160p UHD ... DTS-HD MA 7 1"
	// is a year even though "MA" is not in the stop-word table, and no amount
	// of adding audio tokens would ever make that test complete.
	//
	// Taking the year *nearest the junk* rather than the first one is also
	// what makes "Blade.Runner.2049.2017.1080p" come out as *Blade Runner
	// 2049* released in 2017 rather than *Blade Runner* released in 2049.
	// A year already lifted out of brackets is authoritative, and no token in
	// the title can be a second one. Without this, "1917 (2019)" loses its
	// entire title to the year rule below.
	if (year != 0) return tidy(join(tokens, cut));

	int y = 0;
	if (cut > 0 && is_year_token(tokens[cut - 1], y)) {
		if (year == 0) year = y;
		cut--;
		}
	else {
		// No junk to anchor against — a trailing year, as in "Film.1080p.1949".
		// Here the year must have nothing but junk after it, which is what
		// keeps "holiday 2019 crete" whole: a real word follows its 2019.
		for (size_t i = tokens.size(); i-- > 0; ) {
			if (!is_year_token(tokens[i], y)) continue;
			bool rest_is_junk = true;
			for (size_t j = i + 1; j < tokens.size(); ++j)
				if (!is_stop_word(tokens[j])) { rest_is_junk = false; break; }
			if (!rest_is_junk) continue;
			if (year == 0) year = y;
			cut = std::min(cut, i);
			break;
			}
		}

	return tidy(join(tokens, cut));
	}

VideoName parse_video_name(std::string_view name)
	{
	VideoName out;
	std::string s(name);
	const std::string original = tidy(s);

	// ---- explicit overrides, lifted out before anything else ----
	// These are the escape hatch: a file the rules get wrong can be fixed by
	// renaming it, without the rules having to grow a special case. Jellyfin
	// reads the same markers.
	std::smatch m;
	static const std::regex tmdb_re(R"(\[tmdbid[=-](\d+)\])", std::regex::icase);
	if (std::regex_search(s, m, tmdb_re)) {
		out.tmdb_id = m[1];
		s = std::regex_replace(s, tmdb_re, " ");
		}
	static const std::regex imdb_re(R"(\[imdbid[=-](tt\d+)\])", std::regex::icase);
	if (std::regex_search(s, m, imdb_re)) {
		out.imdb_id = m[1];
		s = std::regex_replace(s, imdb_re, " ");
		}
	// A bracketed year is unambiguous wherever it sits, so it wins over every
	// rule below and is removed before the token scan can mistake it.
	static const std::regex paren_year(R"([\(\[]((?:19|20)\d{2})[\)\]])");
	if (std::regex_search(s, m, paren_year)) {
		out.year = std::stoi(m[1]);
		s = std::regex_replace(s, paren_year, " ");
		}

	// ---- noise ----
	static const std::regex tracker(R"(^\s*(www\.)?[\w-]+\.(com|net|org|to|me)\s*-\s*)",
	                                 std::regex::icase);
	s = std::regex_replace(s, tracker, "");
	static const std::regex brackets(R"(\[[^\]]*\]|\{[^}]*\})");
	s = std::regex_replace(s, brackets, " ");

	// ---- episode markers ----
	// Only the unambiguous forms split the title. A leading number is handled
	// further down and deliberately does *not*: "12 Angry Men" is a film, and
	// letting a leading number claim the title would rename it to "Angry Men".
	static const std::regex sxxeyy(R"([Ss](\d{1,2})[\s._-]?[Ee](\d{1,3}))");
	static const std::regex nxnn  (R"((?:^|[\s._-])(\d{1,2})[Xx](\d{2})(?:[\s._-]|$))");
	static const std::regex worded(
		R"([Ss]eason[\s._-]*(\d{1,2})[\s._-]*[Ee]pisode[\s._-]*(\d{1,3}))",
		std::regex::icase);

	std::smatch em;
	bool matched_episode =
	       std::regex_search(s, em, sxxeyy)
	    || std::regex_search(s, em, worded)
	    || std::regex_search(s, em, nxnn);

	if (matched_episode) {
		out.season  = std::stoi(em[1]);
		out.episode = std::stoi(em[2]);
		int before_year = 0, after_year = 0;
		out.title         = clean_run(em.prefix().str(), before_year);
		out.episode_title = clean_run(em.suffix().str(), after_year);
		if (out.year == 0) out.year = before_year ? before_year : after_year;
		out.cleaned = true;
		// Both empty is "S01E03.mkv" — a real and common shape. Leave them
		// empty rather than echoing the marker back as a title, so
		// resolve_video_name() can reach for the folder and for "Episode 3".
		return out;
		}

	int year = out.year;
	out.title = clean_run(s, year);
	out.year  = year;

	// A leading number orders episodes that carry no other marker. It sets the
	// episode number only — see the note above.
	static const std::regex leading(R"(^(\d{1,3})[\s._-]+\S)");
	if (std::regex_search(s, m, leading)) out.episode = std::stoi(m[1]);

	// A leading number deliberately does not count as having cleaned anything:
	// it leaves the title alone, so "12 Angry Men" is reported as-is even
	// though 12 is now ordering it.
	out.cleaned = out.title != original || out.year != 0
	           || !out.tmdb_id.empty() || !out.imdb_id.empty();
	// Nothing matched: hand back exactly what came in. This is the property
	// that makes the parser safe to run over a whole mixed library.
	if (!out.cleaned) out.title = original;
	return out;
	}

static bool uninformative(std::string_view stem)
	{
	std::string s = tidy(std::string(stem));
	if (s.size() < 3) return true;
	if (std::regex_match(s, UNINFORMATIVE)) return true;
	// No letters at all is a rip default or a timestamp, never a title.
	return std::none_of(s.begin(), s.end(),
		[](unsigned char c) { return std::isalpha(c); });
	}

VideoName resolve_video_name(std::string_view file_stem,
                             std::string_view folder_name,
                             std::string_view parent_name)
	{
	VideoName file = parse_video_name(file_stem);

	// The show's name is on a folder, and which folder depends on whether the
	// one directly above is a season.
	std::string show_dir(folder_name);
	if (std::regex_match(show_dir, SEASON_FOLDER) && !parent_name.empty())
		show_dir = std::string(parent_name);
	VideoName folder = parse_video_name(show_dir);

	// "Planet Earth/Season 2/03 Jungles.mkv" — the episodes of a season are
	// regularly named without repeating the season, so the folder is the only
	// thing that says which one it is. Used below wherever the file itself did
	// not say; a file that carries S03E01 while sitting in "Season 2" is a
	// misfiled episode, and its own name is the more specific claim.
	int folder_season = 0;
	std::smatch fm;
	std::string fname(folder_name);
	if (std::regex_match(fname, fm, SEASON_FOLDER_NUM))
		folder_season = std::stoi(fm[1]);

	// season > 0 means one of the unambiguous markers matched, not the leading
	// number, which never claims a title.
	if (file.season > 0) {
		file.series_title = !file.title.empty() ? file.title : folder.title;
		// What a client should show for an episode is the episode's own name;
		// the show is already the album it sits in. With no name in the file
		// at all — "S01E03.mkv" — "Episode 3" beats both echoing the marker
		// back and repeating the show's name on every row.
		if (!file.episode_title.empty())
			file.title = file.episode_title;
		else
			file.title = "Episode " + std::to_string(file.episode);
		if (file.year == 0) file.year = folder.year;
		return file;
		}

	// A film whose filename says nothing: the folder is the better source.
	if (uninformative(file_stem) || file.title.empty()) {
		if (!folder.title.empty() && !uninformative(show_dir)) {
			VideoName out = folder;
			out.from_folder = true;
			out.cleaned     = true;
			// A part number on the file still orders the parts.
			if (out.episode == 0) out.episode = file.episode;
			if (out.year    == 0) out.year    = file.year;
			if (out.tmdb_id.empty()) out.tmdb_id = file.tmdb_id;
			if (out.imdb_id.empty()) out.imdb_id = file.imdb_id;
			if (out.season == 0) {
				out.season       = folder_season;
				out.series_title = folder.title;
				}
			return out;
			}
		}

	// A year on the folder still counts for a file that has none of its own —
	// "The Third Man (1949)/third man.mkv".
	if (file.year == 0 && folder.year != 0) {
		file.year    = folder.year;
		file.cleaned = true;
		}
	if (file.season == 0 && folder_season > 0) {
		file.season       = folder_season;
		file.series_title = folder.title;
		}
	return file;
	}

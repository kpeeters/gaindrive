#include "tmdb.hh"
#include "stamp.hh"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <thread>

#include <nlohmann/json.hpp>
#include <httplib.h>

// Sent on every outbound request. Kept in step with the one in gaindrive.cc by
// being built from the same macro.
static const char* TMDB_USER_AGENT =
	"GainDrive/" GAINDRIVE_VERSION " (https://github.com/kpeeters/gaindrive)";

static const char* TMDB_HOST  = "api.themoviedb.org";
static const char* IMAGE_HOST = "image.tmdb.org";

// Poster width. w500 is 60-100 kB of JPEG, which needs no re-encoding to sit
// beside the 640 px long edge the local art tiers produce, and is more than
// the web client asks for at any size.
static const char* POSTER_SIZE = "w500";

// Minimum gap between requests. TMDB has relaxed its published rate limits,
// but a first scan of a large library is a long burst from one address and
// there is nothing to gain by hurrying it — the scan is already in the
// background.
static constexpr auto REQUEST_GAP = std::chrono::milliseconds(250);

// A scan must not stall on a provider that has stopped answering. These are
// per-request; the caller records the failure and moves on to the next file.
static constexpr time_t CONNECT_TIMEOUT_S = 10;
static constexpr time_t READ_TIMEOUT_S    = 20;

// How many search results to consider before giving up. TMDB sorts by
// popularity, and the right answer for a badly named file is regularly not
// first — "Zulu" (1964) sits behind "Zulu" (2013).
static constexpr int MAX_CANDIDATES = 10;

// Titles compared for equality with case, spacing and punctuation removed, so
// "wall e" matches TMDB's "WALL·E".
//
// Non-ASCII bytes are dropped rather than kept, which is weakly better in
// every case: when both sides carry the same accents they still agree, and
// when they do not, keeping the bytes would not have made them agree either.
// It does mean a title whose accents the filename lost — "Amelie" against
// "Amélie" — will not match here, and is left to be rejected. That is the
// safe direction, and it is what an explicit [tmdbid=…] in the filename is
// for. Real Unicode folding would need a dependency this does not justify.
static std::string fold(const std::string& s)
	{
	std::string out;
	for (unsigned char c : s)
		if (std::isalnum(c)) out += static_cast<char>(std::tolower(c));
	return out;
	}

// "1949-03-02" -> 1949. Anything else -> 0.
static int year_of(const std::string& date)
	{
	if (date.size() < 4) return 0;
	try { return std::stoi(date.substr(0, 4)); }
	catch (...) { return 0; }
	}

// Absent and null must mean the same thing here, and nlohmann's value() does
// not: it substitutes the default only when the key is *missing*, and throws
// type_error.302 when the key is present carrying null. TMDB sends null rather
// than omitting — a search result with no poster is "poster_path": null, and a
// film with no announced date is "release_date": null — so value() aborted a
// whole scan on the first such result. These check the type instead.
static std::string str_of(const nlohmann::json& j, const char* key)
	{
	auto it = j.find(key);
	if (it == j.end() || !it->is_string()) return {};
	return it->get<std::string>();
	}

static int int_of(const nlohmann::json& j, const char* key)
	{
	auto it = j.find(key);
	if (it == j.end() || !it->is_number_integer()) return 0;
	return it->get<int>();
	}

static TmdbMatch match_from(const nlohmann::json& j, bool tv)
	{
	TmdbMatch m;
	m.is_tv       = tv;
	m.id          = int_of(j, "id");
	m.title       = tv ? str_of(j, "name")  : str_of(j, "title");
	m.year        = year_of(tv ? str_of(j, "first_air_date")
	                           : str_of(j, "release_date"));
	m.overview    = str_of(j, "overview");
	m.poster_path = str_of(j, "poster_path");
	return m;
	}

Tmdb::Tmdb(std::string api_key)
	: api_key_(std::move(api_key))
	{
	}

void Tmdb::set_api_key(std::string api_key)
	{
	std::lock_guard<std::mutex> lock(mu_);
	api_key_ = std::move(api_key);
	}

bool Tmdb::configured() const
	{
	std::lock_guard<std::mutex> lock(mu_);
	return !api_key_.empty();
	}

void Tmdb::pace() const
	{
	std::lock_guard<std::mutex> lock(mu_);
	auto now = std::chrono::steady_clock::now();
	if (last_request_.time_since_epoch().count() != 0) {
		auto since = now - last_request_;
		if (since < REQUEST_GAP) std::this_thread::sleep_for(REQUEST_GAP - since);
		}
	last_request_ = std::chrono::steady_clock::now();
	}

std::optional<std::string> Tmdb::get(const std::string& path,
                                      const std::string& query) const
	{
	pace();

	httplib::SSLClient cli(TMDB_HOST);
	cli.set_default_headers({ { "User-Agent", TMDB_USER_AGENT } });
	cli.set_connection_timeout(CONNECT_TIMEOUT_S);
	cli.set_read_timeout(READ_TIMEOUT_S);

	std::string key;
	{
	std::lock_guard<std::mutex> lock(mu_);
	key = api_key_;
	}
	std::string url = path + "?api_key=" + key
	                + (query.empty() ? "" : "&" + query);
	auto r = cli.Get(url.c_str());
	if (!r) {
		std::cout << stamp() << "tmdb: no response for " << path << std::endl;
		return std::nullopt;
		}
	if (r->status == 401) {
		// Worth its own line: every lookup will fail the same way, and the
		// cause is a setting rather than anything about the file.
		std::cout << stamp() << "tmdb: HTTP 401 — the API key is not accepted"
		          << std::endl;
		return std::nullopt;
		}
	if (r->status != 200) {
		std::cout << stamp() << "tmdb: HTTP " << r->status << " for " << path
		          << std::endl;
		return std::nullopt;
		}
	return r->body;
	}

static std::optional<TmdbMatch> pick_impl(const std::string& results_json,
                                           const std::string& title, int year,
                                           bool tv)
	{
	auto j = nlohmann::json::parse(results_json, nullptr, false);
	if (j.is_discarded() || !j.contains("results") || !j["results"].is_array())
		return std::nullopt;

	const auto& results = j["results"];
	int n = std::min(static_cast<int>(results.size()), MAX_CANDIDATES);

	if (year > 0) {
		// A year within one is a match. The gap is normal — TMDB records the
		// release date, and a filename usually carries the production year.
		for (int i = 0; i < n; ++i) {
			TmdbMatch m = match_from(results[i], tv);
			if (m.year > 0 && std::abs(m.year - year) <= 1) return m;
			}
		std::cout << stamp() << "tmdb: no year match for \"" << title << "\" ("
		          << year << ") in " << n << " result(s)" << std::endl;
		return std::nullopt;
		}

	// With no year there is nothing to verify against but the title itself, so
	// the bar is an exact one. Anything looser confidently attaches the wrong
	// plot and poster to a vaguely named file, and nothing in the UI would
	// signal that it is wrong — which is worse than leaving it bare.
	std::string want = fold(title);
	for (int i = 0; i < n; ++i) {
		TmdbMatch m = match_from(results[i], tv);
		if (fold(m.title) == want) return m;
		}
	std::cout << stamp() << "tmdb: no exact title match for \"" << title
	          << "\" (no year to check against) in " << n << " result(s)"
	          << std::endl;
	return std::nullopt;
	}

// This class is the whole boundary with the provider's JSON, so a surprise in
// it must cost one file rather than the scan. It already treats a network
// error and a 404 as "no match"; an unreadable body is the same answer. The
// catch is here and not around the caller so that whatever a future field does
// cannot escape into the scanner, which runs in a detached thread.
std::optional<TmdbMatch> tmdb_pick(const std::string& results_json,
                                    const std::string& title, int year,
                                    bool tv)
	{
	try {
		return pick_impl(results_json, title, year, tv);
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "tmdb: unreadable search response for \""
		          << title << "\": " << e.what() << std::endl;
		return std::nullopt;
		}
	}

std::optional<TmdbMatch> Tmdb::search(const std::string& title, int year,
                                       bool tv) const
	{
	if (!configured() || title.empty()) return std::nullopt;

	// The year is deliberately *not* sent as a query parameter. TMDB treats it
	// as a hard filter, so a film whose release year differs from the one in
	// the filename by one — a festival year against a general release, which
	// is common — would come back empty rather than come back close.
	//
	// encode_query_param, not encode_url: the latter leaves '&' and '=' alone,
	// which a film title can contain. It lives in httplib's detail namespace,
	// which is safe here only because third_party/httplib.h is pinned by being
	// copied rather than by a version range — see CLAUDE.md.
	std::string q = "query=" + httplib::detail::encode_query_param(title)
	              + "&include_adult=false";
	auto body = get(std::string("/3/search/") + (tv ? "tv" : "movie"), q);
	if (!body) return std::nullopt;

	return tmdb_pick(*body, title, year, tv);
	}

std::optional<TmdbMatch> Tmdb::by_id(int id, bool tv) const
	{
	if (!configured() || id <= 0) return std::nullopt;

	auto body = get(std::string("/3/") + (tv ? "tv/" : "movie/")
	                + std::to_string(id), "");
	if (!body) return std::nullopt;

	try {
		auto j = nlohmann::json::parse(*body, nullptr, false);
		if (j.is_discarded() || !j.contains("id")) return std::nullopt;
		return match_from(j, tv);
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "tmdb: unreadable detail response for id " << id
		          << ": " << e.what() << std::endl;
		return std::nullopt;
		}
	}

std::optional<std::string> Tmdb::poster(const std::string& poster_path) const
	{
	if (!configured() || poster_path.empty()) return std::nullopt;

	pace();
	httplib::SSLClient cli(IMAGE_HOST);
	cli.set_default_headers({ { "User-Agent", TMDB_USER_AGENT } });
	cli.set_connection_timeout(CONNECT_TIMEOUT_S);
	cli.set_read_timeout(READ_TIMEOUT_S);

	std::string url = std::string("/t/p/") + POSTER_SIZE + poster_path;
	auto r = cli.Get(url.c_str());
	if (!r || r->status != 200 || r->body.empty()) {
		std::cout << stamp() << "tmdb: poster fetch failed for " << poster_path
		          << (r ? " (HTTP " + std::to_string(r->status) + ")" : "")
		          << std::endl;
		return std::nullopt;
		}
	return r->body;
	}

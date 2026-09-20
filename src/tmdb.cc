#include "tmdb.hh"
#include "stamp.hh"
#include "jsonread.hh"
#include "untrusted.hh"

#include <algorithm>
#include <cctype>
#include <exception>
#include <iostream>
#include <memory>
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

// The client for one host, built on first use and then kept.
//
// **set_keep_alive(true) is the whole of this**, and its absence would have
// made holding a client do nothing at all: httplib's keep_alive_ defaults to
// false, in which case it puts `Connection: close` on every request and the
// server hangs up after each one.  Holding the object would then have kept a
// socket that was already dead and reconnected anyway, which looks identical
// from here and costs exactly what it cost before.
//
// A keep-alive socket does die — the peer or something between it and us can
// close it while nothing is in flight — and httplib handles that itself: it
// polls the socket before writing, and closes and reconnects when it finds it
// gone.  So there is no retry to add here.
//
// Must be called with mu_ held.  The reference stays valid after the lock is
// released because a slot is filled once and never reset, which is what lets
// the request itself happen outside the lock.
static httplib::SSLClient& client_for(
	std::unique_ptr<httplib::SSLClient>& slot, const char* host)
	{
	if (!slot) {
		slot = std::make_unique<httplib::SSLClient>(host);
		slot->set_keep_alive(true);
		slot->set_default_headers({ { "User-Agent", TMDB_USER_AGENT } });
		slot->set_connection_timeout(CONNECT_TIMEOUT_S);
		slot->set_read_timeout(READ_TIMEOUT_S);
		}
	return *slot;
	}

// How many search results to consider before giving up. TMDB sorts by
// popularity, and the right answer for a badly named file is regularly not
// first — "Zulu" (1964) sits behind "Zulu" (2013).
static constexpr int MAX_CANDIDATES = 10;

// Containment only counts when the shorter folded title is at least this
// long. Below that, containment is coincidence: "a" is inside almost every
// title there is.
static constexpr size_t MIN_CONTAIN_FOLD = 5;

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

// Every field here goes through jsonread.hh, never through value(): TMDB
// sends null rather than omitting — a search result with no poster is
// "poster_path": null, and a film with no announced date is
// "release_date": null — and value() throws on that, which aborted a whole
// scan on the first such result. The rule and the rest of the reasoning are
// at the definitions.
static TmdbMatch match_from(const nlohmann::json& j, bool tv)
	{
	TmdbMatch m;
	m.is_tv       = tv;
	m.id          = jint(j, "id");
	// Cleaned here because this is where both shapes land — the search result
	// and the detail body — so one site covers every route a match arrives by.
	// untrusted.hh says what a control character costs in each response
	// format; the title is the sharper case of the two, since it overwrites
	// albums.title and songs.title and so appears as an XML *attribute* in
	// every browse listing rather than only in getAlbumInfo2.
	m.title       = clean_prose(tv ? jstr(j, "name") : jstr(j, "title"),
	                            MAX_NAME_BYTES);
	m.year        = year_of(tv ? jstr(j, "first_air_date")
	                           : jstr(j, "release_date"));
	m.overview    = clean_prose(jstr(j, "overview"), MAX_PROSE_BYTES);
	m.poster_path = jstr(j, "poster_path");

	// Two shapes, because the two endpoints answer differently: a search
	// result carries `genre_ids` (bare integers) and the detail endpoint
	// carries `genres` ([{id,name}]). Read whichever is there. Only the
	// detail form yields names here; a search result's ids are resolved by
	// Tmdb::search() through the id->name map, which this function cannot
	// reach because it is deliberately free of the object.
	//
	// The is_array() guards are not belt-and-braces: iterating a nlohmann
	// scalar yields that one scalar, so a malformed `genre_ids` arriving as a
	// bare number would be read as a genre id and resolve to somebody else's
	// genre. A null is an empty range and would be harmless, which is exactly
	// why the dangerous case is easy to miss.
	if (const auto& gs = jsub(j, "genres"); gs.is_array())
		for (const auto& g : gs)
			if (std::string n = clean_genre(jstr(g, "name")); !n.empty())
				m.genres.push_back(n);
	if (const auto& ids = jsub(j, "genre_ids"); ids.is_array())
		for (const auto& id : ids)
			if (id.is_number_integer()) m.genre_ids.push_back(id.get<int>());
	return m;
	}

Tmdb::Tmdb(std::string api_key)
	: api_key_(std::move(api_key))
	{
	}

// Out of line so httplib::SSLClient is complete where the unique_ptrs are
// destroyed; there is nothing else for it to do.
Tmdb::~Tmdb() = default;

void Tmdb::set_api_key(std::string api_key)
	{
	std::lock_guard<std::mutex> lock(mu_);
	api_key_ = std::move(api_key);
	// A different key is a different account, and the map may never have been
	// fetched successfully under the old one — an empty map that believes it
	// is loaded would silently drop every genre for the rest of the run.
	genre_names_.clear();
	genre_names_loaded_ = false;
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

	std::string         key;
	httplib::SSLClient* cli = nullptr;
	{
	std::lock_guard<std::mutex> lock(mu_);
	key = api_key_;
	cli = &client_for(api_, TMDB_HOST);
	}
	std::string url = path + "?api_key=" + key
	                + (query.empty() ? "" : "&" + query);
	auto r = cli->Get(url.c_str());
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

	std::string want = fold(title);

	if (year > 0) {
		// A year within one is a match. The gap is normal — TMDB records the
		// release date, and a filename usually carries the production year.
		//
		// The year alone is not enough: a parse that mangled the title still
		// carries a plausible year, and the first popular film from around it
		// used to win on that alone (title "A", year 2024, took the poster of
		// "A Desert"). The title must agree too. Exact folded equality goes
		// first, so the right film beats one that merely contains it.
		for (int i = 0; i < n; ++i) {
			TmdbMatch m = match_from(results[i], tv);
			if (m.year > 0 && std::abs(m.year - year) <= 1
			    && fold(m.title) == want) return m;
			}
		// Then containment, which is what accepts a dropped article or a
		// dropped subtitle ("Intouchables" against "The Intouchables"). Only
		// when the shorter side is substantial: the year check alone is no
		// defence against "a" being inside almost everything.
		for (int i = 0; i < n; ++i) {
			TmdbMatch m = match_from(results[i], tv);
			if (m.year <= 0 || std::abs(m.year - year) > 1) continue;
			std::string have = fold(m.title);
			const std::string& small = want.size() < have.size() ? want : have;
			const std::string& large = want.size() < have.size() ? have : want;
			if (small.size() >= MIN_CONTAIN_FOLD
			    && large.find(small) != std::string::npos) return m;
			}
		std::cout << stamp() << "tmdb: no title and year match for \"" << title
		          << "\" (" << year << ") in " << n << " result(s)"
		          << std::endl;
		return std::nullopt;
		}

	// With no year there is nothing to verify against but the title itself, so
	// the bar is an exact one. Anything looser confidently attaches the wrong
	// plot and poster to a vaguely named file, and nothing in the UI would
	// signal that it is wrong — which is worse than leaving it bare.
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

// The id->name lists, fetched once. Both types go into one map: they share an
// id space where they overlap (28 is Action in either) and are distinct where
// they do not, so a per-type map would only make the lookup need a media type
// that resolve_genres() does not have to hand.
//
// Not called with mu_ held — get() takes it itself, and pace() sleeps while
// holding it. The flag is set only after both requests have been attempted,
// and only when at least one produced names: an empty map marked loaded would
// silently drop every genre for the life of the object, which is the failure
// this is most likely to have.
void Tmdb::load_genre_names() const
	{
		{
		std::lock_guard<std::mutex> lock(mu_);
		if (genre_names_loaded_) return;
		}

	std::map<int, std::string> found;
	for (const char* kind : { "movie", "tv" }) {
		auto body = get(std::string("/3/genre/") + kind + "/list", "");
		if (!body) continue;
		try {
			auto j = nlohmann::json::parse(*body, nullptr, false);
			if (const auto& gs = jsub(j, "genres"); gs.is_array())
				for (const auto& g : gs) {
					int id = jint(g, "id");
					// The other door a genre name comes in by: search results
					// carry ids and are resolved through this map, so cleaning
					// only match_from() would leave every searched film's
					// genres unchecked.
					std::string name = clean_genre(jstr(g, "name"));
					if (id > 0 && !name.empty()) found.emplace(id, name);
					}
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "tmdb: unreadable " << kind
			          << " genre list: " << e.what() << std::endl;
			}
		}

	if (found.empty()) {
		std::cout << stamp() << "tmdb: no genre names available; "
		             "films will be filed without a genre this scan"
		          << std::endl;
		return;
		}

	std::lock_guard<std::mutex> lock(mu_);
	genre_names_        = std::move(found);
	genre_names_loaded_ = true;
	std::cout << stamp() << "tmdb: " << genre_names_.size()
	          << " genre names loaded" << std::endl;
	}

// Search results name no genres, only ids. Resolving them here rather than in
// match_from() is what keeps tmdb_pick() free of the network and of this
// object, which is the property that makes the matching rule testable.
//
// An id with no name is dropped rather than rendered as a number: a genre list
// is a browse vocabulary, and "878" in it is worse than one film missing from
// Science Fiction.
void Tmdb::resolve_genres(TmdbMatch& m) const
	{
	if (!m.genres.empty() || m.genre_ids.empty()) return;
	load_genre_names();

	std::lock_guard<std::mutex> lock(mu_);
	for (int id : m.genre_ids) {
		auto it = genre_names_.find(id);
		if (it != genre_names_.end()) m.genres.push_back(it->second);
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
	// encode_query_component, not encode_uri: the latter leaves '&' and '='
	// alone, which a film title can contain. space_as_plus=false keeps the
	// %20 spelling the pinned 0.18 encoder produced.
	std::string q = "query=" + httplib::encode_query_component(title, false)
	              + "&include_adult=false";
	auto body = get(std::string("/3/search/") + (tv ? "tv" : "movie"), q);
	if (!body) return std::nullopt;

	auto m = tmdb_pick(*body, title, year, tv);
	if (m) resolve_genres(*m);
	return m;
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

	httplib::SSLClient* cli = nullptr;
	{
	std::lock_guard<std::mutex> lock(mu_);
	cli = &client_for(img_, IMAGE_HOST);
	}

	std::string url = std::string("/t/p/") + POSTER_SIZE + poster_path;
	auto r = cli->Get(url.c_str());
	if (!r || r->status != 200 || r->body.empty()) {
		std::cout << stamp() << "tmdb: poster fetch failed for " << poster_path
		          << (r ? " (HTTP " + std::to_string(r->status) + ")" : "")
		          << std::endl;
		return std::nullopt;
		}
	return r->body;
	}

#pragma once

#include <chrono>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace httplib { class SSLClient; }

// A film or series identified on The Movie Database.
//
// Video containers carry no tag anything writes, so everything here has to be
// earned from the filename — which is what src/videoname.hh produces, and why
// that had to exist first.  TMDB is then asked one question: is there a film
// called this, released that year?
//
// This class knows the API and nothing else: no database, no scanner, no
// MediaStore.  Same shape as VideoArt and the filename parser, and for the
// same reason — the matching rule is the part that will need tuning against a
// real collection, and the whole class is what a different provider would
// replace.
struct TmdbMatch
	{
	int         id      = 0;
	bool        is_tv   = false;
	std::string title;
	int         year    = 0;
	std::string overview;
	std::string poster_path;   // TMDB-relative, e.g. "/abc123.jpg"

	// A film is normally two or three genres -- Alien is Horror *and* Science
	// Fiction -- so this is a list and not a string. Order is TMDB's own,
	// which puts the primary one first; that is the one that reaches the
	// single-valued Subsonic `genre` field.
	//
	// The two forms exist because the two endpoints answer differently. A
	// *search* result carries `genre_ids` and no names; the *detail* endpoint
	// by_id() uses carries `genres` with names. Anything leaving this class
	// has `genres` filled -- Tmdb::search() resolves the ids through the
	// id->name map before returning -- so no caller ever sees a bare id.
	// `genre_ids` is public only because tmdb_pick() is, and that is where a
	// search result is parsed.
	std::vector<int>         genre_ids;
	std::vector<std::string> genres;
	};

// The rule that decides which search result, if any, is the answer, split out
// from the request that fetched them so it can be exercised without a network
// or an API key — this is where a wrong poster would come from, so it is the
// part worth being able to test directly. `results_json` is a TMDB search
// response body.
std::optional<TmdbMatch> tmdb_pick(const std::string& results_json,
                                   const std::string& title, int year,
                                   bool tv);

class Tmdb
	{
	public:
		explicit Tmdb(std::string api_key);
		// Out of line, because the members below are unique_ptrs to a
		// forward-declared type and the destructor has to see it complete.
		~Tmdb();

		// The key is a stored setting rather than a start-up argument, so it
		// can be entered in the client and take effect on the next scan. This
		// exists because the object holds a mutex and so cannot simply be
		// reassigned.
		void set_api_key(std::string api_key);

		// False when no key is configured, in which case nothing here does
		// anything at all. Callers check this rather than discovering it as a
		// failure on every file.
		bool configured() const;

		// Empty when nothing matched *confidently* — which is a different
		// thing from nothing being found, and the distinction is the point.
		// See the rule in tmdb.cc: a wrong poster is worse than none, because
		// nothing in the UI signals that it is wrong.
		std::optional<TmdbMatch> search(const std::string& title, int year,
		                                 bool tv) const;

		// An explicit [tmdbid=550] from the filename. No verification, because
		// the user said so — this is the override for whatever the rule above
		// gets wrong.
		std::optional<TmdbMatch> by_id(int id, bool tv) const;

		// Poster bytes (JPEG) for TmdbMatch::poster_path, or empty.
		std::optional<std::string> poster(const std::string& poster_path) const;

	private:
		// One GET against api.themoviedb.org, paced. Returns the parsed body,
		// or nothing on any failure — a network error and a 404 are the same
		// thing to every caller here.
		std::optional<std::string> get(const std::string& path,
		                                const std::string& query) const;
		// Blocks until the next request is allowed. A scan of a large library
		// is exactly the traffic shape worth being polite about, and the scan
		// runs in the background so the wait costs nothing.
		void pace() const;

		// Fills genre_names_ from /3/genre/movie/list and /3/genre/tv/list if
		// it has not been done yet. Two requests per object, not per film --
		// which is the whole reason the map exists rather than fetching the
		// detail endpoint for every match just to read names off it.
		//
		// A failure is not cached as an answer: the map stays unloaded and the
		// next lookup tries again, so a moment of network trouble costs this
		// scan its genres rather than recording wrong ones for ever.
		void load_genre_names() const;

		// Turns a search result's genre_ids into names, through the map above.
		// Kept off tmdb_pick() so that function needs neither the network nor
		// this object -- which is the property that makes the matching rule
		// testable against canned JSON. A no-op when names are already there
		// (the detail endpoint supplies them) or when there is nothing to
		// resolve.
		void resolve_genres(TmdbMatch& m) const;

		// Guards the key (settable while a scan may be reading it), the pacing
		// clock, the genre map and the two clients below.
		mutable std::mutex                            mu_;
		std::string                                   api_key_;
		mutable std::chrono::steady_clock::time_point last_request_{};

		// TMDB genre id -> name, for both media types in one map. The two
		// lists share an id space (28 is Action in both), and where they do
		// not overlap the ids are simply distinct, so one map is enough and a
		// per-type one would only make the lookup need a type it does not
		// have. Cleared by set_api_key(), since a different key is a
		// different account and the fetch may not have succeeded under the
		// old one.
		mutable std::map<int, std::string>            genre_names_;
		mutable bool                                  genre_names_loaded_ = false;

		// One client per host, held for the object's lifetime so the socket
		// survives between requests.
		//
		// An album costs two requests, one to each host, and building a client
		// per request meant a DNS lookup, a TCP handshake and a TLS handshake
		// for each of them — comparable to REQUEST_GAP itself, and paid a few
		// thousand times over a first scan. httplib keeps the connection alive
		// across Get() calls on one client, so holding them is the whole fix.
		//
		// unique_ptr to a forward-declared type so httplib.h stays out of this
		// header: it is a large header, and tmdb.hh is included by mediastore.
		// Created on first use, because a client is only worth a socket once
		// there is a key configured.
		mutable std::unique_ptr<httplib::SSLClient>   api_;
		mutable std::unique_ptr<httplib::SSLClient>   img_;
	};

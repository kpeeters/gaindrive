// The online info resolver: MusicBrainz, Wikidata, Wikipedia, TheAudioDB and
// Discogs for an artist, MusicBrainz plus Wikidata and Wikipedia for an album,
// and the single background queue that runs both.
//
// Split out of gaindrive.cc as one unit rather than two because the two
// resolvers share the MusicBrainz client below, and mb_pace() is one gate for
// the whole process: a second consumer would add no throughput while halving
// the courtesy headroom LOOKUP_GAP exists to leave.
//
// Everything here is either static to this file or a member of GainDrive, so
// there is no infolookup.hh: nothing outside calls in except through the
// members gaindrive.hh already declares.

#include "gaindrive.hh"
#include "subsonic.hh"
#include "textutil.hh"
#include "netaddr.hh"
#include "authz.hh"
#include "stamp.hh"
#include "imagescale.hh"
#include "jsonread.hh"
#include "untrusted.hh"
#include "artistmatch.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// An artist portrait is stored at this bound on its long edge — comfortably
// above the largest any client asks for (iOS wants 800 for a hero), so the
// stored image is never the limiting factor, and small enough that a database
// row stays a sensible place to keep it.
static constexpr int PORTRAIT_PX = 800;

// A portrait URL points at a third party, and nothing about what comes back is
// bounded except by us. Wikimedia renders are tens of kilobytes; anything at
// this scale is a mistake somewhere.
static constexpr size_t MAX_PORTRAIT_BYTES = 8u * 1024 * 1024;

// Wait between jobs in the resolver, artists and albums alike. The MusicBrainz
// gate below is what actually enforces the rate limit; this is a courtesy gap
// on top, so a background pass over the whole library leaves headroom for a
// client's own getArtistInfo2 rather than keeping the gate permanently
// saturated.
//
// It is one gap for both kinds because there is one queue and one thread --
// see the LookupJob note in gaindrive.hh. Giving albums a thread of their own
// would buy no throughput, mb_pace() being process-global, and would halve the
// headroom this constant exists to leave.
static constexpr auto LOOKUP_GAP = std::chrono::milliseconds(2000);

// ---- MusicBrainz rate limiting ----------------------------------------
//
// **One gate for the whole process, and every request to musicbrainz.org goes
// through it.** The limit is per IP address, not per caller, so pacing each
// caller separately does not add up to anything: the artist chain paced itself
// through the resolver thread, while getAlbumInfo — which makes two more
// MusicBrainz requests per album — ran straight off the HTTP thread pool with
// no pacing at all, up to 32 at a time (both are on the one queue now, but the
// gate is what made this survivable before that). A client browsing would then
// spend the whole budget, and the next artist lookup got a 503 on its *first*
// request, which reads exactly like "MusicBrainz is broken" rather than "we
// asked too fast".
//
// A 503 is not free to earn, either: it is indistinguishable from "this artist
// has nothing" unless the caller is careful, which is why artist_art records
// 'error' separately.
//
// MusicBrainz documents one request per second averaged over time. 1100 ms
// leaves a margin for clock jitter and for the round trip itself counting
// against the window at the far end.
static constexpr auto MB_REQUEST_GAP = std::chrono::milliseconds(1100);

// ---- Metadata provider timeouts ---------------------------------------
//
// **httplib's client defaults are 300 s to connect and 300 s to read**, and
// the provider clients in resolve_artist_info() set neither — so a host that
// black-holed packets rather than refusing them parked the caller for five
// minutes per request. That was a latent hang on an HTTP worker until the
// lookup moved to the resolver thread, and it would still be one there: the
// resolver queue is a single thread, so one unreachable provider stalls every
// job behind it.
//
// The numbers are the ones portrait_fetch() and setCoverArt's url= already use
// for the same kind of request, and nothing here is worth waiting longer for —
// a provider that has not answered in fifteen seconds is not going to.
//
// Note this reads correctly through mb_get(): a timeout makes Get() return a
// falsy Result, and its retry loop only *returns* on `r && r->status != 503`,
// so a timed-out MusicBrainz request costs a retry rather than an immediate
// give-up. Four attempts at fifteen seconds plus backoff is the bound.
//
// resolve_album_info()'s three clients set them too, since it moved onto the
// same thread and inherited the same argument. Nothing in this file talks to a
// provider without them now.
static constexpr int PROVIDER_CONNECT_TIMEOUT_S = 5;
static constexpr int PROVIDER_READ_TIMEOUT_S    = 15;

// One spelling of it rather than one per client.
static void provider_timeouts(httplib::SSLClient& cli)
	{
	cli.set_connection_timeout(PROVIDER_CONNECT_TIMEOUT_S);
	cli.set_read_timeout(PROVIDER_READ_TIMEOUT_S);
	}

static std::mutex                            mb_gate_mu_;
static std::chrono::steady_clock::time_point mb_last_request_;

// Blocks until the next MusicBrainz request is allowed. Modelled on
// Tmdb::pace(), and deliberately a plain sleep under a mutex: the callers are
// either background threads, where waiting costs nothing, or a request handler
// that was going to spend far longer on the network anyway.
static void mb_pace()
	{
	std::lock_guard<std::mutex> lock(mb_gate_mu_);
	auto now = std::chrono::steady_clock::now();
	if (mb_last_request_.time_since_epoch().count() != 0) {
		auto since = now - mb_last_request_;
		if (since < MB_REQUEST_GAP)
			std::this_thread::sleep_for(MB_REQUEST_GAP - since);
		}
	mb_last_request_ = std::chrono::steady_clock::now();
	}

// **Most MusicBrainz 503s are not about our rate at all**, and assuming they
// were is what made artist lookups fail wholesale.
//
// Measured against the live service: a successful response carries
// `X-RateLimit-Limit: 1200` with `remaining` in the hundreds — our per-address
// budget is barely touched — while a 503 carries a *different* header,
// `X-RateLimit-Limit: 15` with `remaining: 11`, `Retry-After: 0`, and a body
// reading "The MusicBrainz web server is currently busy. Please try again
// later." That is their global load shedding, not our quota, and at the time of
// writing roughly one request in three hits it on both the search and the
// lookup endpoint.
//
// So the right answer to a 503 is to **ask again for the same thing**, not to
// give up on this artist and move to the next one — which merely spends
// another attempt on the same busy server and makes the whole pass look like a
// permanent failure.
static constexpr int  MB_MAX_ATTEMPTS   = 4;
static constexpr auto MB_RETRY_BACKOFF  = std::chrono::milliseconds(700);
static constexpr auto MB_RETRY_CAP      = std::chrono::milliseconds(8000);

// True when a 503 is MusicBrainz saying it is busy rather than that we asked
// too fast. The two are worth telling apart in the log: one is theirs and one
// would be ours.
static bool mb_busy(const httplib::Result& r)
	{
	return r && r->status == 503
	    && r->body.find("currently busy") != std::string::npos;
	}

static bool mb_rate_limited(const httplib::Result& r)
	{
	return r && r->status == 503 && !mb_busy(r);
	}

// One paced MusicBrainz GET, retried while the service says it is busy.
// `what` labels the log lines; every caller already has a name to hand.
static httplib::Result mb_get(httplib::SSLClient& cli, const std::string& path,
                               const httplib::Params& params,
                               const std::string& what)
	{
	httplib::Result r;
	for (int attempt = 1; attempt <= MB_MAX_ATTEMPTS; ++attempt) {
		mb_pace();
		r = cli.Get(path, params, httplib::Headers{});

		// Anything that is not a 503 is an answer, including a 404.
		if (r && r->status != 503) return r;
		if (attempt == MB_MAX_ATTEMPTS) break;

		// Exponential, but Retry-After wins when they send a usable one. They
		// send 0 with a busy 503, which means "immediately" and would spin, so
		// the backoff is a floor rather than a default.
		auto wait = MB_RETRY_BACKOFF * (1 << (attempt - 1));
		if (r) {
			auto ra = r->get_header_value("Retry-After");
			if (!ra.empty()) {
				try {
					auto secs = std::stoi(ra);
					if (secs > 0)
						wait = std::max(wait, std::chrono::milliseconds(secs * 1000));
					}
				catch (const std::exception&) { /* not a number; keep ours */ }
				}
			}
		if (wait > MB_RETRY_CAP) wait = MB_RETRY_CAP;

		std::cout << stamp() << what << ": MusicBrainz "
		          << (!r              ? "did not answer"
		              : mb_busy(r)    ? "is busy"
		                              : "refused (503)")
		          << ", retrying in " << wait.count() << " ms (attempt "
		          << attempt << " of " << MB_MAX_ATTEMPTS << ")" << std::endl;
		std::this_thread::sleep_for(wait);
		}
	return r;
	}

// ---- Artist info helper -----------------------------------------------

// Performs the MusicBrainz -> Wikidata -> Wikipedia -> TheAudioDB -> Discogs
// lookup for one artist and caches the result. Runs on the info resolver's
// thread and nowhere else; it is paced, it sleeps, and it must never be reached
// from a request handler again.
//
// `provider_error`, when given, is set true if any provider failed to answer —
// no response, or a status that is neither 200 nor 404. That distinction is the
// whole point of the parameter: **a provider that says "no" is a fact, and a
// provider that says nothing is not.** Without it a caller cannot tell "nobody
// has a picture of this artist" from "MusicBrainz returned 503 because we asked
// too fast", and recording the second as though it were the first makes a
// transient rate-limit permanent.
//
// **It never reads artist_info_cache**, and the missing `force` parameter is
// that rule rather than an omission. Its one caller is lookup_worker(), which
// always wanted the providers asked: resolve_artist_info() caches whenever the
// MusicBrainz *search* succeeded, so an artist whose image providers were the
// ones that fell over — then or in any earlier version of gaindrive — has a
// cached row with an empty image_url, and reading that back would find no image
// and conclude there is none. That is the very confusion artist_art exists to
// record correctly. It costs one lookup per artist, once, and the answer is
// then kept for good. handle_artist_info() is what serves the cache.
static MediaStore::CachedArtistInfo resolve_artist_info(int id, const std::string& name,
                                                         MediaStore& store,
                                                         bool* provider_error = nullptr)
	{
	// A level-1 folder of a categories root is a section — Film, Series,
	// Documentary — not a performer.  Looking it up would query MusicBrainz
	// for "Film" and cache whatever came back as that section's biography.
	// Same failure shape as the CD1 lookup recorded in ISSUES.md.
	if (store.is_category_folder(id)) {
		std::cout << stamp() << "getArtistInfo [" << name
		          << "] is a category, not an artist; skipping lookup"
		          << std::endl;
		return {};
		}

	std::cout << stamp() << "getArtistInfo [" << name << "] querying MusicBrainz"
	          << std::endl;
	MediaStore::CachedArtistInfo info;
	httplib::SSLClient mb("musicbrainz.org");
	provider_timeouts(mb);
	mb.set_default_headers({
		{"User-Agent", USER_AGENT}
		});
	// Both the name field and the alias field, and more than one candidate:
	// see mb_artist_query() and mb_pick_artist() in artistmatch.hh. The limit
	// is a page size on a request that is made either way, so asking for
	// several costs nothing and is what lets the exact-match rule work at all.
	httplib::Params params{
		{"query", mb_artist_query(name)},
		{"limit", "8"},
		{"fmt",   "json"}
		};
	// 404 is an answer: this artist is not there. Anything else — no response
	// at all, 503, 429, 500 — means we did not get to ask, and the caller must
	// not record the silence as a result.
	auto note = [&](const httplib::Result& res) {
		if (provider_error && (!res || (res->status != 200 && res->status != 404)))
			*provider_error = true;
		};

	bool mb_ok = false;

	// Step 1 is a *search by name*, and it is the half of this that can be
	// wrong without anything downstream being able to tell: two artists share
	// a name, one of them wins, and the biography and portrait that follow
	// belong to the other one. mb_pick_artist() narrows that -- an exact match
	// on the name or on one of its aliases outranks MusicBrainz's own score,
	// and nothing at all is preferred to a poor guess -- but it cannot close
	// it, since two artists really do share a name. When the files themselves
	// carry the id there is nothing to search for and none of this applies.
	//
	// mb_ok has to be set here as well, or the cache write at the end never
	// runs and every request re-resolves the whole chain for ever. The Last.fm
	// URL is composed from the name rather than fetched, so it is copied out of
	// the search branch rather than skipped with it.
	std::string tag_mbid = store.get_artist_tag_mbid(id);
	if (!tag_mbid.empty()) {
		std::cout << stamp() << "getArtistInfo [" << name
		          << "] MusicBrainz id from the files' tags, skipping search"
		          << std::endl;
		info.mbid        = tag_mbid;
		info.last_fm_url = "https://www.last.fm/music/" + url_encode(name);
		mb_ok            = true;
		}

	if (info.mbid.empty()) {
		auto r = mb_get(mb, "/ws/2/artist", params,
		                "getArtistInfo [" + name + "]");
		note(r);
		if (!r) {
			std::cout << stamp() << "getArtistInfo [" << name
			          << "] MusicBrainz request failed (no response)" << std::endl;
			}
		else if (r->status != 200) {
			std::cout << stamp() << "getArtistInfo [" << name
			          << "] MusicBrainz HTTP " << r->status
			          << (mb_rate_limited(r)
			                  ? " - rate limited; something in this server is asking"
			                    " MusicBrainz faster than once a second" : "")
			          << std::endl;
			}
		else {
			mb_ok = true;
			// The chosen artist's own name is logged because it is routinely
			// *not* the folder's -- 上原ひろみ for a folder called "Hiromi
			// Uehara" is the case this rule exists for -- so without it a log
			// cannot show whether a lookup found the right person. Same for
			// the score and the exactness: they are the whole of why this
			// candidate won, and a wrong match is otherwise indistinguishable
			// from a right one until somebody reads the biography.
			auto pick = mb_pick_artist(r->body, name);
			// is_uuid because this is about to be concatenated into a
			// MusicBrainz URL *path* below.  mb_uuid() has applied the same
			// check to a file's tag since that path existed, on the stated
			// grounds that a tag is arbitrary bytes somebody else wrote — a
			// search result is that too, and was the half being trusted.
			if (pick && !is_uuid(pick->mbid)) {
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] ignoring a match whose id is not a UUID"
				          << std::endl;
				pick.reset();
				}
			if (pick) {
				info.mbid        = pick->mbid;
				info.last_fm_url = "https://www.last.fm/music/" + url_encode(name);
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] matched [" << pick->name << "] "
				          << pick->mbid << " score=" << pick->score
				          << (pick->exact ? " (exact)" : " (best guess)")
				          << std::endl;
				}
			else {
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] no MusicBrainz artist matched this name"
				          << std::endl;
				}
			}
		}

	// Step 2 — MusicBrainz URL relations → Wikipedia article URL.
	if (!info.mbid.empty()) {
		// The one-second wait that used to sit here is mb_get()'s job now;
		// doing it in both places only made every lookup a second slower.
		httplib::Params p2{{"inc","url-rels"},{"fmt","json"}};
		auto r2 = mb_get(mb, "/ws/2/artist/" + info.mbid, p2,
		                 "getArtistInfo [" + name + "] url-rels");
		note(r2);
		if (!r2) {
			std::cout << stamp() << "getArtistInfo [" << name
			          << "] MusicBrainz url-rels request failed" << std::endl;
			}
		else if (r2->status != 200) {
			std::cout << stamp() << "getArtistInfo [" << name
			          << "] MusicBrainz url-rels HTTP " << r2->status
			          << (mb_rate_limited(r2) ? " - rate limited" : "")
			          << std::endl;
			}
		else {
			auto j2 = nlohmann::json::parse(r2->body, nullptr, false);
			const auto& rels = jsub(j2, "relations");
			std::cout << stamp() << "getArtistInfo [" << name
			          << "] MusicBrainz url-rels: " << rels.size() << " relation(s)";
			for (auto& rel : rels) {
				std::string t = jstr(rel, "type");
				std::cout << " [" << (t.empty() ? "?" : t) << "]";
				}
			std::cout << std::endl;

			// Prefer direct wikipedia relation; fall back to wikidata.
			std::string wiki_title;
			std::string wd_image_url;
			for (auto& rel : rels) {
				std::string type     = jstr(rel, "type");
				std::string resource = jstr(jsub(rel, "url"), "resource");
				if (type == "allmusic" && info.allmusic_url.empty()) {
					info.allmusic_url = clean_url(resource);
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] AllMusic: " << resource << std::endl;
					}
				else if (type == "discogs" && info.discogs_url.empty()) {
					info.discogs_url = clean_url(resource);
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Discogs: " << resource << std::endl;
					}
				else if (type == "wikipedia") {
					auto pos = resource.find("/wiki/");
					if (pos != std::string::npos) {
						wiki_title = resource.substr(pos + 6);
						std::cout << stamp() << "getArtistInfo [" << name
						          << "] Wikipedia (direct): " << wiki_title << std::endl;
						break;
						}
					}
				else if (type == "wikidata" && wiki_title.empty()) {
					auto pos = resource.rfind('/');
					if (pos == std::string::npos) continue;
					std::string entity = resource.substr(pos + 1);
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Wikidata entity: " << entity << std::endl;
					httplib::SSLClient wd("www.wikidata.org");
					provider_timeouts(wd);
					wd.set_default_headers({
						{"User-Agent",USER_AGENT}
						});
					auto rwd = wd.Get("/w/api.php",
						httplib::Params{
							{"action","wbgetentities"},{"ids",entity},
							{"props","sitelinks|claims"},{"sitefilter","enwiki"},
							{"format","json"}
							},
						httplib::Headers{});
					note(rwd);
					if (rwd && rwd->status == 200) {
						auto jwd = nlohmann::json::parse(rwd->body, nullptr, false);
						const auto& ent = jsub(jsub(jwd, "entities"), entity);
						wiki_title = jstr(jsub(jsub(ent, "sitelinks"), "enwiki"),
						                  "title");
						if (!wiki_title.empty())
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] Wikipedia (via Wikidata): "
							          << wiki_title << std::endl;
						else
							// The step that most often ends the bio chain, and it
							// used to end it in silence: a log line only for the
							// sitelink that was found says nothing about the one
							// that was not. A MusicBrainz *group* commonly has no
							// article of its own while the person does.
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] no English Wikipedia article for "
							          << entity << std::endl;

						// Wikidata P18 (image) as fallback when no Wikipedia article.
						const auto& p18 =
							jidx(jsub(jsub(ent, "claims"), "P18"), 0);
						std::string fn =
							jstr(jsub(jsub(p18, "mainsnak"), "datavalue"), "value");
						if (!fn.empty()) {
							for (char& c : fn) if (c == ' ') c = '_';
							wd_image_url =
								"https://commons.wikimedia.org/wiki/Special:FilePath/"
								+ url_encode(fn);
							}
						}
					}
				}

			// Step 3 — Wikipedia REST summary → bio + thumbnail.
			if (!wiki_title.empty()) {
				httplib::SSLClient wp("en.wikipedia.org");
				provider_timeouts(wp);
				wp.set_default_headers({
					{"User-Agent",USER_AGENT}
					});
				// Percent-encoded, because this is spliced into an HTTP
				// request line: a CR/LF in a provider-supplied title is
				// request splitting against Wikipedia.  The underscore
				// substitution stays and happens first, or url_encode would
				// turn the spaces into %20 and the article would not be found.
				std::string path_title = wiki_title;
				for (char& c : path_title) if (c == ' ') c = '_';
				path_title = url_encode(path_title);
				auto r3 = wp.Get("/api/rest_v1/page/summary/" + path_title,
				                 httplib::Params{}, httplib::Headers{});
				note(r3);
				if (!r3) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Wikipedia request failed" << std::endl;
					}
				else if (r3->status != 200) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Wikipedia HTTP " << r3->status << std::endl;
					}
				else {
					auto j3 = nlohmann::json::parse(r3->body, nullptr, false);
					if (!j3.is_discarded()) {
						// Cleaned here rather than on the way to the cache:
						// the log lines below print these, and untrusted.hh
						// explains what a control character costs in each of
						// the two response formats.  path_title is reused for
						// the stored URL so the encoding is done once.
						info.biography = clean_prose(jstr(j3, "extract"),
						                             MAX_PROSE_BYTES);
						info.wiki_url  = "https://en.wikipedia.org/wiki/" + path_title;
						info.image_url = clean_url(jstr(jsub(j3, "thumbnail"),
						                                "source"));
						std::cout << stamp() << "getArtistInfo [" << name
						          << "] bio=" << info.biography.size()
						          << " chars, image="
						          << (info.image_url.empty() ? "(none)" : info.image_url)
						          << std::endl;
						}
					}
				}
			if (info.image_url.empty() && !wd_image_url.empty()) {
				info.image_url = wd_image_url;
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] image from Wikidata P18: " << wd_image_url << std::endl;
				}

			// TheAudioDB is the fallback for the *biography* as well as for
			// the portrait, and is therefore asked whenever either is still
			// missing rather than only when the image is. The two do not
			// fail together: Wikipedia supplies both or neither, so an
			// artist with an article but no thumbnail reaches here for a
			// picture, and one with no article at all reaches here for both.
			//
			// That second case is not exotic. A folder tagged with a
			// MusicBrainz *group* id — "Stevie Ray Vaughan and Double
			// Trouble" rather than the person — commonly reaches a Wikidata
			// item with no enwiki sitelink, and Wikipedia being the only bio
			// tier meant such an artist could never have one. The tag id is
			// still the authority and the name search is still not retried;
			// it is the provider chain that was one tier short.
			//
			// Each field is taken independently and only when empty, so
			// Wikipedia goes on winning wherever it answered.
			if (info.image_url.empty() || info.biography.empty()) {
				std::this_thread::sleep_for(std::chrono::seconds(1));
				httplib::SSLClient tadb("www.theaudiodb.com");
				provider_timeouts(tadb);
				tadb.set_default_headers({
					{"User-Agent",USER_AGENT}
					});
				auto rt = tadb.Get("/api/v1/json/2/artist-mb.php",
				                   httplib::Params{{"i", info.mbid}},
				                   httplib::Headers{});
				note(rt);
				if (rt && rt->status == 200) {
					// A miss here is "artists": null, not an empty array.
					auto jt = nlohmann::json::parse(rt->body, nullptr, false);
					const auto& ta = jidx(jsub(jt, "artists"), 0);
					if (info.image_url.empty()) {
						info.image_url = clean_url(jstr(ta, "strArtistThumb"));
						if (!info.image_url.empty())
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] image from TheAudioDB: "
							          << info.image_url << std::endl;
						}
					if (info.biography.empty()) {
						// Both spellings, in this order. strBiographyEN is the
						// language-tagged field and the one to prefer, but
						// records exist where it is null while strBiography
						// holds the English prose — reading only the tagged
						// name yields nothing for those.
						info.biography = clean_prose(jstr(ta, "strBiographyEN"),
						                             MAX_PROSE_BYTES);
						if (info.biography.empty())
							info.biography = clean_prose(jstr(ta, "strBiography"),
							                             MAX_PROSE_BYTES);
						if (!info.biography.empty())
							std::cout << stamp() << "getArtistInfo [" << name
							          << "] bio from TheAudioDB: "
							          << info.biography.size() << " chars"
							          << std::endl;
						}
					}
				}

			if (info.image_url.empty() && !info.discogs_url.empty()) {
				std::string token = store.get_setting("discogs_token");
				if (token.empty()) {
					std::cout << stamp() << "getArtistInfo [" << name
					          << "] Discogs URL available but no token configured"
					          << std::endl;
					}
				else {
					auto apos = info.discogs_url.find("/artist/");
					if (apos != std::string::npos) {
						std::string id_str;
						for (char c : info.discogs_url.substr(apos + 8))
							{ if (!std::isdigit(c)) break; id_str += c; }
						if (!id_str.empty()) {
							std::this_thread::sleep_for(std::chrono::seconds(1));
							httplib::SSLClient disc("api.discogs.com");
							provider_timeouts(disc);
							disc.set_default_headers({
								{"User-Agent", USER_AGENT},
								{"Authorization", "Discogs token=" + token}
								});
							auto rd = disc.Get("/artists/" + id_str,
							                   httplib::Params{}, httplib::Headers{});
							note(rd);
							if (rd && rd->status == 200) {
								auto jd = nlohmann::json::parse(rd->body, nullptr, false);
								const auto& imgs = jsub(jd, "images");
								std::string uri;
								for (auto& img : imgs)
									if (jstr(img, "type") == "primary")
										{ uri = jstr(img, "uri"); break; }
								if (uri.empty()) uri = jstr(jidx(imgs, 0), "uri");
								uri = clean_url(uri);
								if (!uri.empty()) {
									info.image_url = uri;
									std::cout << stamp() << "getArtistInfo [" << name
									          << "] image from Discogs API: "
									          << uri << std::endl;
									}
								}
							else {
								std::cout << stamp() << "getArtistInfo [" << name
								          << "] Discogs API failed"
								          << (rd ? " HTTP " + std::to_string(rd->status)
								                : " (no response)")
								          << std::endl;
								}
							}
						}
					}
				}

			if (info.image_url.empty())
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] no image found" << std::endl;
			if (info.biography.empty())
				std::cout << stamp() << "getArtistInfo [" << name
				          << "] no biography found" << std::endl;
			}
		}

	if (mb_ok) {
		store.cache_artist_info(id, info);
		std::cout << stamp() << "getArtistInfo [" << name << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}
	else {
		std::cout << stamp() << "getArtistInfo [" << name
		          << "] not cached (MusicBrainz unavailable)" << std::endl;
		}
	return info;
	}

// Shared implementation for getArtistInfo and getArtistInfo2.
// key is "artistInfo" or "artistInfo2" — controls the XML element / JSON key.
//
// **This reads the database and nothing else**, which is the whole point of it
// and was not true until recently. It used to call resolve_artist_info() right
// here, on the httplib worker: MusicBrainz (paced against a process-global gate
// that sleeps under its own mutex, and retried four times when the service says
// it is busy — which is roughly one request in three), then Wikidata, Wikipedia,
// TheAudioDB and Discogs, two unconditional one-second courtesy sleeps included.
// One uncached artist held a worker for tens of seconds.
//
// That did not merely make *this* endpoint slow. httplib dispatches one task per
// *connection* rather than per request, and a browser gets about six connections
// to an origin, so clicking through three or four unresolved artists spent most
// of the budget and the cover-art requests queued up behind them — in the
// browser, where nothing on the server can see it. The symptom is an album grid
// crawling while serving images that are sitting in memory.
//
// It is exactly the fault the *portrait* was moved off the request thread to
// fix; see the comment in getCoverArt. The picture went to the background
// resolver and the words were left behind. They travel together now, which cost
// nothing to arrange: lookup_run_job() already calls resolve_artist_info() and
// that function already writes artist_info_cache, so the biography this handler
// wants is a side effect the resolver was producing all along.
//
// That side effect is also the whole reason startInfoLookup exists: what the
// resolver's own seed asks about is a missing *portrait*, so an artist who has
// a picture and an empty biography — the providers for the two are not the
// same, and they do not fail together — is never asked about again.
void GainDrive::handle_artist_info(const httplib::Request& req,
                                    httplib::Response& res, const char* key)
	{
	bool use_json = (fmt_of(req) == "json");
	auto err = [&](int code, const char* msg) {
		if (use_json)
			res.set_content(subsonic_error_json(code, msg), "application/json");
		else
			res.set_content(subsonic_error(code, msg),      "application/xml");
		};

	auto it = req.params.find("id");
	if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

	int id = to_int(it->second, -1);
	std::string name = store_.get_folder_name(id);
	if (name.empty()) { err(70, "Artist not found."); return; }
	// An id under another user's uploads must answer exactly like a missing
	// one — this endpoint both leaks the directory name and pushes it onto
	// the provider queue, so it is a read like any other.
	if (!item_read_allowed(req, store_, uploads_root_name_,
	                       store_.get_folder_path(id))) {
		err(70, "Artist not found."); return;
		}

	bool force = req.params.count("force") > 0
	          && req.params.find("force")->second != "0";
	// The read stays open to everyone; the re-ask does not. It spends the single
	// paced MusicBrainz gate every other pane is waiting on, and it overwrites a
	// cache the whole server shares — so one account could keep re-resolving an
	// artist nobody else wanted re-resolved. Refused rather than quietly ignored:
	// a client that asked for a fresh lookup should hear that it did not get one.
	if (force) {
		auto ui = store_.get_user(req.get_param_value("u"));
		if (!ui || !ui->is_admin) {
			err(50, "Forcing a provider lookup requires admin role."); return;
			}
		}

	MediaStore::CachedArtistInfo info;
	bool resolving = false;

	// A level-1 folder of a categories root is a section — Film, Series,
	// Documentary — not a performer. This test used to live inside
	// resolve_artist_info(); it has to be made here now, or a section would be
	// pushed onto the resolver queue to have MusicBrainz asked about "Film".
	// Nothing is resolving and nothing ever will be, so say so: a client that
	// polled on `resolving` would poll for ever.
	//
	// Nor is an album a performer, and is_category_folder() cannot say so: a
	// loose film is its own album at depth 2, where that test answers false,
	// and a folder-browsing client that hands this endpoint the file's own id
	// would have MusicBrainz asked about "Meeting Gorbatchev 2018". The same
	// guard getCoverArt's portrait branch carries.
	if (!store_.is_category_folder(id) && !store_.folder_is_album(id)) {
		if (auto cached = store_.get_cached_artist_info(id))
			info = *cached;

		// Whether the resolver has *finished* with this artist, which is not
		// the same question as whether it found anything. store_artist_art()
		// is called on every pass whatever the outcome, so a row saying "ok"
		// or "none" means the answer above — including an empty one — is
		// final, while a missing row or "error" means it has yet to run or
		// could not reach a provider. Conflating the two is the failure the
		// artist_art status column exists to prevent, one endpoint over: an
		// artist nobody has written about would otherwise be re-queued and
		// re-polled for ever.
		std::string path  = store_.get_folder_path(id);
		auto        state = store_.get_artist_art_state(path);
		resolving = force || !state || state->status == "error"
		         || lookup_pending(path);

		if (resolving) {
			// Front of the queue: what somebody is looking at beats the
			// alphabet. Already queued or in flight is a no-op, so a client
			// polling every few seconds costs nothing.
			lookup_request_front(LookupKind::Artist, id, path, name);
			std::cout << stamp() << "getArtistInfo [" << name << "] "
			          << (force ? "re-queued (force)" : "queued for the resolver")
			          << std::endl;
			}
		}

	// Build response. All fields are child elements per the Subsonic spec.
	auto add_text_el = [](XMLDocument& doc, XMLElement* parent,
	                       const char* tag, const std::string& val) {
		if (val.empty()) return;
		auto* el = doc.NewElement(tag);
		el->SetText(val.c_str());
		parent->InsertEndChild(el);
		};

	// `resolving` is emitted only when it is true, so its absence carries the
	// same meaning to a client that has never heard of it as to one that has:
	// this is everything there is. A standard Subsonic client ignores it and
	// sees an empty biography on the first view and a real one on the next,
	// which is the bargain the portrait already makes with it.
	std::string body;
	if (use_json)
		body = subsonic_ok_json([&info, resolving, key](nlohmann::json& r) {
			nlohmann::json ai = nlohmann::json::object();
			if (!info.biography.empty())      ai["biography"]     = info.biography;
			if (!info.mbid.empty())           ai["musicBrainzId"] = info.mbid;
			if (!info.last_fm_url.empty())    ai["lastFmUrl"]     = info.last_fm_url;
			if (!info.wiki_url.empty())       ai["wikiUrl"]       = info.wiki_url;
			if (!info.allmusic_url.empty())   ai["allMusicUrl"]   = info.allmusic_url;
			if (!info.discogs_url.empty())    ai["discogsUrl"]    = info.discogs_url;
			if (!info.image_url.empty()) {
				ai["smallImageUrl"]  = info.image_url;
				ai["mediumImageUrl"] = info.image_url;
				ai["largeImageUrl"]  = info.image_url;
				}
			if (resolving)                    ai["resolving"]     = true;
			r[key] = ai;
			});
	else
		body = subsonic_ok([&info, &add_text_el, resolving, key]
		                   (XMLDocument& doc, XMLElement* root) {
			auto* ai = doc.NewElement(key);
			add_text_el(doc, ai, "biography",     info.biography);
			add_text_el(doc, ai, "musicBrainzId", info.mbid);
			add_text_el(doc, ai, "lastFmUrl",     info.last_fm_url);
			add_text_el(doc, ai, "wikiUrl",       info.wiki_url);
			add_text_el(doc, ai, "allMusicUrl",   info.allmusic_url);
			add_text_el(doc, ai, "discogsUrl",    info.discogs_url);
			add_text_el(doc, ai, "smallImageUrl",  info.image_url);
			add_text_el(doc, ai, "mediumImageUrl", info.image_url);
			add_text_el(doc, ai, "largeImageUrl",  info.image_url);
			if (resolving) add_text_el(doc, ai, "resolving", "true");
			root->InsertEndChild(ai);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album info helper ------------------------------------------------

// What a folder name carries and a release-group title does not.
//
// **MusicBrainz's search is a phrase query, so a suffix does not merely lower
// the score -- it returns nothing at all.**  Measured against the live
// service: releasegroup:"Abbey Road (Remastered)" answers with zero results,
// releasegroup:"Abbey Road" answers with the album at score 100.  Same for
// "[2011 Remaster]", "(Disc 1)" and " - Legacy Edition".  That, and not any
// character needing escaping, is why a library resolves so few of its albums:
// escaping changes nothing here, because everything between the quotes is
// literal to Lucene already and the analyzer drops the punctuation anyway --
// a title holding a comma, a colon, a slash, an exclamation mark or even a
// double quote searches identically escaped or not.
//
// Suffixes only, and only ones that are visibly an aside.  A title whose
// brackets open at position zero is left alone, since the whole of it is the
// aside, and a bare " - " needs one of the edition words below before it
// counts -- plenty of real titles are written with a dash.  Not resolving an
// album costs a description; resolving the wrong one attributes somebody
// else's to it, with nothing downstream able to tell.
static std::string strip_album_decoration(const std::string& title)
	{
	auto rtrim = [](std::string& s) {
		while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.pop_back();
		};

	std::string t = title;
	rtrim(t);

	// As many trailing "(...)" or "[...]" groups as there are: a folder is
	// quite capable of saying "(Deluxe Edition) (Disc 2)".
	for (;;) {
		if (t.size() < 2) break;
		char open = t.back() == ')' ? '(' : t.back() == ']' ? '[' : '\0';
		if (open == '\0') break;
		auto pos = t.rfind(open);
		if (pos == std::string::npos || pos == 0) break;
		t.erase(pos);
		rtrim(t);
		}

	auto dash = t.rfind(" - ");
	if (dash != std::string::npos && dash > 0) {
		std::string tail = t.substr(dash + 3);
		for (char& c : tail)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		static const char* const MARKS[] = {
			"edition", "remaster", "version", "reissue", "deluxe", "expanded",
			"anniversary", "bonus", "disc", "cd"
			};
		for (const char* m : MARKS)
			if (tail.find(m) != std::string::npos) { t.erase(dash); break; }
		}

	rtrim(t);
	return t;
	}

// Which MusicBrainz search an album title is put to.
//
// **A release group is named after the *original* release, so a reissue keeps
// its own name only one level down.**  Measured: no release group anywhere
// carries the title "The Fillmore Concerts", not even with the artist clause
// removed, while a *release* search finds three at score 100 and every one of
// them sits in the release group "At Fillmore East".  A library filed under
// the name printed on the disc is invisible to a release-group search however
// exactly it is spelled, and no amount of loosening the query changes that --
// it is the wrong index.
//
// The release search answers with its release group inline, so this costs one
// request and yields the same kind of id.
enum class MbBy { ReleaseGroup, Release };

// Performs the MusicBrainz -> Wikidata -> Wikipedia lookup for one album and
// caches the result. Runs on the resolver thread and nowhere else; it is paced
// by mb_pace() and it must never be reached from a request handler again.
//
// **It never reads album_info_cache**, the same rule resolve_artist_info()
// states and for the same reason: its one caller is lookup_run_job(), which
// always wanted the providers asked. handle_album_info() is what serves the
// cache.
//
// There is no `provider_error` out-parameter, unlike the artist side, because
// there is nowhere to record one: an album has no artist_art to hold a verdict
// in, so the cache write below *is* the verdict and its absence is the retry.
// That is what the mb_ok gate buys, and why it is not optional.
static MediaStore::CachedAlbumInfo resolve_album_info(int id,
                                                      const std::string& title,
                                                      const std::string& artist,
                                                      MediaStore& store)
	{
	MediaStore::CachedAlbumInfo info;
	// The artist is half of the query and was not in the log, so a lookup that
	// found nothing could not be told apart from one that asked for the wrong
	// artist entirely -- which is what a compilation, a folder named after a
	// label, or a tag the scan read as the album artist produces.
	std::cout << stamp() << "getAlbumInfo [" << title
	          << "] querying MusicBrainz, folder " << id
	          << ", artist [" << artist << "]" << std::endl;
	httplib::SSLClient mb("musicbrainz.org");
	provider_timeouts(mb);
	mb.set_default_headers({
		{"User-Agent", USER_AGENT}
		});

	// Whether MusicBrainz answered at all -- see the gate at the bottom. The
	// tags supplying the release-group id counts: the question the search would
	// have asked is already answered, so there is nothing left to be silent
	// about.
	bool mb_ok = false;

	// Step 1 — search for the release-group by title + artist, unless the
	// files already said which one it is.
	//
	// **The tag that answers this is MUSICBRAINZ_RELEASEGROUPID and not
	// MUSICBRAINZ_ALBUMID.** The latter is a *release* — one pressing of
	// one edition — and asking /ws/2/release-group for it is a 404. Both
	// are stored, on albums.musicbrainz_id and
	// albums.musicbrainz_releasegroup_id respectively; only the second one
	// is usable here, so a file tagged with the release alone keeps the
	// search rather than making a request that cannot work.
	info.mbid = store.get_album_tag_releasegroup_mbid(id);
	if (!info.mbid.empty()) {
		mb_ok = true;
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] release-group id from the files' tags, skipping"
		             " search: " << info.mbid << std::endl;
		}

	// One release-group search: the query it sends, every candidate that came
	// back, and the id taken from them. Empty when nothing usable did.  mb_ok
	// is set whenever MusicBrainz answered at all, which is the gate on the
	// cache write at the bottom -- a 200 naming nothing is an answer, a 503 is
	// not.
	auto search = [&](const std::string& want, MbBy by) -> std::string {
		const bool by_rel = by == MbBy::Release;
		const char* path  = by_rel ? "/ws/2/release" : "/ws/2/release-group";

		// **Logged verbatim, because this string is the lookup.** Neither the
		// folder name nor the tags show what actually gets sent, and the
		// difference is where the misses are.  Only the field changes between
		// the two searches: the artist half stays, so the release search is
		// no looser than the release-group one, just pointed at the index
		// that holds reissue titles.
		std::string query = std::string(by_rel ? "release:\"" : "releasegroup:\"")
		                  + mb_escape_phrase(want)
		                  + "\" AND artist:\"" + mb_escape_phrase(artist) + "\"";
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] search: " << path << "?query=" << query
		          << std::endl;
		httplib::Params p1{
			{"query", query},
			// Was 1. The pick below is still the first result, so this
			// changes no outcome; it is a page size on a request being made
			// either way, and the candidates that lose are what say whether
			// MusicBrainz does not have the album or merely spells it
			// differently.
			{"limit", "5"},
			{"fmt",   "json"}
			};
		auto r1 = mb_get(mb, path, p1, "getAlbumInfo [" + title + "]");
		if (!r1) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz request failed (no response)" << std::endl;
			return {};
			}
		if (r1->status != 200) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz HTTP " << r1->status
			          << (mb_rate_limited(r1) ? " - rate limited" : "")
			          << std::endl;
			return {};
			}

		// A 200 is an answer even when it names nothing: MusicBrainz has been
		// asked and has no release group under this title and artist. That is
		// the case the empty cache row exists to record.
		mb_ok = true;
		auto j1 = nlohmann::json::parse(r1->body, nullptr, false);
		if (j1.is_discarded()) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] search reply was not JSON: "
			          << r1->body.substr(0, 200) << std::endl;
			}
		const auto& hits = jsub(j1, by_rel ? "releases" : "release-groups");
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] search returned " << hits.size() << " of "
		          << jint(j1, "count") << (by_rel ? " release(s)"
		                                          : " release-group(s)")
		          << std::endl;
		// Every candidate, not just the winner. A miss is nearly always one of
		// two things and this is what tells them apart: nothing came back at
		// all (the title carries something the release group does not), or
		// something came back under a title or an artist spelled differently
		// from ours and only the score kept it down.
		for (const auto& cand : hits) {
			std::string credit;
			for (const auto& ac : jsub(cand, "artist-credit")) {
				credit += jstr(jsub(ac, "artist"), "name");
				credit += jstr(ac, "joinphrase");
				}
			const auto& grp  = jsub(cand, "release-group");
			std::string type = jstr(by_rel ? grp : cand, "primary-type");
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "]   candidate score=" << jint(cand, "score")
			          << " [" << jstr(cand, "title") << "] by [" << credit
			          << "]" << (type.empty() ? "" : " " + type);
			// **Both titles on the release path.** The whole point of this
			// search is that the two differ -- a release called "The Fillmore
			// Concerts" inside a release group called "At Fillmore East" --
			// so printing only one makes a right answer read like a wrong one.
			if (by_rel)
				std::cout << " -> release-group [" << jstr(grp, "title") << "] "
				          << jstr(grp, "id");
			else
				std::cout << " " << jstr(cand, "id");
			std::cout << std::endl;
			}

		// A release names its release group inline, which is what makes this
		// worth one request: either way what comes out is a release-group id.
		// Same reasoning as the artist search for the is_uuid() check -- it is
		// concatenated into "/ws/2/release-group/" below.
		const auto& top = jidx(hits, 0);
		std::string rg  = by_rel ? jstr(jsub(top, "release-group"), "id")
		                         : jstr(top, "id");
		if (!rg.empty() && !is_uuid(rg)) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] ignoring a release-group id that is not a UUID"
			          << std::endl;
			rg.clear();
			}
		return rg;
		};

	if (info.mbid.empty()) {
		// Each rung runs only when the one above found nothing, so an album
		// that resolves today resolves to exactly what it did before and
		// costs exactly what it cost before.  Every rung is still a phrase
		// query AND'd with the artist: this widens what is asked without
		// loosening what is accepted.
		//
		// The folder's own title at both endpoints before the stripped one at
		// either, because stripping discards information and the most
		// faithful question is worth asking first.  The two fallbacks are
		// complementary rather than redundant, which is measured: the release
		// search is the only thing that finds "The Fillmore Concerts", and
		// strip_album_decoration() is the only thing that finds "Abbey Road
		// (Remastered)" -- for which the release search returns nothing
		// either.
		const std::string bare = strip_album_decoration(title);
		// Second entry empty when nothing was stripped, which is what ends the
		// ladder after one title rather than asking the same thing twice.
		const std::string tries[] = { title, bare == title ? std::string() : bare };

		for (int step = 0; step < 2 && info.mbid.empty(); ++step) {
			const std::string& want = tries[step];
			if (want.empty()) break;
			if (step == 1)
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] nothing under that title; retrying as [" << want
				          << "]" << std::endl;
			for (MbBy by : { MbBy::ReleaseGroup, MbBy::Release }) {
				info.mbid = search(want, by);
				if (!info.mbid.empty()) break;
				}
			}

		if (info.mbid.empty())
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] no release-group taken from the search" << std::endl;
		}

	// Step 2 — fetch URL relations for the release-group.
	if (!info.mbid.empty()) {
		// The one-second wait that used to sit here is mb_get()'s job now;
		// doing it in both places only made every lookup a second slower.
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] fetching /ws/2/release-group/" << info.mbid
		          << "?inc=url-rels" << std::endl;
		auto r2 = mb_get(mb, "/ws/2/release-group/" + info.mbid,
		                 httplib::Params{{"inc","url-rels"},{"fmt","json"}},
		                 "getAlbumInfo [" + title + "] url-rels");
		if (!r2) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz url-rels request failed" << std::endl;
			}
		else if (r2->status != 200) {
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz url-rels HTTP " << r2->status
			          << (mb_rate_limited(r2) ? " - rate limited" : "")
			          << std::endl;
			}
		else {
			auto j2 = nlohmann::json::parse(r2->body, nullptr, false);
			const auto& rels = jsub(j2, "relations");
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] MusicBrainz url-rels: " << rels.size()
			          << " relation(s)";
			for (auto& rel : rels) {
				std::string t = jstr(rel, "type");
				std::cout << " [" << (t.empty() ? "?" : t) << "]";
				}
			std::cout << std::endl;

			// Prefer direct wikipedia relation; fall back to wikidata.
			std::string wiki_title;
			for (auto& rel : rels) {
				std::string type     = jstr(rel, "type");
				std::string resource = jstr(jsub(rel, "url"), "resource");
				if (type == "allmusic" && info.allmusic_url.empty()) {
					info.allmusic_url = clean_url(resource);
					std::cout << stamp() << "getAlbumInfo [" << title
					          << "] AllMusic: " << resource << std::endl;
					}
				else if (type == "wikipedia") {
					auto pos = resource.find("/wiki/");
					if (pos != std::string::npos) {
						wiki_title = resource.substr(pos + 6);
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] Wikipedia (direct): " << wiki_title
						          << std::endl;
						break;
						}
					}
				else if (type == "wikidata" && wiki_title.empty()) {
					auto pos = resource.rfind('/');
					if (pos == std::string::npos) continue;
					std::string entity = resource.substr(pos + 1);
					std::cout << stamp() << "getAlbumInfo [" << title
					          << "] Wikidata entity: " << entity << std::endl;
					httplib::SSLClient wd("www.wikidata.org");
					provider_timeouts(wd);
					wd.set_default_headers({
						{"User-Agent",USER_AGENT}
						});
					auto rwd = wd.Get("/w/api.php",
						httplib::Params{
							{"action","wbgetentities"},{"ids",entity},
							{"props","sitelinks"},{"sitefilter","enwiki"},
							{"format","json"}
							},
						httplib::Headers{});
					if (rwd && rwd->status == 200) {
						auto jwd = nlohmann::json::parse(rwd->body, nullptr, false);
						const auto& ent = jsub(jsub(jwd, "entities"), entity);
						wiki_title = jstr(jsub(jsub(ent, "sitelinks"), "enwiki"),
						                  "title");
						if (!wiki_title.empty())
							std::cout << stamp() << "getAlbumInfo [" << title
							          << "] Wikipedia (via Wikidata): "
							          << wiki_title << std::endl;
						}
					}
				}

			// Step 3 — Wikipedia REST summary → notes text.
			if (wiki_title.empty())
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] no Wikipedia or Wikidata relation on this"
				             " release-group" << std::endl;
			if (!wiki_title.empty()) {
				httplib::SSLClient wp("en.wikipedia.org");
				provider_timeouts(wp);
				wp.set_default_headers({
					{"User-Agent",USER_AGENT}
					});
				// Percent-encoded before it becomes a request line — see
				// the artist chain above for what a CR/LF there would be.
				std::string path_title = wiki_title;
				for (char& c : path_title) if (c == ' ') c = '_';
				path_title = url_encode(path_title);
				auto r3 = wp.Get("/api/rest_v1/page/summary/" + path_title,
				                 httplib::Params{}, httplib::Headers{});
				if (!r3) {
					std::cout << stamp() << "getAlbumInfo [" << title
					          << "] Wikipedia request failed" << std::endl;
					}
				else if (r3->status != 200) {
					std::cout << stamp() << "getAlbumInfo [" << title
					          << "] Wikipedia HTTP " << r3->status << std::endl;
					}
				else {
					auto j3 = nlohmann::json::parse(r3->body, nullptr, false);
					if (!j3.is_discarded()) {
						info.notes    = clean_prose(jstr(j3, "extract"),
						                            MAX_PROSE_BYTES);
						info.wiki_url = "https://en.wikipedia.org/wiki/" + path_title;
						std::cout << stamp() << "getAlbumInfo [" << title
						          << "] notes=" << info.notes.size()
						          << " chars" << std::endl;
						}
					}
				}
			}
		}

	// **Gated on MusicBrainz having answered**, exactly as
	// resolve_artist_info() is. Writing the row either way is what made one
	// 503 — and MusicBrainz sheds load at roughly one request in three — into
	// a permanent "this album has nothing", with no `force` and no TTL to undo
	// it. A row that is not written is asked about again on the next view;
	// that is the whole mechanism.
	if (mb_ok) {
		store.cache_album_info(id, info);
		std::cout << stamp() << "getAlbumInfo [" << title << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}
	else {
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] not cached (MusicBrainz unavailable)" << std::endl;
		}
	return info;
	}

// Shared implementation for getAlbumInfo and getAlbumInfo2.
// key is "albumInfo" or "albumInfo2" — controls the XML element / JSON key.
//
// **This reads the database and nothing else**, which is the whole point of it
// and was not true until recently. It used to run the MusicBrainz release-group
// search, the url-rels lookup, Wikidata and Wikipedia right here, on the
// httplib worker, with no timeouts set on any of the three clients — so one
// uncached album could hold a pool thread for minutes, and httplib dispatches
// one task per *connection*, which means every request a browser had queued
// behind it on that connection waited too. handle_artist_info() had exactly
// this fault and this is the same fix, one endpoint over.
void GainDrive::handle_album_info(const httplib::Request& req,
                                  httplib::Response& res, const char* key)
	{
	bool use_json = (fmt_of(req) == "json");
	auto err = [&](int code, const char* msg) {
		if (use_json)
			res.set_content(subsonic_error_json(code, msg), "application/json");
		else
			res.set_content(subsonic_error(code, msg),      "application/xml");
		};

	auto it = req.params.find("id");
	if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

	int id = to_int(it->second, -1);
	auto album_data = store_.get_album(id);
	if (!album_data) { err(70, "Album not found."); return; }
	// Same rule as handle_artist_info: an id in somebody else's uploads is
	// answered like a missing one, before its title reaches a log line or the
	// resolver queue.
	if (!item_read_allowed(req, store_, uploads_root_name_,
	                       store_.get_folder_path(id))) {
		err(70, "Album not found."); return;
		}

	const std::string& title = album_data->album.title;

	bool force = req.params.count("force") > 0
	          && req.params.find("force")->second != "0";
	// The read stays open to everyone; the re-ask does not. It spends the single
	// paced MusicBrainz gate every other pane is waiting on, and it overwrites a
	// cache the whole server shares — so one account could keep re-resolving an
	// album nobody else wanted re-resolved. Refused rather than quietly ignored:
	// a client that asked for a fresh lookup should hear that it did not get one.
	if (force) {
		auto ui = store_.get_user(req.get_param_value("u"));
		if (!ui || !ui->is_admin) {
			err(50, "Forcing a provider lookup requires admin role."); return;
			}
		}

	auto cached = store_.get_cached_album_info(id);
	MediaStore::CachedAlbumInfo info;
	if (cached) info = *cached;

	// Whether the resolver has *finished* with this album. With the mb_ok gate
	// in resolve_album_info() the presence of a row is the answer: a search
	// that succeeded and found nothing writes an empty row, which is final,
	// while a search that could not be made writes none and is asked again
	// here. That is the album's equivalent of artist_art's status column, one
	// bit wide, and it needs no table because there are no bytes to keep.
	//
	// lookup_pending() is the term `force` needs: the cached row still
	// describes the previous answer until the worker replaces it, so without
	// it a refresh would report itself finished before it had started.
	std::string path      = store_.get_folder_path(id);
	bool        resolving = force || !cached || lookup_pending(path);

	// A film's notes come from TMDB, never from here: an album under a
	// categories root has its section for an artist, so the release-group
	// search can only find nothing or find the wrong record, and either
	// answer is cached for ever. Skipping the ask is strictly better than
	// caching the miss, and it stops every film without a TMDB match from
	// spending the shared MusicBrainz pace budget once per library.
	if (resolving && store_.in_categories_root(path)) resolving = false;

	if (resolving) {
		// Front of the queue: what somebody is looking at beats a pass over
		// the whole library. Already queued or in flight is a no-op.
		lookup_request_front(LookupKind::Album, id, path, title);
		std::cout << stamp() << "getAlbumInfo [" << title << "] "
		          << (force ? "re-queued (force)" : "queued for the resolver")
		          << std::endl;
		}

	// `resolving` is emitted only when it is true, so its absence means the
	// same to a client that has never heard of it as to one that has: this is
	// everything there is. A standard Subsonic client ignores it and gets empty
	// notes on the first view and real ones on the next — the same bargain
	// getArtistInfo2 already makes.
	std::string body;
	if (use_json)
		body = subsonic_ok_json([&info, resolving, key](nlohmann::json& r) {
			nlohmann::json ai = nlohmann::json::object();
			if (!info.mbid.empty())           ai["musicBrainzId"] = info.mbid;
			if (!info.notes.empty())          ai["notes"]         = info.notes;
			if (!info.wiki_url.empty())       ai["wikiUrl"]       = info.wiki_url;
			if (!info.allmusic_url.empty())   ai["allMusicUrl"]   = info.allmusic_url;
			if (resolving)                    ai["resolving"]     = true;
			r[key] = ai;
			});
	else {
		auto add_text_el = [](XMLDocument& doc, XMLElement* parent,
		                       const char* tag, const std::string& val) {
			if (val.empty()) return;
			auto* el = doc.NewElement(tag);
			el->SetText(val.c_str());
			parent->InsertEndChild(el);
			};
		body = subsonic_ok([&info, &add_text_el, resolving, key]
		                   (XMLDocument& doc, XMLElement* root) {
			auto* ai = doc.NewElement(key);
			add_text_el(doc, ai, "musicBrainzId", info.mbid);
			add_text_el(doc, ai, "notes",         info.notes);
			add_text_el(doc, ai, "wikiUrl",       info.wiki_url);
			add_text_el(doc, ai, "allMusicUrl",   info.allmusic_url);
			if (resolving) add_text_el(doc, ai, "resolving", "true");
			root->InsertEndChild(ai);
			});
		}
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// The artist folders with nothing resolved yet, oldest question first. Called
// once at start and again whenever the queue drains, which is how an artist a
// scan has just added is picked up without the scanner needing to know this
// exists.
// Guards its own body, because both its callers are on the worker thread and
// an exception escaping there is std::terminate. A failed seed costs this pass
// and nothing else: the fifteen-minute timer asks again.
void GainDrive::lookup_seed()
	{
	// Guarded here rather than at its two call sites, because both of them are
	// on the worker thread and an exception escaping there is std::terminate.
	// A failed seed costs this pass and nothing else: the fifteen-minute timer
	// asks again.
	try {
		// A 'none' — the providers had nothing — is worth re-asking about
		// after a month; an 'error' says the network failed and is retried at
		// once, which artists_needing_art() handles by not excluding it at all.
		const int64_t month_ago = static_cast<int64_t>(std::time(nullptr))
		    - 30LL * 24 * 3600;
		auto jobs = store_.artists_needing_art(month_ago);

		std::lock_guard<std::mutex> lk(lookup_mu_);
		for (auto& j : jobs) {
			if (lookup_queued_.count(j.path)) continue;
			lookup_queued_.insert(j.path);
			lookup_queue_.push_back(
				LookupJob{LookupKind::Artist, j.folder_id, j.path, j.name});
			}
		if (!lookup_queue_.empty())
			std::cout << stamp() << "Artist portraits: " << lookup_queue_.size()
			          << " to resolve" << std::endl;
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "Info resolver seed failed: " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "Info resolver seed failed: unknown exception"
		          << std::endl;
		}
	}

// The start-up seed runs at the same moment the scan thread is detached, so on
// a first scan of an empty library it asks a database with no artists in it,
// finds nothing, and sleeps.  Nothing in the scanner knew this thread existed,
// so the first portrait was fetched up to fifteen minutes after the artists it
// wanted had appeared — on a cold start, always.
//
// This is the notification the scanner owes it.  Deliberately only "look
// again", not a queue: what needs looking up is a database question that
// lookup_seed() already answers, and duplicating that here would be a second
// definition of which artists want art.
void GainDrive::lookup_wake()
	{
	{
	std::lock_guard<std::mutex> lock(lookup_mu_);
	lookup_reseed_ = true;
	}
	lookup_cv_.notify_one();
	}

void GainDrive::lookup_request_front(LookupKind kind, int folder_id,
                                     const std::string& path,
                                     const std::string& name)
	{
	{
	std::lock_guard<std::mutex> lk(lookup_mu_);
	// Already queued: move it to the front rather than adding it twice. What
	// a client is looking at right now should not wait behind the alphabet.
	for (auto it = lookup_queue_.begin(); it != lookup_queue_.end(); ++it) {
		if (it->path == path) {
			LookupJob j = *it;
			lookup_queue_.erase(it);
			lookup_queue_.push_front(std::move(j));
			lookup_cv_.notify_one();
			return;
			}
		}
	if (lookup_queued_.count(path)) return;   // in flight; it will finish
	lookup_queued_.insert(path);
	lookup_queue_.push_front(LookupJob{kind, folder_id, path, name});
	}
	lookup_cv_.notify_one();
	}

// The back of the same queue. No move-to-back counterpart to the loop above:
// something already queued is already going to be looked up, and dragging it
// behind ten thousand others would undo a client's own request.
// Answers whether it queued anything, so the caller's count is what was
// really added rather than what it considered -- a pass started while the
// previous one is still running must not claim the overlap twice.
bool GainDrive::lookup_request_back(LookupKind kind, int folder_id,
                                    const std::string& path,
                                    const std::string& name)
	{
	{
	std::lock_guard<std::mutex> lk(lookup_mu_);
	if (lookup_queued_.count(path)) return false;
	lookup_queued_.insert(path);
	lookup_queue_.push_back(LookupJob{kind, folder_id, path, name});
	}
	lookup_cv_.notify_one();
	return true;
	}

// startInfoLookup's whole body of work. Deliberately *not* folded into
// lookup_seed(): that one answers "what has never been resolved", runs on a
// timer, and must stay cheap, while this one answers "what has no words" and
// is asked for by hand. Conflating them would make every fifteen-minute wake
// re-ask about every artist the providers have nothing to say about.
GainDrive::LookupSeeded GainDrive::lookup_seed_missing_info(bool artists,
                                                            bool albums)
	{
	LookupSeeded n;
	if (artists)
		for (auto& j : store_.artists_needing_bio())
			if (lookup_request_back(LookupKind::Artist, j.folder_id,
			                        j.path, j.name))
				n.artists++;
	if (albums)
		for (auto& j : store_.albums_needing_info())
			if (lookup_request_back(LookupKind::Album, j.folder_id,
			                        j.path, j.name))
				n.albums++;
	return n;
	}

// See the header. lookup_queued_ covers both halves of "pending": a job is
// inserted when it is queued and erased only after store_artist_art() has run,
// so the in-flight window is inside it too.
bool GainDrive::lookup_pending(const std::string& path)
	{
	std::lock_guard<std::mutex> lk(lookup_mu_);
	return lookup_queued_.count(path) > 0;
	}

// Downloads one image URL and normalises it to something that can be stored
// and scaled later. Never throws: this runs on a background thread, where an
// escaping exception is std::terminate.
MediaStore::ArtistArtRow GainDrive::portrait_fetch(const std::string& url)
	{
	MediaStore::ArtistArtRow row;
	row.status     = "error";
	row.source_url = url;

	try {
		bool https = url.rfind("https://", 0) == 0;
		bool http  = url.rfind("http://",  0) == 0;
		if (!https && !http) return row;

		std::string fetch_url = url;
		// Ask Wikimedia for a render, never the original. Special:FilePath
		// with no width serves the *file* behind a P18 claim, which is quite
		// often an SVG or a multi-megabyte TIFF — undecodable here and a waste
		// of bandwidth even when it is a JPEG. With a width it rasterises.
		if (fetch_url.find("wikimedia.org/wiki/Special:FilePath") != std::string::npos
		    && fetch_url.find("width=") == std::string::npos)
			fetch_url += (fetch_url.find('?') == std::string::npos ? "?" : "&")
			           + std::string("width=") + std::to_string(PORTRAIT_PX);

		// Redirects are followed by hand, exactly as setCoverArt does, and for
		// the same reason: the address check has to run again on every hop,
		// because a public URL that 302s to 127.0.0.1 is the ordinary way a
		// check that only looks at the first address is defeated.  Commons
		// answers Special:FilePath with a 302, so a portrait genuinely needs
		// the hops — set_follow_location(true) was how they were taken, which
		// meant no hop was checked at all.
		//
		// This URL is not typed by anyone: it arrives in a provider's JSON,
		// which is a narrower source than setCoverArt's but not a trusted one,
		// and it is dereferenced by the server rather than by a browser.  So
		// the whole LAN, loopback and any link-local metadata service were
		// reachable through whatever MusicBrainz, Wikipedia, TheAudioDB or
		// Discogs returned.
		//
		// The residual is the one setCoverArt records: this resolution and
		// httplib's own when it connects are two lookups, so DNS rebinding
		// stays open.  Closing it needs a client that can be handed an address
		// rather than a name.
		std::string next = fetch_url;
		httplib::Result r;
		for (int hop = 0; ; ++hop) {
			if (hop > MAX_COVER_REDIRECTS) {
				std::cout << stamp() << "Artist portrait: too many redirects for "
				          << url << std::endl;
				return row;
				}
			bool hop_https = next.rfind("https://", 0) == 0;
			bool hop_http  = next.rfind("http://",  0) == 0;
			if (!hop_https && !hop_http) {
				std::cout << stamp() << "Artist portrait: refusing a non-http(s)"
				             " redirect from " << url << std::endl;
				return row;
				}
			size_t scheme_end = hop_https ? 8 : 7;
			size_t slash      = next.find('/', scheme_end);
			std::string host  = next.substr(scheme_end,
				slash == std::string::npos ? std::string::npos : slash - scheme_end);
			std::string path  = (slash == std::string::npos) ? "/"
			                                                 : next.substr(slash);

			if (!host_is_global(host)) {
				std::cout << stamp() << "Artist portrait: refusing non-global host: "
				          << host << std::endl;
				return row;
				}

			auto get = [&](auto& cli) {
				cli.set_follow_location(false);
				cli.set_connection_timeout(5);
				cli.set_read_timeout(15);
				cli.set_default_headers({{"User-Agent", USER_AGENT}});
				return cli.Get(path.c_str());
				};
			auto [hname, hport] = split_host_port(host, hop_https ? 443 : 80);
			if (hop_https) { httplib::SSLClient cli(hname, hport); r = get(cli); }
			else           { httplib::Client    cli(hname, hport); r = get(cli); }
			if (!r) return row;
			if (r->status == 301 || r->status == 302 || r->status == 303
			    || r->status == 307 || r->status == 308) {
				std::string loc = r->get_header_value("Location");
				if (loc.empty()) return row;
				next = loc;
				continue;
				}
			break;
			}
		if (r->status != 200 || r->body.empty()) return row;

		// These are arbitrary third-party URLs; nothing about them is bounded
		// except by us.
		if (r->body.size() > MAX_PORTRAIT_BYTES) {
			std::cout << stamp() << "Artist portrait: " << url << " is "
			          << r->body.size() << " bytes; refusing it" << std::endl;
			return row;
			}

		// Normalise. Whatever a provider sent becomes one predictable thing at
		// a known bound, so scaling it later needs no second download and no
		// second guess about the format.
		//
		// Fit::Long, unlike every thumbnail: this is not one. It bounds the
		// *stored* original, and "neither edge past 800" is what bounds bytes
		// somebody else chose the shape of. The square crops happen later, on
		// the way out through CoverArtCache, which asks for Fit::Short.
		auto s = imagescale::scale_to_fit(r->body, PORTRAIT_PX,
		                                  imagescale::Fit::Long);
		if (!s.ok) {
			std::cout << stamp() << "Artist portrait: cannot decode " << url
			          << " (" << s.error << ")" << std::endl;
			return row;
			}

		row.status = "ok";
		row.mime   = s.mime;
		row.width  = s.width;
		row.height = s.height;
		row.bytes  = std::move(s.bytes);
		return row;
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "Artist portrait: " << url << ": " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "Artist portrait: " << url
		          << ": unknown exception while fetching" << std::endl;
		}
	return row;
	}

// One job, providers and all. Split out of lookup_worker() so that the guard
// below wraps *a job* rather than the loop: an exception escaping this thread
// is std::terminate, so it has to be caught somewhere, but catching it around
// the whole loop meant one bad row silently ended every biography, every album
// description and every portrait until the next restart -- with getArtistInfo2
// answering `resolving` for ever, since artist_art never got its row.
void GainDrive::lookup_run_job(const LookupJob& job)
	{
	if (job.kind == LookupKind::Album) {
		// Same contract as the artist branch: the providers, never the cache.
		// resolve_album_info() writes album_info_cache itself, and there is
		// nothing else to record here -- no bytes to keep, and no verdict
		// column, because with the mb_ok gate a row's *existence* is the
		// verdict. See handle_album_info().
		auto album = store_.get_album(job.folder_id);
		// Pruned between being queued and being reached. Nothing to resolve
		// and nothing to complain about: a rescan is allowed to remove an
		// album while a pass over the library is running.
		if (!album) return;
		resolve_album_info(job.folder_id, album->album.title,
		                   album->album.artist, store_);
		return;
		}

	MediaStore::ArtistArtRow row;
	// A categories section is called "Film" or "Series"; asking MusicBrainz
	// about that is exactly the mistake is_category_folder exists to prevent.
	// The seed queries already exclude them, but a demand request comes
	// straight from an id in a URL.
	if (store_.is_category_folder(job.folder_id)) {
		row.status = "none";
		}
	else {
		// Always the providers and never artist_info_cache — see the note on
		// resolve_artist_info(). This call is also what fills that cache, so
		// it resolves the biography getArtistInfo2 serves as much as it
		// resolves the picture.
		bool provider_error = false;
		auto info = resolve_artist_info(job.folder_id, job.name, store_,
		                                &provider_error);
		if (provider_error && info.image_url.empty()) {
			// A provider did not answer — a 503 from MusicBrainz is the usual
			// one, since it rate-limits hard. We have learnt nothing about
			// this artist, so record that rather than a verdict: 'error' is
			// retried, 'none' is not touched for a month. Getting this wrong
			// made one rate-limited moment look exactly like "nobody has a
			// picture of them".
			row.status = "error";
			}
		else if (info.image_url.empty()) {
			// Every provider was asked and none had one. Recorded, or every
			// pass would ask again.
			row.status = "none";
			}
		else {
			row = portrait_fetch(info.image_url);
			}
		}

	store_.store_artist_art(job.path, job.name, row);
	cover_cache_.invalidate(job.path);

	if (row.status == "ok")
		std::cout << stamp() << "Artist portrait: " << job.name << " "
		          << row.width << "x" << row.height << ", "
		          << row.bytes.size() << " bytes" << std::endl;
	else
		// Which of the two it is matters — "error" will be asked again, "none"
		// will not for a month — so say which, rather than leaving the
		// difference to be inferred from behaviour a month later.
		std::cout << stamp() << "Artist portrait: " << job.name
		          << ": " << row.status
		          << (row.status == "error"
		                  ? " (a provider did not answer; will retry)"
		                  : " (no provider has one)") << std::endl;
	}

// One thread, one queue, both kinds of job.
//
// **Two guards, and they are not redundant.** The inner one is around
// lookup_run_job(), and it is the one that matters: a provider's malformed JSON
// or a contended store_artist_art() costs that artist or album and the loop
// carries on. The outer one is here only for what the inner one cannot reach —
// the queue's own mutex and condition variable — and it ends the thread,
// because a loop whose lock does not work has nothing useful left to do.
//
// This used to be the outer guard alone, and that is the shape to keep away
// from: one SQLite exception silently ended every biography, description and
// portrait until the next restart, with getArtistInfo2 answering `resolving`
// for ever afterwards because artist_art never got its row.
void GainDrive::lookup_worker()
	{
	try {
	lookup_seed();

	while (!lookup_stop_) {
		LookupJob job;
		{
		std::unique_lock<std::mutex> lk(lookup_mu_);
		if (lookup_queue_.empty()) {
			// Nothing to do: sleep, then look again. That is how an artist
			// added by a scan since the last pass is found, and the timer is
			// the fallback for a scan nothing told us about — the folder
			// watcher's, an upload's, a URL fetch's.
			//
			// lookup_reseed_ is in the predicate because the queue is still
			// empty when a scan finishes: waking on !lookup_queue_.empty()
			// alone would re-evaluate to false and go straight back to sleep
			// for the rest of the fifteen minutes, which is precisely the wait
			// being removed.
			lookup_cv_.wait_for(lk, std::chrono::minutes(15),
				[this] { return lookup_stop_ || lookup_reseed_
				              || !lookup_queue_.empty(); });
			if (lookup_stop_) break;
			if (lookup_queue_.empty()) {
				// Cleared here, where it is acted on, and not on every wake: a
				// scan finishing while this thread was waking for a queued
				// request would otherwise clear the flag without ever
				// re-seeding, and the artists that scan added would wait for
				// the timer after all.
				lookup_reseed_ = false;
				lk.unlock();
				lookup_seed();
				continue;
				}
			}
		job = lookup_queue_.front();
		lookup_queue_.pop_front();
		}

		if (lookup_stop_) break;

		try { lookup_run_job(job); }
		catch (const std::exception& e) {
			std::cout << stamp() << "Lookup for [" << job.name
			          << "] failed: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Lookup for [" << job.name
			          << "] failed: unknown exception" << std::endl;
			}

		// Erased here rather than inside the job, so that it happens on the
		// failing path too. Leaving it in the set would make lookup_pending()
		// say "being resolved right now" for ever, which is a client polling a
		// question nobody is answering.
		{
		std::lock_guard<std::mutex> lk(lookup_mu_);
		lookup_queued_.erase(job.path);
		}

		// The gap between jobs, and it is never skipped.
		//
		// MusicBrainz allows one request a second per address and answers 503
		// when that is exceeded. Either chain makes two MusicBrainz requests
		// per job, the artist one with a one-second sleep between them, so this
		// wait is what keeps the sustained rate under the limit — and being
		// rate-limited is not a harmless slowdown here, because a 503 is
		// indistinguishable from "this artist has no picture" unless the code
		// is careful, and one full-speed pass over a library would earn a great
		// many of them.
		//
		// An earlier version skipped the wait whenever the queue was not empty,
		// meaning to let a user who was waiting jump ahead. But a seeded
		// backlog leaves the queue permanently non-empty, so the pacing never
		// applied at all during precisely the pass that needed it — and
		// startInfoLookup seeds one deliberately. A demand request already gets
		// what it needs by going to the front of the queue; it does not also
		// need to outrun the rate limit.
		{
		std::unique_lock<std::mutex> lk(lookup_mu_);
		lookup_cv_.wait_for(lk, LOOKUP_GAP,
			[this] { return lookup_stop_.load(); });
		}
		}
	}
	catch (const std::exception& e) {
		std::cout << stamp() << "Info resolver thread stopped: " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "Info resolver thread stopped: unknown exception"
		          << std::endl;
		}
	}

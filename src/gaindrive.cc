#include "gaindrive.hh"
#include "stamp.hh"
#include "streamer.hh"
#include "codecs.hh"
#include "imagescale.hh"
#include "jsonread.hh"
#include "untrusted.hh"
#include "artistmatch.hh"
#include "embedded_web.hh"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <random>
#include <set>
#include <sstream>
#include <thread>

#include <openssl/rand.h>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

#include <archive.h>
#include <archive_entry.h>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>
#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <tpropertymap.h>

using namespace tinyxml2;

// ---- Subsonic XML helpers ---------------------------------------------

static const char* SUBSONIC_NS  = "http://subsonic.org/restapi";
static const char* SUBSONIC_VER = "1.16.1";

// OpenSubsonic requires a server that sets openSubsonic=true to also identify
// itself: `type` is the implementation, `serverVersion` is our own version as
// distinct from the API version above.
static const char* SERVER_TYPE    = "gaindrive";
static const char* SERVER_VERSION = GAINDRIVE_VERSION;

// Sent on every outbound metadata request. One definition rather than the nine
// copies this used to be, so a version bump cannot leave some of them behind.
static const char* USER_AGENT =
	"GainDrive/" GAINDRIVE_VERSION " (info@phi-sci.com)";

// An artist portrait is stored at this bound on its long edge — comfortably
// above the largest any client asks for (iOS wants 800 for a hero), so the
// stored image is never the limiting factor, and small enough that a database
// row stays a sensible place to keep it.
static constexpr int PORTRAIT_PX = 800;

// A portrait URL points at a third party, and nothing about what comes back is
// bounded except by us. Wikimedia renders are tens of kilobytes; anything at
// this scale is a mistake somewhere.
static constexpr size_t MAX_PORTRAIT_BYTES = 8u * 1024 * 1024;

// The same for a cover fetched by setCoverArt, which is a URL a person typed
// rather than one a provider returned, and so is bounded for the same reason
// and one more: this one is reachable by any account allowed to edit the item.
static constexpr size_t MAX_COVER_BYTES     = 16u * 1024 * 1024;
static constexpr int    MAX_COVER_REDIRECTS = 5;

// The ceiling on one upload archive as it streams through /upload, and
// therefore also httplib's global payload cap — the content-reader path
// enforces that cap on what it hands the receiver. Every other route is
// bounded far lower, before a byte of body is read, by the pre-request
// handler and MAX_SMALL_BODY_BYTES.
static constexpr size_t MAX_REQUEST_BYTES = 4ull * 1024 * 1024 * 1024;

// The body bound for every route that is not /upload. Nothing else accepts a
// large body: the biggest legitimate one is a saveChapters file, which
// MAX_CHAPTERS keeps to a few tens of kilobytes.
static constexpr size_t MAX_SMALL_BODY_BYTES = 1024 * 1024;

// Bounds on what one search may ask for. getAlbumList and getRecentSongs have
// clamped for as long as they have existed; search did not, and its LIKE
// pattern can be made to match everything, so the two together turn one
// request into the whole library serialised into a single in-memory document.
static constexpr int MAX_SEARCH_COUNT = 500;

// The matching bound on every paging offset. Unbounded, OFFSET is a full
// table walk inside SQLite — LIMIT 500 OFFSET 100000000 visits every row to
// discard it — executed while holding db_mutex_, behind which every other
// request in the server queues, authentication included. A million deep is
// past the end of any real library and costs a bounded walk.
static constexpr int MAX_LIST_OFFSET = 1000000;

// What one uploaded archive may expand to. An archive's compressed size bounds
// nothing — the ratio is the attack — so the extraction has to carry its own
// limit or a few megabytes fills the uploads volume, which is a filesystem
// other people's music is also on.
static constexpr uint64_t MAX_ARCHIVE_BYTES   = 8ull * 1024 * 1024 * 1024;
static constexpr int      MAX_ARCHIVE_ENTRIES = 20000;

// Wait between artists in the portrait resolver. The MusicBrainz gate below is
// what actually enforces the rate limit; this is a courtesy gap on top, so a
// background pass over the whole library leaves headroom for a client's own
// getArtistInfo2 rather than keeping the gate permanently saturated.
static constexpr auto PORTRAIT_GAP = std::chrono::milliseconds(2000);

// ---- MusicBrainz rate limiting ----------------------------------------
//
// **One gate for the whole process, and every request to musicbrainz.org goes
// through it.** The limit is per IP address, not per caller, so pacing each
// caller separately does not add up to anything: the artist chain paced itself
// through the portrait worker, while getAlbumInfo — which makes two more
// MusicBrainz requests per album — ran straight off the HTTP thread pool with
// no pacing at all, up to 32 at a time. A client browsing a library would then
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
// portrait queue is a single thread, so one unreachable provider stalls every
// artist behind it.
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
// handle_album_info()'s three clients are deliberately left alone for now; it
// is still synchronous on the request thread and wants the whole treatment
// this endpoint just had, not half of it.
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

// Subsonic ids are strings in the API even though they are row ids here. Every
// id crossing the wire in JSON goes through this — the XML path renders
// attributes as text anyway, so it needs no equivalent.
static std::string sid(int id)
	{
	return std::to_string(id);
	}

// Numeric query params, without letting a malformed one escape the handler.
// std::stoi throws on garbage and on overflow; httplib turns that into a bare
// HTTP 500, which no Subsonic client can interpret — they expect a 200 with an
// <error> body.  Every request parameter goes through these; a bare std::stoi
// below this point reads a value the server itself produced.
static int to_int(const std::string& s, int def)
	{
	if (s.empty()) return def;
	try { return std::stoi(s); } catch (...) { return def; }
	}

static float to_float(const std::string& s, float def)
	{
	if (s.empty()) return def;
	try { return std::stof(s); } catch (...) { return def; }
	}

static int64_t to_int64(const std::string& s, int64_t def)
	{
	if (s.empty()) return def;
	try { return std::stoll(s); } catch (...) { return def; }
	}

// SQLite CURRENT_TIMESTAMP formats as "YYYY-MM-DD HH:MM:SS" in UTC, but the
// API wants ISO 8601. Same instant, different spelling. Empty in, empty out,
// so callers can keep using emptiness to mean "absent".
static std::string iso8601(const std::string& ts)
	{
	if (ts.empty()) return ts;
	std::string s = ts;
	if (s.size() > 10 && s[10] == ' ') s[10] = 'T';
	if (s.back() != 'Z') s += 'Z';
	return s;
	}

// Creates a <subsonic-response> root element inside doc and returns it.
static XMLElement* make_root(XMLDocument& doc, const char* status)
	{
	doc.InsertEndChild(doc.NewDeclaration());
	auto* root = doc.NewElement("subsonic-response");
	root->SetAttribute("xmlns",        SUBSONIC_NS);
	root->SetAttribute("status",        status);
	root->SetAttribute("version",       SUBSONIC_VER);
	root->SetAttribute("type",          SERVER_TYPE);
	root->SetAttribute("serverVersion", SERVER_VERSION);
	root->SetAttribute("openSubsonic",  "true");
	doc.InsertEndChild(root);
	return root;
	}

static std::string to_string(XMLDocument& doc)
	{
	XMLPrinter printer;
	doc.Print(&printer);
	return printer.CStr();
	}

// Build a complete ok response, optionally populated by a callback.
static std::string subsonic_ok(
	std::function<void(XMLDocument&, XMLElement*)> fn = {})
	{
	XMLDocument doc;
	auto* root = make_root(doc, "ok");
	if (fn) fn(doc, root);
	return to_string(doc);
	}

static std::string subsonic_error(int code, const char* msg)
	{
	XMLDocument doc;
	auto* root = make_root(doc, "failed");
	auto* err  = doc.NewElement("error");
	err->SetAttribute("code",    code);
	err->SetAttribute("message", msg);
	root->InsertEndChild(err);
	return to_string(doc);
	}

// ---- Subsonic JSON helpers --------------------------------------------

static std::string subsonic_ok_json(
	std::function<void(nlohmann::json&)> fn = {})
	{
	nlohmann::json r;
	r["status"]        = "ok";
	r["version"]       = SUBSONIC_VER;
	r["type"]          = SERVER_TYPE;
	r["serverVersion"] = SERVER_VERSION;
	r["openSubsonic"]  = true;
	if (fn) fn(r);
	nlohmann::json j;
	j["subsonic-response"] = r;
	return j.dump();
	}

static std::string subsonic_error_json(int code, const char* msg)
	{
	nlohmann::json j;
	j["subsonic-response"]["status"]           = "failed";
	j["subsonic-response"]["version"]          = SUBSONIC_VER;
	j["subsonic-response"]["type"]             = SERVER_TYPE;
	j["subsonic-response"]["serverVersion"]    = SERVER_VERSION;
	j["subsonic-response"]["openSubsonic"]     = true;
	j["subsonic-response"]["error"]["code"]    = code;
	j["subsonic-response"]["error"]["message"] = msg;
	return j.dump();
	}

// Returns "json" if the client requested JSON, otherwise "xml".
static std::string fmt_of(const httplib::Request& req)
	{
	auto it = req.params.find("f");
	return (it != req.params.end() && it->second == "json") ? "json" : "xml";
	}

// The address to attribute a request to in the log.
//
// Behind a reverse proxy every request arrives *from the proxy*, so
// `remote_addr` is the same value for all of them and distinguishes nothing —
// which matters most for exactly the question it is there to answer: whether a
// request came from a browser or from a Chromecast fetching for itself. Apache
// and nginx both pass the original along in `X-Forwarded-For`, a
// comma-separated chain.
//
// **The entry taken is the rightmost one that is not a trusted proxy, never
// the first.** Proxies *append* the peer they heard from, so what arrives here
// is `<whatever the client sent>, <real client>` — the leftmost entry is the
// one string in the chain the attacker composed in full. This code used to
// take it, which handed the login throttle's bucket to the caller: send a
// fresh X-Forwarded-For per request and there is no rate limit at all.
// httplib::get_client_ip() walks from the right and skips configured proxies,
// which is the standard answer, and one copy of it beats a second one here.
//
// **Trusted only from a configured proxy.** The header is client-supplied and
// forgeable, so honouring it from any peer lets a caller write whatever it
// likes into the log — and, since the login throttle keys on this, choose its
// own rate-limit bucket. It used to be trusted unconditionally, which was
// defensible while nothing read it back; the throttle is what made that stop
// being true.
//
// The default list is loopback, because a proxy on the same host is what the
// packaging sets up and what `host: 127.0.0.1` in the example config assumes.
// A server reachable directly gets `remote_addr`, which cannot be spoofed past
// the TCP handshake.
static std::vector<std::string> trusted_proxies_ = { "127.0.0.1", "::1" };

// An IPv4 peer on a dual-stack listener shows up as `::ffff:127.0.0.1`, which
// is the same host as `127.0.0.1` and must match the same configuration entry.
// Compared both ways so a proxy list written in either spelling works.
static std::string strip_v4_mapped(const std::string& addr)
	{
	static const std::string prefix = "::ffff:";
	if (addr.rfind(prefix, 0) == 0 &&
	    addr.find('.') != std::string::npos)
		return addr.substr(prefix.size());
	return addr;
	}

void gaindrive_set_trusted_proxies(std::vector<std::string> addrs)
	{
	trusted_proxies_ = std::move(addrs);
	}

// The origin this server is reachable at — scheme and host, no trailing
// slash — when a deployment knows it. Consulted wherever a URL for a third
// device is composed; empty means "believe the request's Host header", which
// is right on a LAN, where there is no other source, and wrong on the public
// internet, where Host is whatever the caller typed.
static std::string public_url_;

void gaindrive_set_public_url(std::string origin)
	{
	public_url_ = std::move(origin);
	}

static bool from_trusted_proxy(const httplib::Request& req)
	{
	const std::string peer = strip_v4_mapped(req.remote_addr);
	for (const auto& p : trusted_proxies_)
		if (strip_v4_mapped(p) == peer) return true;
	return false;
	}

static std::string client_addr(const httplib::Request& req)
	{
	if (!from_trusted_proxy(req)) return req.remote_addr;

	const std::string xff = req.get_header_value("X-Forwarded-For");
	if (xff.empty()) return req.remote_addr;

	// Rightmost untrusted entry — see the comment on trusted_proxies_.
	std::string addr = httplib::get_client_ip(xff, trusted_proxies_);
	if (addr.empty()) return req.remote_addr;

	// Some proxies write `1.2.3.4:5678` or `[2001:db8::1]:443`. The throttle
	// keys on this string, so a port would split one client into many buckets.
	if (addr.front() == '[') {
		const auto close = addr.find(']');
		if (close != std::string::npos) addr = addr.substr(1, close - 1);
		}
	else if (std::count(addr.begin(), addr.end(), ':') == 1)
		addr = addr.substr(0, addr.find(':'));
	return addr;
	}

// A string safe to put in a log line: control characters replaced and the
// length bounded.
//
// Everything logged here — a path, a parameter, a username — arrives from the
// network, and it was written out raw. A CR or LF in any of it forges whole
// log lines, which matters more once something downstream reads this log to
// decide whom to block, and a long value simply makes the log useless.
static std::string log_safe(const std::string& s, size_t max_bytes = 512)
	{
	std::string out;
	const size_t n = std::min(s.size(), max_bytes);
	out.reserve(n);
	for (size_t i = 0; i < n; ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		out += (c < 0x20 || c == 0x7f) ? '.' : s[i];
		}
	if (s.size() > max_bytes) out += "...";
	return out;
	}

// ---- Helpers ----------------------------------------------------------

// The lowercased extension of a path with no leading dot, which is the form
// songs.codec holds and therefore the form is_video_ext() and codec_to_mime()
// expect. Empty for a path with no extension.
static std::string ext_of(const std::string& path)
	{
	std::string ext = std::filesystem::path(path).extension().string();
	if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext;
	}

// Returns the name stripped of a leading article ("The ", "A ", …) for
// alphabetical index grouping.
static std::string sort_key(const std::string& name)
	{
	static const std::vector<std::string> articles = {
		"The ","A ","An ","El ","La ","Los ","Las ",
		"Le ","Les ","Die ","Das ","Ein ","Eine "
		};
	for (auto& art : articles)
		if (name.size() > art.size() && name.substr(0, art.size()) == art)
			return name.substr(art.size());
	return name;
	}

// Sorts level-1 folders and groups them into the index buckets `getArtists` and
// `getIndexes` both answer with.
//
// Shared because those two handlers held byte-identical copies of it, differing
// only in the wrapper key of the response — and the first thing anyone edits one
// of them for, they will not think to do twice.
//
// Two groupings, because there are two questions. Normally the bucket is the
// first letter, which is what the alphabet rail and the sticky headers are for.
// When an admin is looking at *everybody's* uploads it is the owner instead:
// that is the only thing telling two identically named folders apart, and
// grouping by letter there would interleave two people's material under one
// heading with nothing to say whose is whose.
static std::map<std::string, std::vector<const MediaStore::ArtistDir*>>
index_buckets(std::vector<MediaStore::ArtistDir>& artists, bool by_owner)
	{
	std::sort(artists.begin(), artists.end(),
		[by_owner](const MediaStore::ArtistDir& a, const MediaStore::ArtistDir& b) {
			// Owner first when grouping by it, so each person's own list is
			// still alphabetical inside their heading.
			if (by_owner && a.owner != b.owner) return a.owner < b.owner;
			return sort_key(a.name) < sort_key(b.name);
			});

	std::map<std::string, std::vector<const MediaStore::ArtistDir*>> buckets;
	for (auto& a : artists) {
		if (by_owner) { buckets[a.owner].push_back(&a); continue; }
		std::string key    = sort_key(a.name);
		std::string letter = key.empty() || !std::isalpha((unsigned char)key[0])
		                   ? "#"
		                   : std::string(1, (char)std::toupper((unsigned char)key[0]));
		buckets[letter].push_back(&a);
		}
	return buckets;
	}

// Resolves the `personal` parameter to what get_artist_dirs() wants.
//
// "true" is the caller's own uploads; "*" is everybody's and is **admin only**,
// because it is other people's material. Anything else, including absent, is the
// shared library. Returns false having already written the refusal.
static bool personal_scope(const httplib::Request& req, httplib::Response& res,
                           MediaStore& store, bool use_json, std::string& out)
	{
	const std::string want = req.get_param_value("personal");
	const std::string uname = req.get_param_value("u");
	if (want.empty() || want == "false") { out.clear(); return true; }
	if (want != MediaStore::PERSONAL_ALL_USERS) { out = uname; return true; }

	auto ui = store.get_user(uname);
	if (!ui || !ui->is_admin) {
		const char* msg = "Listing every account's uploads requires admin role.";
		res.set_content(use_json ? subsonic_error_json(50, msg)
		                         : subsonic_error(50, msg),
		                use_json ? "application/json" : "application/xml");
		return false;
		}
	out = MediaStore::PERSONAL_ALL_USERS;
	return true;
	}

// What stream.view would actually send for this song, when that differs from
// the stored file.  Mirrors the branch order in Streamer::serve() — a format
// override wins over the per-user bitrate cap, and the cap alone means mp3.
// Empty when the source is served as-is.
struct TranscodeInfo
	{
	std::string_view mime;
	std::string_view suffix;
	int              bitrate;
	};

static std::optional<TranscodeInfo> transcode_target(
	const MediaStore::ChildEntry& c, int max_bitrate,
	const std::string& format = "")
	{
	// Video: the container is what changes, not the audio muxer, so none of
	// the negotiation below applies.  Anything that is not already in a
	// browser-playable container arrives as MP4, whether that took a remux or
	// a full re-encode.  Whether it is *seekable* is a different question and
	// is answered separately by nativeSeek — see song_entry_json().
	if (is_video_ext(c.codec)) {
		// Same predicate serve_video() picks its tier with, so the advertised
		// type cannot disagree with what the stream turns out to be.
		if (video_direct_playable(c.codec, c.video_codec, c.audio_codec))
			return std::nullopt;   // served untouched
		return TranscodeInfo{ VIDEO_MP4_MIME, "mp4", 0 };
		}

	auto source = target_for(c.codec);
	std::optional<Target> wanted;
	if (!format.empty() && format != "raw")
		wanted = target_for(format);
	// Same muxer and encoder is the same audio, whatever the caller spelled it.
	if (wanted && (!source || source->muxer   != wanted->muxer
	                       || source->encoder != wanted->encoder)) {
		// Keep this rule identical to the one in Streamer::serve(); a client
		// that trusts transcodedBitRate and then receives something else has
		// no way to tell which of the two lied.
		int bitrate = (max_bitrate > 0 && max_bitrate < 320) ? max_bitrate : 320;
		return TranscodeInfo{ wanted->mime, wanted->name, bitrate };
		}
	if (max_bitrate <= 0 || c.bitrate <= 0 || c.bitrate <= max_bitrate)
		return std::nullopt;
	return TranscodeInfo{ "audio/mpeg", "mp3", max_bitrate };
	}

// The artist to report for a song: the file's own tag when it is a different
// claim from the folder it sits in, the folder's artist otherwise.
//
// This is the only place both facts are in hand -- a query has just one of
// them, and a client cannot normalise without a second copy of artist_key()'s
// rule (artistmatch.hh) in every language it is written in.  Falling back to
// the folder rather than to nothing also keeps an untagged file from reporting
// no artist at all, which is what a client sees from every other server in
// that case.
static std::string artist_of(const MediaStore::ChildEntry& c)
	{
	if (c.track_artist.empty()) return c.artist;
	std::string tag_key = artist_key(c.track_artist);
	std::string dir_key = artist_key(c.artist);
	// A name made entirely of punctuation keys as nothing; comparing two empty
	// keys would call unrelated artists equal, so fall back to the raw strings.
	if (tag_key.empty() || dir_key.empty())
		return c.track_artist == c.artist ? c.artist : c.track_artist;
	return tag_key == dir_key ? c.artist : c.track_artist;
	}

// Serialises a song ChildEntry into a JSON object.  When max_bitrate causes a
// transcode, also emits transcodedContentType / transcodedSuffix (standard
// Subsonic) and transcodedBitRate (gaindrive extension; ignored by clients
// that don't know it) so the client knows the actual stream format.
static nlohmann::json song_entry_json(const MediaStore::ChildEntry& c,
                                       int max_bitrate = 0,
                                       const std::string& format = "")
	{
	// Video rows live in the same table and come back through the same
	// queries; the extension is what distinguishes them, so codecs.hh answers
	// this without a dedicated column travelling through every query.
	bool is_video = is_video_ext(c.codec);
	std::string track_artist = artist_of(c);
	nlohmann::json s = {
		{"id",          sid(c.id)},
		{"parent",      sid(c.parent_id)},
		// The album folder is the song's album in ID3 terms; clients asking
		// for tag-based data expect albumId rather than parent.
		{"albumId",     sid(c.parent_id)},
		{"isDir",       false},
		{"type",        is_video ? "video" : "music"},
		{"isVideo",     is_video},
		{"title",       c.title},
		// The track's own artist, as every other Subsonic server reports it;
		// see artist_of().  displayArtist repeats it and displayAlbumArtist
		// carries the folder-derived artist, which is what makes
		// `artist != displayAlbumArtist` a client's whole test for "this track
		// is not by the album's artist".
		//
		// Both OpenSubsonic fields are sent unconditionally, empty included:
		// the spec's rule is that a server supporting an optional field must
		// always return it, so a client can tell "these agree" from "this
		// server does not know about the field".
		{"artist",            track_artist},
		{"displayArtist",     track_artist},
		{"displayAlbumArtist", c.artist},
		{"album",       c.album},
		{"track",       c.track_number},
		{"discNumber",  c.disc_number},
		{"year",        c.year},
		{"genre",       c.genre},
		{"size",        c.file_size},
		{"contentType", std::string(codec_to_mime(c.codec))},
		{"suffix",      c.codec},
		{"duration",    (int)c.duration},
		{"bitRate",     c.bitrate}
		};
	if (c.cover_art_id >= 0) s["coverArt"] = sid(c.cover_art_id);
	if (!c.starred.empty()) s["starred"] = iso8601(c.starred);
	// gaindrive extension. discNumber already carries this number, and every
	// Subsonic client groups by that; this says the grouping is a *season*, so
	// a client that knows about it can head the group "Series 2" rather than
	// "Disc 2". Omitted rather than sent as 0, so its absence means "not an
	// episode, or an entry from a query that does not select it".
	if (c.season > 0) s["season"] = c.season;
	// Omitted rather than sent as 0 when the scan could not probe the file, or
	// when the query that produced this entry does not select the dimensions.
	if (c.width  > 0) s["originalWidth"]  = c.width;
	if (c.height > 0) s["originalHeight"] = c.height;
	if (auto t = transcode_target(c, max_bitrate, format)) {
		s["transcodedContentType"] = std::string(t->mime);
		s["transcodedSuffix"]      = std::string(t->suffix);
		// Video has no meaningful single bitrate to promise — the encode is
		// CRF-driven — so the field is omitted rather than sent as 0.
		if (t->bitrate > 0) s["transcodedBitRate"] = t->bitrate;
		}
	// gaindrive extension.  Says whether the stream this entry would produce
	// carries a Content-Length and answers Range requests, so the client can
	// let the media element seek by itself instead of re-requesting with
	// timeOffset.  transcodedSuffix cannot answer this: it is present for both
	// the remux and re-encode tiers, and those differ precisely here.
	if (is_video) s["nativeSeek"] = video_seeks_natively(c.video_codec,
	                                                     c.audio_codec);
	return s;
	}

// Creates an XML element for a song with the given tag name.
static XMLElement* song_entry_xml(XMLDocument& doc,
                                   const MediaStore::ChildEntry& c,
                                   const char* tag,
                                   int max_bitrate = 0,
                                   const std::string& format = "")
	{
	bool  is_video = is_video_ext(c.codec);
	auto* el = doc.NewElement(tag);
	el->SetAttribute("id",          c.id);
	el->SetAttribute("parent",      c.parent_id);
	el->SetAttribute("isDir",       false);
	el->SetAttribute("title",       c.title.c_str());
	// See the JSON entry: the track's own artist, with the folder-derived one
	// beside it, both OpenSubsonic fields always present.
	std::string track_artist = artist_of(c);
	el->SetAttribute("artist",            track_artist.c_str());
	el->SetAttribute("displayArtist",     track_artist.c_str());
	el->SetAttribute("displayAlbumArtist", c.artist.c_str());
	el->SetAttribute("album",       c.album.c_str());
	if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
	el->SetAttribute("track",       c.track_number);
	el->SetAttribute("discNumber",  c.disc_number);
	el->SetAttribute("year",        c.year);
	el->SetAttribute("genre",       c.genre.c_str());
	el->SetAttribute("size",        (int64_t)c.file_size);
	el->SetAttribute("contentType", std::string(codec_to_mime(c.codec)).c_str());
	el->SetAttribute("suffix",      c.codec.c_str());
	el->SetAttribute("duration",    (int)c.duration);
	el->SetAttribute("bitRate",     c.bitrate);
	el->SetAttribute("type",        is_video ? "video" : "music");
	el->SetAttribute("isVideo",     is_video);
	el->SetAttribute("albumId",     c.parent_id);
	if (c.width  > 0) el->SetAttribute("originalWidth",  c.width);
	if (c.height > 0) el->SetAttribute("originalHeight", c.height);
	// See the JSON entry: a gaindrive extension marking discNumber as a season.
	if (c.season > 0) el->SetAttribute("season", c.season);
	if (!c.starred.empty())
		el->SetAttribute("starred", iso8601(c.starred).c_str());
	if (auto t = transcode_target(c, max_bitrate, format)) {
		el->SetAttribute("transcodedContentType", std::string(t->mime).c_str());
		el->SetAttribute("transcodedSuffix",      std::string(t->suffix).c_str());
		if (t->bitrate > 0)
			el->SetAttribute("transcodedBitRate", t->bitrate);
		}
	if (is_video)
		el->SetAttribute("nativeSeek",
		                 video_seeks_natively(c.video_codec, c.audio_codec));
	return el;
	}

// Bridges MediaStore's song record to the streamer's.  Written once because
// the video fields are easy to forget in an aggregate initialiser — and a
// dropped is_video sends a video down the audio ladder, where TARGETS has no
// entry for its container and the whole tier decision is skipped.
// size_override exists for the Cast probe, which deliberately serves a slice.
// A comma-joined set, because there are only ever a handful of codec pairs and
// a table would be a schema change for a fact that is pure cache: worst case a
// forgotten refusal costs one failed LOAD again.  Neither codec name can
// contain a comma, so the join needs no escaping.
static const char* CAST_NOVIDEO_KEY = "cast_novideo:";

static Streamer::SongInfo streamer_song(const MediaStore::SongInfo& s,
                                         const std::string& abs,
                                         int64_t size_override = 0)
	{
	Streamer::SongInfo si{ abs, s.codec, s.bitrate, s.duration,
	                       size_override > 0 ? size_override : s.file_size,
	                       s.id, s.file_modified };
	si.is_video    = s.is_video;
	si.width       = s.width;
	si.height      = s.height;
	si.video_codec = s.video_codec;
	si.audio_codec = s.audio_codec;
	return si;
	}

// Looks up the authenticated user's max_bitrate so song entries can advertise
// the transcoded* fields.  Must be called only after check_auth has succeeded.
// Returns 0 (= unlimited / no transcode) if the user record can't be read.
static int request_max_bitrate(const httplib::Request& req, MediaStore& store)
	{
	auto it = req.params.find("u");
	if (it == req.params.end()) return 0;
	auto u = store.get_user(it->second);
	return u ? u->max_bitrate : 0;
	}

// Builds the full subsonic response body for a playlist with its songs.
static std::string playlist_body(const MediaStore::PlaylistInfo& pl, bool use_json,
                                  int max_bitrate)
	{
	if (use_json)
		return subsonic_ok_json([&pl, max_bitrate](nlohmann::json& r) {
			nlohmann::json entries = nlohmann::json::array();
			for (auto& c : pl.songs)
				entries.push_back(song_entry_json(c, max_bitrate));
			r["playlist"] = {
				{"id",        sid(pl.id)},
				{"name",      pl.name},
				{"comment",   pl.comment},
				{"owner",     pl.owner},
				{"public",    pl.is_public},
				{"songCount", pl.song_count},
				{"duration",  pl.duration},
				{"created",   iso8601(pl.created)},
				{"changed",   iso8601(pl.updated)},
				{"entry",     entries}
				};
			});
	return subsonic_ok([&pl, max_bitrate](XMLDocument& doc, XMLElement* root) {
		auto* playlist = doc.NewElement("playlist");
		playlist->SetAttribute("id",        pl.id);
		playlist->SetAttribute("name",      pl.name.c_str());
		playlist->SetAttribute("comment",   pl.comment.c_str());
		playlist->SetAttribute("owner",     pl.owner.c_str());
		playlist->SetAttribute("public",    pl.is_public);
		playlist->SetAttribute("songCount", pl.song_count);
		playlist->SetAttribute("duration",  pl.duration);
		playlist->SetAttribute("created",   iso8601(pl.created).c_str());
		playlist->SetAttribute("changed",   iso8601(pl.updated).c_str());
		for (auto& c : pl.songs)
			playlist->InsertEndChild(song_entry_xml(doc, c, "entry", max_bitrate));
		root->InsertEndChild(playlist);
		});
	}

// A frame size as the API spells it, "WxH", or empty for anything else.
//
// This is validation rather than parsing, and it is load-bearing: `size`
// reaches Streamer::video_ffmpeg_argv(), which splices it either side of the
// `x` into an ffmpeg **filtergraph** — `scale=<W>:<H>:force_original_...`. A
// filtergraph is its own language, it is not the shell but it is not inert
// either, and `-vf` accepts source filters: `movie=` and `subtitles=` both
// name a file to read, the latter rendering a text file straight into the
// frames. The trailing option can be absorbed by ending an injected chain with
// another scale, so there is no accidental protection in the concatenation.
//
// Everything else on that path was already safe — maxBitRate, timeOffset and
// duration go through to_int() and come back out through std::to_string, and
// `format` is whitelisted by target_for(). This was the one that was not.
static std::string sane_video_size(const std::string& s)
	{
	auto x = s.find('x');
	if (x == std::string::npos || x == 0 || x + 1 == s.size()) return "";
	if (s.size() > 11) return "";
	for (size_t i = 0; i < s.size(); ++i)
		if (i != x && !std::isdigit(static_cast<unsigned char>(s[i]))) return "";
	const int w = to_int(s.substr(0, x), 0);
	const int h = to_int(s.substr(x + 1), 0);
	if (w < 16 || h < 16 || w > 7680 || h > 4320) return "";
	return std::to_string(w) + "x" + std::to_string(h);
	}

// The containers a client declared it demuxes for itself, from the comma list
// stream.view spells `playableContainers`.
//
// Validated here rather than in Streamer for the reason sane_video_size() above
// gives: the one caller reachable from outside is the one that checks. What is
// bounded is the whole parameter, at 64 characters — that caps the token count
// and every token length at once, so there are no separate counters to keep
// agreeing with each other.
//
// An unrecognised token is **dropped, not refused**, matching `size`. A client
// naming a container this server has never heard of is asking for nothing, not
// asking wrongly, and a hard error would make adding a container to a client a
// breaking change against every older server.
//
// Everything that survives is a VIDEO_TARGETS name, which is what lets
// serve_video() log the set verbatim.
static ClientContainers parse_client_containers(const std::string& s)
	{
	ClientContainers out;
	if (s.empty() || s.size() > 64) return out;
	size_t start = 0;
	while (start <= s.size()) {
		size_t      comma = s.find(',', start);
		std::string tok   = s.substr(start, comma == std::string::npos
		                                    ? std::string::npos
		                                    : comma - start);
		// songs.codec holds a lowercased extension, so that is what a
		// declaration has to be compared against.
		for (char& c : tok)
			c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
		if (container_declarable(tok)) out.insert(std::move(tok));
		if (comma == std::string::npos) break;
		start = comma + 1;
		}
	return out;
	}

// Where one of getVideoInfo's caption tracks lives, as the API reports it:
// "sidecar" for the subtitle file beside the video, "container" for a stream
// inside it.  The same two words getChapters already uses for the same
// distinction.
//
// It exists because the answer stopped being the server's business alone. A
// client that demuxes the container itself (see playableContainers on
// stream.view) is handed those streams by its own demuxer, so side-loading
// them through getCaptions as well would list every subtitle twice — while the
// sidecar, which no container carries, still has to come from here.
//
// A pure derivation: SIDECAR_CAPTION_INDEX is negative precisely so it can
// never be a stream index, which is the fact this reads.
static const char* caption_source(const MediaStore::CaptionTrack& c)
	{
	return c.index < 0 ? "sidecar" : "container";
	}

static std::string url_encode(const std::string& s)
	{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
		}
	return out;
	}

// A scaled cover is buffered and sent with set_content(), never with the
// no-length content provider.  That is framing, not an optimisation: the
// no-length overload emits neither Content-Length nor Transfer-Encoding, and
// httplib only sends Connection: close when the connection is closing for
// unrelated reasons.  The response then goes out on a keep-alive connection
// with nothing marking where the body ends, the client reads on into the
// following response, and every image after the first on that connection is
// the previous one's — which is what "all the thumbnails are wrong, and
// reloading doesn't help" looks like.  CLAUDE.md records the same hazard for
// serve_transcoded; this call site cost a long investigation before it got it.
//
// The scaling itself now happens in process (see imagescale.hh) and its result
// is cached (see coverart.hh), so this file no longer runs ffmpeg for images;
// CoverArtCache keeps a fork as a last resort for what stb cannot decode.

// ---- Login throttle ---------------------------------------------------

// Failed authentications per client address, with a delay that grows and then
// a block.
//
// Every credential this server accepts rides in a query string and is checked
// by one string comparison, so without this `ping.view` is an unmetered
// password oracle — and `Access-Control-Allow-Origin: *` means any web page
// can drive it. The pre-routing comment reasons the CORS choice through and
// its conclusion holds on a LAN; a public address is what changes it.
//
// Two things about the shape:
//
// * **It keys on client_addr(), which now means remote_addr unless a trusted
//   proxy said otherwise.** Keying on a header any caller can set would let an
//   attacker pick a fresh bucket per request, which is worse than no throttle
//   because it looks like one.
// * **The map is capped and swept.** The key is attacker-influenced, so an
//   unbounded map is itself the memory-exhaustion bug this file is fixing
//   elsewhere. `last_access_seen_` in MediaStore gets away with no cap because
//   it only ever inserts on *success*; this one cannot.
namespace {

// Tuned against a client storm rather than against an attacker, because the
// attacker is the easy case. **One stale credential is not one failed
// request**: a client whose saved password has gone bad opens an album grid
// and fires a hundred cover-art requests in parallel, every one of which
// fails auth — so thresholds sized for a human typing a password wrong would
// lock a legitimate user out of their own server, from their own device,
// for no reason they could act on.
//
// Fifty failures then ten minutes still caps guessing at a few hundred
// attempts an hour, which is nothing against any password worth the name, and
// the delay below has already made the rate useless long before the block. A
// successful login clears the bucket outright, so the recovery for the stale
// client is the thing its user was going to do anyway.
constexpr int    THROTTLE_FREE_TRIES  = 10;     // before any delay
constexpr int    THROTTLE_BLOCK_AFTER = 50;     // failures before a hard block
constexpr auto   THROTTLE_BLOCK_FOR   = std::chrono::minutes(10);
constexpr auto   THROTTLE_FORGET      = std::chrono::minutes(30);
constexpr size_t THROTTLE_MAX_KEYS    = 4096;

struct AuthFailures {
	int                                   count = 0;
	std::chrono::steady_clock::time_point last;
	};

std::mutex                              throttle_mu;
std::map<std::string, AuthFailures>      throttle;

// Drops entries nothing has touched for a while. Called with the lock held,
// and only when the map is at its cap, so the common path costs nothing.
void throttle_sweep(std::chrono::steady_clock::time_point now)
	{
	for (auto it = throttle.begin(); it != throttle.end(); )
		if (now - it->second.last > THROTTLE_FORGET) it = throttle.erase(it);
		else                                          ++it;
	// Still full after the sweep: this is an active spray from many addresses
	// rather than a stale map, so drop it wholesale rather than grow without
	// bound. Everyone gets their free tries back, which is the safe direction.
	if (throttle.size() >= THROTTLE_MAX_KEYS) throttle.clear();
	}

// What this caller has to pay before its credentials are looked at.
//
// **A block is refused, never slept through.** The delay and the block are
// deliberately different mechanisms, because this runs on an httplib worker
// thread and there are 32 of them: sleeping out a fifteen-minute block would
// let 32 requests from one blocked address pin the entire pool, which is a
// far better denial of service than the guessing it is meant to stop. The
// delay is capped for the same reason — long enough to make a guessing rate
// useless, short enough that holding a thread for it does not matter.
struct Penalty {
	std::chrono::milliseconds delay{0};
	bool                      blocked = false;
	};

Penalty throttle_penalty(const std::string& key)
	{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(throttle_mu);
	auto it = throttle.find(key);
	if (it == throttle.end()) return {};
	if (now - it->second.last > THROTTLE_FORGET) {
		throttle.erase(it);
		return {};
		}
	const int n = it->second.count;
	if (n <= THROTTLE_FREE_TRIES) return {};
	if (n >= THROTTLE_BLOCK_AFTER) {
		// The block runs from the *last* attempt, so hammering it keeps it
		// shut rather than waiting it out while still trying.
		if (now - it->second.last < THROTTLE_BLOCK_FOR) return {{}, true};
		throttle.erase(it);
		return {};
		}
	// 200 ms, 400, 800 …, capped.
	const int steps = std::min(n - THROTTLE_FREE_TRIES, 4);
	return { std::chrono::milliseconds(100 << steps), false };
	}

void throttle_record_failure(const std::string& key)
	{
	const auto now = std::chrono::steady_clock::now();
	std::lock_guard<std::mutex> lock(throttle_mu);
	if (throttle.size() >= THROTTLE_MAX_KEYS && !throttle.count(key))
		throttle_sweep(now);
	auto& e = throttle[key];
	if (now - e.last > THROTTLE_FORGET) e.count = 0;
	e.count++;
	e.last = now;
	}

void throttle_record_success(const std::string& key)
	{
	std::lock_guard<std::mutex> lock(throttle_mu);
	throttle.erase(key);
	}

}  // namespace

// Extracts u/p/t/s params and validates auth. Writes error into res on failure.
static bool check_auth(const httplib::Request& req, httplib::Response& res,
                       MediaStore& store)
	{
	auto qp = [&](const std::string& k) -> std::string {
		auto it = req.params.find(k);
		return it != req.params.end() ? it->second : "";
		};

	std::string u  = qp("u");
	std::string pw = qp("p");
	std::string t  = qp("t");
	std::string s  = qp("s");

	bool use_json = (fmt_of(req) == "json");
	auto err = [&](int code, const char* msg) {
		if (use_json)
			res.set_content(subsonic_error_json(code, msg), "application/json");
		else
			res.set_content(subsonic_error(code, msg),      "application/xml");
		};

	if (u.empty() || (pw.empty() && (t.empty() || s.empty()))) {
		err(10, "Required parameter missing.");
		return false;
		}

	// Paid before the password is looked at, so a caller in the penalty box
	// cannot use the endpoint as an oracle at all, and so a wrong password
	// costs the same whether the account exists or not.
	const std::string throttle_key = client_addr(req);
	{
	const auto pen = throttle_penalty(throttle_key);
	if (pen.blocked) {
		// Answered as an ordinary wrong password: a blocked caller learning
		// that it is blocked learns the throttle's shape, and a client whose
		// user really did mistype has the same thing to do either way.
		err(40, "Wrong username or password.");
		return false;
		}
	if (pen.delay.count() > 0) std::this_thread::sleep_for(pen.delay);
	}

	if (!store.validate_auth(u, pw, t, s)) {
		throttle_record_failure(throttle_key);
		// A distinct, greppable line — the response is a 200 carrying a
		// Subsonic failure envelope, as the spec requires, so this log line is
		// the only thing a host-level blocker can see. The username is
		// included and the credential deliberately is not.
		std::cout << stamp(throttle_key) << "auth failed for user "
		          << log_safe(u, 64) << std::endl;
		err(40, "Wrong username or password.");
		return false;
		}

	throttle_record_success(throttle_key);
	return true;
	}
// Returns true if the authenticated user has cast permission.
static bool check_cast_perm(const httplib::Request& req, httplib::Response& res,
                             MediaStore& store, bool use_json)
	{
	auto u    = req.get_param_value("u");
	auto info = store.get_user(u);
	if (!info || !info->cast_allowed) {
		const char* msg = "User is not authorized for the given operation.";
		res.set_content(use_json ? subsonic_error_json(50, msg)
		                        : subsonic_error(50, msg),
		                use_json ? "application/json" : "text/xml");
		return false;
		}
	return true;
	}

// Returns true if the authenticated user may write into their personal uploads
// folder. Admins may regardless — the same rule /upload has applied inline
// since it was written, lifted out here because four more endpoints now need it
// and a permission check with five copies is a permission check with four
// chances of being forgotten.
static bool check_upload_perm(const httplib::Request& req,
                               httplib::Response& res, MediaStore& store,
                               bool use_json)
	{
	auto info = store.get_user(req.get_param_value("u"));
	if (!info || (!info->upload_allowed && !info->is_admin)) {
		const char* msg = "User is not authorized for the given operation.";
		res.set_content(use_json ? subsonic_error_json(50, msg)
		                        : subsonic_error(50, msg),
		                use_json ? "application/json" : "application/xml");
		return false;
		}
	return true;
	}

// Returns true if the authenticated user may *modify* the library item stored
// at `rel_path`. Admin may modify anything; an upload user may modify what is
// inside their own batch, which is the same predicate moveAlbum applies at its
// permission block and exists for the same reason — somebody who has just
// uploaded has to be able to fix the names. Everything else lives in a shared
// root, so changing it is admin's alone.
//
// It takes the stored path rather than an id because every caller has already
// resolved the id, and because the ownership fact *is* the path: the second
// component of an uploads path is the owner's username, which is the same
// encoding deleteUpload checks.
//
// Note the depth rule is deliberately weaker than deleteUpload's exact five
// components. There the shape is the boundary — without it the endpoint is
// "delete any folder by guessing an integer". Here the item already exists and
// has been resolved from an id, so ownership alone is the question, and a
// stricter depth would refuse a cover on the user's own artist folder.
// Split from the reporting wrapper below because getChapters answers the same
// question as a *field* -- whether to offer the client a Save button -- and a
// second copy of the admin-or-own-uploads rule is exactly what that wrapper
// exists to prevent.
static bool item_write_allowed(const httplib::Request& req, MediaStore& store,
                                const std::string& uploads_root_name,
                                const std::string& rel_path)
	{
	const std::string uname = req.get_param_value("u");
	auto info = store.get_user(uname);
	if (info && info->is_admin) return true;

	std::vector<std::string> parts;
	for (const auto& c : std::filesystem::path(rel_path))
		parts.push_back(c.string());

	return info && info->upload_allowed
	    && !uploads_root_name.empty()
	    && parts.size() >= 2
	    && parts[0] == uploads_root_name
	    && !uname.empty() && parts[1] == uname;
	}

static bool check_item_write_perm(const httplib::Request& req,
                                   httplib::Response& res, MediaStore& store,
                                   const std::string& uploads_root_name,
                                   const std::string& rel_path, bool use_json)
	{
	if (item_write_allowed(req, store, uploads_root_name, rel_path)) return true;

	const char* msg = "Modifying the shared library requires admin role.";
	res.set_content(use_json ? subsonic_error_json(50, msg)
	                        : subsonic_error(50, msg),
	                use_json ? "application/json" : "application/xml");
	return false;
	}

// Returns true if the authenticated user may *read* the item stored at
// `rel_path`. Everything in a library root is shared and readable; the uploads
// root is personal, so only its owner and an admin may reach it.
//
// This exists because the uploads root is kept out of *browsing* — see
// not_uploads() and the skips in get_music_folders()/music_folder_by_id() —
// but every id-addressed read went straight to `WHERE id = ?`. So the listing
// hid another user's batch while stream, download, getCoverArt and
// getMusicDirectory all served it to anyone who tried the number. Filtering
// here rather than in each query keeps the eleven ChildEntry queries untouched,
// which is the same trade getVideos made for is_video.
static bool item_read_allowed(const httplib::Request& req, MediaStore& store,
                              const std::string& uploads_root_name,
                              const std::string& rel_path)
	{
	if (uploads_root_name.empty() || rel_path.empty()) return true;

	std::vector<std::string> parts;
	for (const auto& c : std::filesystem::path(rel_path))
		parts.push_back(c.string());
	if (parts.empty() || parts[0] != uploads_root_name) return true;

	const std::string uname = req.get_param_value("u");
	auto info = store.get_user(uname);
	if (info && info->is_admin) return true;
	return parts.size() >= 2 && !uname.empty() && parts[1] == uname;
	}

static bool check_item_read_perm(const httplib::Request& req,
                                  httplib::Response& res, MediaStore& store,
                                  const std::string& uploads_root_name,
                                  const std::string& rel_path, bool use_json)
	{
	if (item_read_allowed(req, store, uploads_root_name, rel_path)) return true;

	// Deliberately the same shape of answer a missing item gets, so this does
	// not become an oracle for which ids exist in somebody else's uploads.
	const char* msg = "Not found.";
	res.set_content(use_json ? subsonic_error_json(70, msg)
	                        : subsonic_error(70, msg),
	                use_json ? "application/json" : "application/xml");
	return false;
	}

// ---- Outbound fetch guard ---------------------------------------------

// True if `addr` is one an outbound fetch may go to: a globally routable
// unicast address and nothing else.
//
// Anything else is a request the *server* can make and the caller cannot, and
// that difference is the whole of an SSRF: loopback reaches admin interfaces
// bound to 127.0.0.1, link-local reaches 169.254.169.254, and RFC1918 reaches
// every other machine on the LAN. The unspecified and multicast cases are here
// because 0.0.0.0 is another spelling of loopback on Linux and a multicast
// destination is never a web server.
static bool addr_is_global(const struct sockaddr* addr)
	{
	if (addr->sa_family == AF_INET) {
		const uint32_t a = ntohl(
			reinterpret_cast<const struct sockaddr_in*>(addr)->sin_addr.s_addr);
		const uint8_t b0 = (a >> 24) & 0xff, b1 = (a >> 16) & 0xff;
		if (b0 == 0)                        return false;  // 0.0.0.0/8
		if (b0 == 10)                       return false;  // 10/8
		if (b0 == 127)                      return false;  // loopback
		if (b0 == 169 && b1 == 254)         return false;  // link-local
		if (b0 == 172 && (b1 & 0xf0) == 16) return false;  // 172.16/12
		if (b0 == 192 && b1 == 168)         return false;  // 192.168/16
		if (b0 == 100 && (b1 & 0xc0) == 64) return false;  // CGNAT 100.64/10
		if (b0 >= 224)                      return false;  // multicast, reserved
		return true;
		}
	if (addr->sa_family == AF_INET6) {
		const auto& s6 = reinterpret_cast<const struct sockaddr_in6*>(addr)->sin6_addr;
		if (IN6_IS_ADDR_LOOPBACK(&s6) || IN6_IS_ADDR_UNSPECIFIED(&s6)
		    || IN6_IS_ADDR_LINKLOCAL(&s6) || IN6_IS_ADDR_SITELOCAL(&s6)
		    || IN6_IS_ADDR_MULTICAST(&s6))
			return false;
		// Unique local (fc00::/7), and IPv4-mapped, which would otherwise be a
		// straight bypass of every rule above. V4COMPAT is the deprecated
		// spelling of the same idea (::1.2.3.4, RFC 4291 §2.5.5.1) — modern
		// stacks refuse to route it, but "refuse" is their promise, not
		// ours, and the check costs one macro beside the one that matters.
		if ((s6.s6_addr[0] & 0xfe) == 0xfc) return false;
		if (IN6_IS_ADDR_V4MAPPED(&s6) || IN6_IS_ADDR_V4COMPAT(&s6)) {
			struct sockaddr_in v4{};
			v4.sin_family = AF_INET;
			std::memcpy(&v4.sin_addr.s_addr, s6.s6_addr + 12, 4);
			return addr_is_global(reinterpret_cast<struct sockaddr*>(&v4));
			}
		return true;
		}
	return false;
	}

// True if every address `host` resolves to is globally routable. Every one,
// not any: a name that resolves to both a public address and 127.0.0.1 is a
// standard way of getting a checked fetch to connect somewhere else.
//
// This is not a complete defence — the resolution here and the one httplib
// performs when it connects are two separate lookups, so a name whose answer
// changes between them is still a hole (DNS rebinding). Closing that needs the
// connection to be made to an address we resolved ourselves, which httplib's
// client does not offer. It is recorded rather than fixed because the
// remaining hole needs an attacker-controlled nameserver, while what it
// replaced needed only a typed URL.
static bool host_is_global(const std::string& host)
	{
	// Strip a bracketed IPv6 literal and any :port.
	std::string h = host;
	if (!h.empty() && h.front() == '[') {
		auto close = h.find(']');
		if (close == std::string::npos) return false;
		h = h.substr(1, close - 1);
		}
	else {
		auto colon = h.find(':');
		if (colon != std::string::npos) h = h.substr(0, colon);
		}
	if (h.empty()) return false;

	struct addrinfo hints{};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* result = nullptr;
	if (getaddrinfo(h.c_str(), nullptr, &hints, &result) != 0 || !result)
		return false;

	bool all_global = true;
	for (auto* ai = result; ai; ai = ai->ai_next)
		if (!addr_is_global(ai->ai_addr)) {
			all_global = false;
			break;
			}
	freeaddrinfo(result);
	return all_global;
	}

// The authority part of a fetched URL, split for the client constructors.
// SSLClient's single-string constructor does not parse a `:port` — the colon
// and digits go into the TLS SNI and the connect fails — so the split has to
// happen here, for both clients, or an https URL with an explicit port
// silently fetches nothing.
static std::pair<std::string, int> split_host_port(const std::string& host,
                                                   int default_port)
	{
	if (!host.empty() && host.front() == '[') {
		auto close = host.find(']');
		if (close == std::string::npos) return { host, default_port };
		std::string h = host.substr(1, close - 1);
		if (close + 1 < host.size() && host[close + 1] == ':')
			return { h, to_int(host.substr(close + 2), default_port) };
		return { h, default_port };
		}
	auto colon = host.find(':');
	if (colon == std::string::npos) return { host, default_port };
	return { host.substr(0, colon),
	         to_int(host.substr(colon + 1), default_port) };
	}

// ---- Artist info helper -----------------------------------------------

// Performs the MusicBrainz -> Wikidata -> Wikipedia -> TheAudioDB -> Discogs
// lookup for one artist and caches the result. Runs on the portrait resolver's
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
// that rule rather than an omission. Its one caller is portrait_worker(), which
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
// nothing to arrange: portrait_worker() already calls resolve_artist_info() and
// that function already writes artist_info_cache, so the biography this handler
// wants is a side effect the resolver was producing all along.
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

	MediaStore::CachedArtistInfo info;
	bool resolving = false;

	// A level-1 folder of a categories root is a section — Film, Series,
	// Documentary — not a performer. This test used to live inside
	// resolve_artist_info(); it has to be made here now, or a section would be
	// pushed onto the resolver queue to have MusicBrainz asked about "Film".
	// Nothing is resolving and nothing ever will be, so say so: a client that
	// polled on `resolving` would poll for ever.
	if (!store_.is_category_folder(id)) {
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
		         || portrait_pending(path);

		if (resolving) {
			// Front of the queue: what somebody is looking at beats the
			// alphabet. Already queued or in flight is a no-op, so a client
			// polling every few seconds costs nothing.
			portrait_request_front(id, path, name);
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

// ---- Album info helper -----------------------------------------------

// Shared implementation for getAlbumInfo and getAlbumInfo2.
// key is "albumInfo" or "albumInfo2" — controls the XML element / JSON key.
// Searches MusicBrainz for the release-group, then resolves a Wikipedia
// article via Wikidata if needed. Results are cached in album_info_cache.
static void handle_album_info(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store,
                               const std::string& uploads_root_name,
                               const char* key)
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
	auto album_data = store.get_album(id);
	if (!album_data) { err(70, "Album not found."); return; }
	// Same rule as handle_artist_info: an id in somebody else's uploads is
	// answered like a missing one, before its title or artist reaches a log
	// line or a MusicBrainz query.
	if (!item_read_allowed(req, store, uploads_root_name,
	                       store.get_folder_path(id))) {
		err(70, "Album not found."); return;
		}

	const std::string& title  = album_data->album.title;
	const std::string& artist = album_data->album.artist;

	auto cached = store.get_cached_album_info(id);
	MediaStore::CachedAlbumInfo info;
	if (cached) {
		info = *cached;
		std::cout << stamp() << "getAlbumInfo [" << title << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}
	else {
		std::cout << stamp() << "getAlbumInfo [" << title
		          << "] querying MusicBrainz" << std::endl;
		httplib::SSLClient mb("musicbrainz.org");
		mb.set_default_headers({
			{"User-Agent", USER_AGENT}
			});

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
		if (!info.mbid.empty())
			std::cout << stamp() << "getAlbumInfo [" << title
			          << "] release-group id from the files' tags, skipping"
			             " search" << std::endl;

		if (info.mbid.empty()) {
			httplib::Params p1{
				{"query", "releasegroup:\"" + title + "\" AND artist:\"" + artist + "\""},
				{"limit", "1"},
				{"fmt",   "json"}
				};
			auto r1 = mb_get(mb, "/ws/2/release-group", p1,
			                 "getAlbumInfo [" + title + "]");
			if (!r1) {
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] MusicBrainz request failed (no response)" << std::endl;
				}
			else if (r1->status != 200) {
				std::cout << stamp() << "getAlbumInfo [" << title
				          << "] MusicBrainz HTTP " << r1->status
				          << (mb_rate_limited(r1) ? " - rate limited" : "")
				          << std::endl;
				}
			else {
				auto j1 = nlohmann::json::parse(r1->body, nullptr, false);
				// Same reasoning as the artist search: this is concatenated
				// into "/ws/2/release-group/" below.
				std::string rg = jstr(jidx(jsub(j1, "release-groups"), 0), "id");
				if (!rg.empty() && !is_uuid(rg)) {
					std::cout << stamp() << "getAlbumInfo [" << title
					          << "] ignoring a release-group id that is not a UUID"
					          << std::endl;
					rg.clear();
					}
				info.mbid = rg;
				}
			}

		// Step 2 — fetch URL relations for the release-group.
		if (!info.mbid.empty()) {
			// The one-second wait that used to sit here is mb_get()'s job now;
			// doing it in both places only made every lookup a second slower.
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
				if (!wiki_title.empty()) {
					httplib::SSLClient wp("en.wikipedia.org");
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

		store.cache_album_info(id, info);
		std::cout << stamp() << "getAlbumInfo [" << title << "] cached"
		          << " mbid=" << (info.mbid.empty() ? "(none)" : info.mbid)
		          << std::endl;
		}

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&info, key](nlohmann::json& r) {
			nlohmann::json ai = nlohmann::json::object();
			if (!info.mbid.empty())           ai["musicBrainzId"] = info.mbid;
			if (!info.notes.empty())          ai["notes"]         = info.notes;
			if (!info.wiki_url.empty())       ai["wikiUrl"]       = info.wiki_url;
			if (!info.allmusic_url.empty())   ai["allMusicUrl"]   = info.allmusic_url;
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
		body = subsonic_ok([&info, &add_text_el, key](XMLDocument& doc, XMLElement* root) {
			auto* ai = doc.NewElement(key);
			add_text_el(doc, ai, "musicBrainzId", info.mbid);
			add_text_el(doc, ai, "notes",         info.notes);
			add_text_el(doc, ai, "wikiUrl",       info.wiki_url);
			add_text_el(doc, ai, "allMusicUrl",   info.allmusic_url);
			root->InsertEndChild(ai);
			});
		}
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- Album list helper -----------------------------------------------

// Shared implementation for getAlbumList and getAlbumList2.
// key is "albumList" or "albumList2".
static void handle_album_list(const httplib::Request& req, httplib::Response& res,
                               MediaStore& store, const char* key)
	{
	bool use_json = (fmt_of(req) == "json");

	auto param_int = [&](const char* name, int def) {
		auto it = req.params.find(name);
		return it != req.params.end() ? to_int(it->second, def) : def;
		};
	auto param_str = [&](const char* name) {
		auto it = req.params.find(name);
		return it != req.params.end() ? it->second : std::string{};
		};

	std::string type     = param_str("type");
	if (type.empty()) type = "newest";
	int size             = std::min(500, std::max(1, param_int("size", 10)));
	int offset           = std::clamp(param_int("offset", 0), 0, MAX_LIST_OFFSET);
	int from_year        = param_int("fromYear", 0);
	int to_year          = param_int("toYear",   0);
	std::string genre    = param_str("genre");
	std::string username = param_str("u");
	bool personal        = param_str("personal") == "true";
	std::string pu       = personal ? username : "";

	auto albums = store.get_album_list(type, size, offset,
	                                   from_year, to_year, genre, username, pu);

	std::string body;
	if (use_json)
		body = subsonic_ok_json([&albums, key](nlohmann::json& r) {
			nlohmann::json arr = nlohmann::json::array();
			for (auto& al : albums) {
				nlohmann::json entry = {
					{"id",        sid(al.id)},
					{"parent",    sid(al.parent_id)},
					{"artistId",  sid(al.parent_id)},
					{"isDir",     true},
					{"title",     al.title},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"videoCount", al.video_count}
					};
				if (!al.created.empty()) entry["created"] = iso8601(al.created);
				if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
				arr.push_back(std::move(entry));
				}
			r[key] = {{"album", arr}};
			});
	else
		body = subsonic_ok([&albums, key](XMLDocument& doc, XMLElement* root) {
			auto* list = doc.NewElement(key);
			for (auto& al : albums) {
				auto* el = doc.NewElement("album");
				el->SetAttribute("id",        al.id);
				el->SetAttribute("parent",    al.parent_id);
				el->SetAttribute("isDir",     true);
				el->SetAttribute("title",     al.title.c_str());
				el->SetAttribute("name",      al.title.c_str());
				el->SetAttribute("artist",    al.artist.c_str());
				if (al.cover_art_id >= 0)
					el->SetAttribute("coverArt", al.cover_art_id);
				el->SetAttribute("songCount", al.song_count);
				el->SetAttribute("duration",  al.duration);
				el->SetAttribute("videoCount", al.video_count);
				if (!al.created.empty()) el->SetAttribute("created", iso8601(al.created).c_str());
				if (al.year > 0)         el->SetAttribute("year",    al.year);
				if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
				if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
				list->InsertEndChild(el);
				}
			root->InsertEndChild(list);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// ---- GainDrive --------------------------------------------------------

// A v4 UUID, from the CSPRNG.
//
// It was mt19937_64 seeded from a single 32-bit random_device draw, which is
// trivially predictable. Neither of the two things it names is a capability
// today — an upload batch directory sits under a path already scoped by the
// authenticated username, and a fetch job id is looked up within the caller's
// own list — so this is not a fix for a live hole. It is that both are ids
// handed back to a client, which is exactly the shape of thing that later
// grows into a credential, and OpenSSL is already linked.
static std::string make_uuid()
   {
   uint64_t a = 0, b = 0;
   uint8_t  raw[16];
   if (RAND_bytes(raw, sizeof(raw)) != 1) {
      // The CSPRNG failing is not survivable by carrying on with whatever was
      // on the stack, which is what ignoring the return value amounts to.
      std::cout << stamp() << "RAND_bytes failed generating a UUID" << std::endl;
      throw std::runtime_error("no entropy available");
      }
   std::memcpy(&a, raw,     8);
   std::memcpy(&b, raw + 8, 8);
   // Set UUID v4 version and variant bits.
   a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
   b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
   std::ostringstream ss;
   ss << std::hex << std::setfill('0')
      << std::setw(8)  << (uint32_t)(a >> 32)        << '-'
      << std::setw(4)  << (uint32_t)((a >> 16) & 0xFFFF) << '-'
      << std::setw(4)  << (uint32_t)(a & 0xFFFF)     << '-'
      << std::setw(4)  << (uint32_t)(b >> 48)         << '-'
      << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
   return ss.str();
   }

// Extract a zip/tar/tar.gz/tgz archive from memory into dest_dir.
// Entry paths are sanitised: absolute components and ".." are stripped so
// no file can escape dest_dir. Returns the number of regular files written,
// or -1 if the archive could not be opened.
// libarchive's error string can embed an entry's own name, and an entry name
// is attacker bytes that may hold newlines — a forged log line, in the log a
// host-level blocker reads. NULL when there is no message.
static std::string archive_err(struct archive* a)
   {
   const char* e = archive_error_string(a);
   return log_safe(e ? e : "(none)");
   }

static int extract_archive_to_dir(const std::filesystem::path& archive_file,
                                   const std::filesystem::path& dest_dir)
   {
   namespace fs = std::filesystem;

   struct archive* a = archive_read_new();
   archive_read_support_format_all(a);
   archive_read_support_filter_all(a);

   struct archive* wd = archive_write_disk_new();
   // NOABSOLUTEPATHS is still not set, and for the original reason: it would
   // reject every entry, because we set an absolute destination path
   // ourselves.
   //
   // SECURE_SYMLINKS **is** now set, and the note that used to be here — that
   // path traversal is prevented entirely by the sanitisation loop below — was
   // true of an entry's *pathname* and false of a symlink's *target*, which
   // the loop never looked at. An archive holding
   //
   //     esc  -> /etc        (a symlink entry)
   //     esc/passwd          (an ordinary file entry)
   //
   // has both pathnames pass the loop unchanged, since neither contains "..",
   // and libarchive then follows the link it has just created. The objection
   // to the flag was that it refuses to extract through a host symlink that
   // the music root may contain; that does not apply here, because the
   // destination is a batch directory gaindrive created itself, under the
   // uploads root and never inside a library root.
   //
   // The filetype filter below makes the flag belt-and-braces rather than the
   // only defence, since a link that is never written cannot be followed.
   archive_write_disk_set_options(wd, ARCHIVE_EXTRACT_TIME
                                    | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                    | ARCHIVE_EXTRACT_SECURE_NODOTDOT);

   auto cleanup = [&]{
      archive_read_close(a);
      archive_read_free(a);
      archive_write_close(wd);
      archive_write_free(wd);
      };

   // From a file, not memory: the upload handler streams the body to disk
   // precisely so an archive is never held in RAM, and reading it back here
   // would undo that.
   int r = archive_read_open_filename(a, archive_file.c_str(), 64 * 1024);
   if (r != ARCHIVE_OK) {
      std::cout << stamp() << "extract: open failed (" << r << "): "
                << archive_err(a) << std::endl;
      cleanup();
      return -1;
      }

   int count = 0, skipped = 0, entries = 0;
   uint64_t written = 0;
   struct archive_entry* entry;
   int hr;
   while ((hr = archive_read_next_header(a, &entry)) == ARCHIVE_OK
          || hr == ARCHIVE_WARN) {
      if (hr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: header warn: "
                   << archive_err(a) << std::endl;

      const char* raw = archive_entry_pathname(entry);
      if (!raw) { skipped++; continue; }

      // Build a sanitised relative path by dropping any "..", ".", and "/" components.
      fs::path safe;
      for (const auto& part : fs::path(raw)) {
         auto s = part.string();
         if (s == ".." || s == "." || s == "/") continue;
         safe /= part;
         }
      if (safe.empty()) {
         std::cout << stamp() << "extract: skip (empty after sanitise): " << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }

      // Only ordinary files and the directories holding them. A symlink or a
      // hardlink entry names a *target*, which is a second path the
      // sanitisation above never sees — and following one is how an extraction
      // escapes a directory it cannot traverse out of. Nothing an uploader
      // legitimately sends is either.
      const auto ft = archive_entry_filetype(entry);
      if (ft != AE_IFREG && ft != AE_IFDIR) {
         std::cout << stamp() << "extract: skip (not a file or directory): "
                   << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }
      if (archive_entry_hardlink(entry) || archive_entry_symlink(entry)) {
         std::cout << stamp() << "extract: skip (link entry): " << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }
      // `entries` counts everything written, directories included — counting
      // only files let a zip of a few hundred kilobytes create millions of
      // directories, which is inode exhaustion on a filesystem other people's
      // music is also on, with the byte cap never tripping because a
      // directory carries no data.
      if (entries >= MAX_ARCHIVE_ENTRIES) {
         std::cout << stamp() << "extract: entry limit reached, stopping"
                   << std::endl;
         break;
         }
      entries++;

      fs::path target = dest_dir / safe;
      archive_entry_set_pathname(entry, target.c_str());

      // ARCHIVE_WARN (-20) means partial success; still write the data.
      int wr = archive_write_header(wd, entry);
      if (wr < ARCHIVE_WARN) {
         std::cout << stamp() << "extract: write_header failed (" << wr << ") for "
                   << target << ": " << archive_err(wd) << std::endl;
         skipped++;
         continue;
         }
      if (wr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: write_header warn for "
                   << target << ": " << archive_err(wd) << std::endl;

      // Bounded, because an archive's *compressed* size says nothing about
      // what it expands to: a few megabytes of zeros is gigabytes on disk, and
      // the uploads root is a real filesystem someone else's music is also on.
      const void* buf; size_t sz; la_int64_t off;
      bool truncated = false;
      while (archive_read_data_block(a, &buf, &sz, &off) == ARCHIVE_OK) {
         // The declared block offset counts against the budget as well as the
         // data: a sparse tar entry placing a few bytes at offset 2^50 makes
         // a file of that apparent length for almost nothing, and everything
         // downstream — songs.file_size, the transcode planner — believes the
         // fiction.
         if (written + sz > MAX_ARCHIVE_BYTES
             || off < 0
             || static_cast<uint64_t>(off) + sz > MAX_ARCHIVE_BYTES) {
            truncated = true; break;
            }
         archive_write_data_block(wd, buf, sz, off);
         written += sz;
         }
      if (truncated) {
         std::cout << stamp() << "extract: size limit reached at " << target
                   << ", stopping" << std::endl;
         archive_write_finish_entry(wd);
         skipped++;
         break;
         }
      if (archive_entry_filetype(entry) == AE_IFREG)
         count++;
      archive_write_finish_entry(wd);
      }

   if (hr != ARCHIVE_EOF)
      std::cout << stamp() << "extract: read_next_header stopped (r=" << hr << "): "
                << archive_err(a) << std::endl;

   std::cout << stamp() << "extract: done, files=" << count
             << " skipped=" << skipped << std::endl;
   cleanup();
   return count;
   }

// Make a string safe to use as a single directory name. Shared by the upload
// reorganiser, by moveAlbum and by the URL fetcher's name override, so a name
// typed by hand lands in exactly the directory the uploader would have created
// for the same tag.
//
// Dropping '/' is also the traversal guard: a name is one path component by
// construction, so no input can escape the folder it is being created in. Note
// ".." needs no case of its own: the leading-dot strip below annihilates it,
// along with "." and "...", and an empty result is refused by every caller that
// took a name from a person.
//
// Dropping the control characters is what keeps such a name out of two places
// it has no business reaching. A directory name is echoed into the log by the
// scanner and by the batch renamer, where an embedded newline forges a log
// line; and it is written into a tinyxml2 attribute by getFetchJobs, where a
// raw C0 byte is not well-formed XML and nothing escapes it. Neither mattered
// while every component came from yt-dlp or from TagLib; a typed one is the
// first arbitrary string here to become a path component.
// Case-insensitively, because a filesystem extension is not case-sensitive to
// a person: someone renaming a film to "The Third Man.MKV" has already typed
// the extension and must not be given a second one.
static bool ends_with_ci(const std::string& s, const std::string& suffix)
	{
	if (suffix.size() > s.size()) return false;
	return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
	                   [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a))
		     == std::tolower(static_cast<unsigned char>(b));
		});
	}

static std::string sanitise_component(const std::string& s)
   {
   std::string r;
   for (char c : s)
      if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f &&
          c != '/' && c != '\\' && c != ':' &&
          c != '*' && c != '?'  && c != '"' && c != '<' && c != '>')
         r += c;
   while (!r.empty() && (r.front() == ' ' || r.front() == '.'))
      r.erase(r.begin());
   while (!r.empty() && (r.back()  == ' ' || r.back()  == '.'))
      r.pop_back();
   return r;
   }

// Reorganise all audio files under batch_root into <artist>/<album>/ subdirs
// using embedded tag metadata. Non-audio siblings follow their audio files when
// all audio in a source dir maps to the same (artist, album) target. Empty
// directories left behind are pruned. Falls back to "Unknown Artist" /
// "Unknown Album" for files with no usable tags.
static void reorganise_by_tags(const std::filesystem::path& batch_root)
   {
   namespace fs = std::filesystem;

   static const std::set<std::string> AUDIO_EXT = {
      ".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"};
   auto is_audio = [](const fs::path& p) {
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      return AUDIO_EXT.count(ext) > 0;
      };
   // A tag that sanitises away to nothing still has to land somewhere, so this
   // path substitutes a name. moveAlbum deliberately does not — there a name
   // was typed, and silently filing it under "Unknown" would hide the mistake.
   auto sanitise = [](const std::string& s) -> std::string {
      std::string r = sanitise_component(s);
      return r.empty() ? "Unknown" : r;
      };

   // Collect audio files that are NOT already in an Artist/Album subdir
   // (depth >= 2 from batch_root). Those are left in place; only flat or
   // single-level files need reorganising.
   struct AudioFile { fs::path path; std::string artist; std::string album; };
   std::vector<AudioFile> audio_files;
   std::error_code ec;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec)) {
      if (!e.is_regular_file() || !is_audio(e.path())) continue;
      auto rel   = e.path().lexically_relative(batch_root);
      int  depth = (int)std::distance(rel.begin(), rel.end()) - 1; // -1 for filename
      if (depth >= 2) continue;  // already in Artist/Album structure
      std::string artist = "Unknown Artist", album = "Unknown Album";
      TagLib::FileRef ref(e.path().c_str());
      if (!ref.isNull() && ref.tag()) {
         auto a = ref.tag()->artist().to8Bit(true);
         auto b = ref.tag()->album().to8Bit(true);
         if (!a.empty()) artist = a;
         if (!b.empty()) album  = b;
         }
      audio_files.push_back({e.path(), sanitise(artist), sanitise(album)});
      }

   if (audio_files.empty()) return;

   // Move each audio file into batch_root/<artist>/<album>/.
   // Track which source directories contributed to which (artist, album) targets
   // so sibling non-audio files (covers, .m3u, etc.) can follow.
   std::map<fs::path, std::set<std::pair<std::string, std::string>>> dir_targets;
   for (auto& af : audio_files) {
      fs::path target = batch_root / af.artist / af.album;
      fs::create_directories(target, ec);
      fs::rename(af.path, target / af.path.filename(), ec);
      if (ec)
         std::cout << stamp() << "reorganise: rename failed for "
                   << af.path << ": " << ec.message() << std::endl;
      else
         dir_targets[af.path.parent_path()].insert({af.artist, af.album});
      }

   // For source dirs that fed exactly one (artist, album) target, relocate
   // any remaining files (covers, lyrics, etc.) to the same destination.
   for (auto& [src, targets] : dir_targets) {
      if (targets.size() != 1) continue;
      auto& [artist, album] = *targets.begin();
      fs::path target = batch_root / artist / album;
      for (auto& e : fs::directory_iterator(src, ec))
         if (e.is_regular_file())
            fs::rename(e.path(), target / e.path().filename(), ec);
      }

   // Prune empty directories — collect them all first, then sort deepest-first
   // so children are removed before parents.
   std::vector<fs::path> dirs;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec))
      if (e.is_directory()) dirs.push_back(e.path());
   std::sort(dirs.begin(), dirs.end(),
             [](const fs::path& a, const fs::path& b){
                return b.string().size() < a.string().size();
                });
   for (auto& d : dirs)
      if (fs::is_empty(d, ec)) fs::remove(d, ec);
   }

// Drops anything that is not well-formed UTF-8, and truncates only on a
// character boundary.
//
// The longest a single chapter title may be. A bound rather than a guess: the
// list is arbitrary text a client posted, and it comes straight back out
// through a JSON document and an XML attribute.
static constexpr size_t MAX_CHAPTER_NAME_BYTES = 500;

// The one place a chapter list becomes a response. Shared by getChapters,
// saveChapters and getAlbumChapters, so none of the three can describe the
// same markers differently — the panel is redrawn from the save's reply, and
// the album view from a fourth query, so a disagreement would show as the list
// changing under the user for no reason.
//
// `start` carries milliseconds because a save is a round trip: a client reads
// this list, edits one marker and writes the rest back, so truncating to whole
// seconds here would quietly flatten every fractional timestamp in a
// hand-written file. `duration` is whole seconds, matching Subsonic's Child,
// which is what a chapter row would become if these are ever listed as tracks.
// It is derived rather than stored — the next marker's start, or the video's
// own duration for the last, which only the server knows.
static nlohmann::json chapter_array_json(const std::vector<Chapter>& ch,
                                          double song_duration)
	{
	nlohmann::json list = nlohmann::json::array();
	for (size_t i = 0; i < ch.size(); i++) {
		double next = (i + 1 < ch.size()) ? ch[i + 1].start : song_duration;
		double d    = next - ch[i].start;
		list.push_back({
			{"index",    static_cast<int>(i + 1)},
			{"start",    std::llround(ch[i].start * 1000.0) / 1000.0},
			{"duration", d > 0 ? static_cast<int>(std::llround(d)) : 0},
			{"name",     ch[i].name} });
		}
	return list;
	}

static void chapter_array_xml(XMLDocument& doc, XMLElement* parent,
                               const std::vector<Chapter>& ch,
                               double song_duration)
	{
	for (size_t i = 0; i < ch.size(); i++) {
		double next = (i + 1 < ch.size()) ? ch[i + 1].start : song_duration;
		double d    = next - ch[i].start;
		auto* el = doc.NewElement("chapter");
		el->SetAttribute("index", static_cast<int>(i + 1));
		// Preformatted rather than handed to tinyxml2 as a double: its %.17g
		// would render 13.2 as 13.199999999999999.
		char buf[32];
		std::snprintf(buf, sizeof buf, "%.3f",
		              std::llround(ch[i].start * 1000.0) / 1000.0);
		el->SetAttribute("start",    buf);
		el->SetAttribute("duration", d > 0 ? static_cast<int>(std::llround(d)) : 0);
		el->SetAttribute("name",     ch[i].name.c_str());
		parent->InsertEndChild(el);
		}
	}

static void write_chapters_response(httplib::Response& res, int song_id,
                                     const MediaStore::VideoChapters& vc,
                                     double song_duration, bool writable,
                                     bool use_json)
	{
	std::string body;
	if (use_json)
		body = subsonic_ok_json([&](nlohmann::json& r) {
			r["chapters"] = {
				{"id",       sid(song_id)},
				{"source",   vc.source},
				{"writable", writable},
				{"chapter",  chapter_array_json(vc.chapters, song_duration)}
				};
			});
	else
		body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
			auto* parent = doc.NewElement("chapters");
			parent->SetAttribute("id",       song_id);
			parent->SetAttribute("source",   vc.source.c_str());
			parent->SetAttribute("writable", writable);
			chapter_array_xml(doc, parent, vc.chapters, song_duration);
			root->InsertEndChild(parent);
			});
	res.set_content(body, use_json ? "application/json" : "application/xml");
	}

// Splits a filename into the part a sibling shares and the part that names its
// kind, treating a sidecar's double extension as one suffix.
//
// std::filesystem splits on the *last* dot, so "Track.chapters.txt" has stem
// "Track.chapters" -- and that separates it from "Track.opus" in two different
// places downstream. reparent_loose_media() would file it under an album called
// "Track.chapters" while the media went to "Track"; and batch_merge_into()'s
// collision suffixing would produce "Track.chapters (2).txt", a name that no
// longer ends in CHAPTERS_SUFFIX, so it stops being a sidecar at all *and*
// starts appearing in getAlbumTexts as liner notes.
//
// Both were latent while nothing put a sidecar in a batch. A fetch that writes
// one from the tool's metadata makes them reachable.
static std::pair<std::string, std::string> split_sidecar_name(
	const std::filesystem::path& p)
	{
	const std::string name(p.filename().string());
	const std::string suffix(MediaStore::CHAPTERS_SUFFIX);
	if (name.size() > suffix.size()
	        && name.compare(name.size() - suffix.size(), suffix.size(),
	                        suffix) == 0)
		return { name.substr(0, name.size() - suffix.size()), suffix };
	return { p.stem().string(), p.extension().string() };
	}

// Turns a fetch tool's own metadata into the sidecar gaindrive indexes.
//
// yt-dlp writes <name>.info.json beside each download when asked, and its
// `chapters` array is the site's own parse of the timestamps the uploader
// wrote. That is the whole reason a fetched concert or DJ set can list its
// songs with nobody marking them up by hand.
//
// Keyed on the *file* rather than on which handler ran, so it serves an
// uploaded archive that happens to contain one, and a handler somebody
// configured with the flag themselves, without special-casing yt-dlp.
//
// **It must run before anything else in the pipeline renames a file**, which
// is why scan_batch() calls it first. std::filesystem splits on the last dot,
// so "Track.info.json" has stem "Track.info": reparent_loose_media() would
// file it under an album of that name and batch_merge_into() would suffix it
// "Track.info (2).json", either of which separates it from the media it
// describes. Running first also means the sidecar it writes travels the rest
// of the pipeline as an ordinary file, which split_sidecar_name() is what
// makes safe.
static void convert_tool_sidecars(const std::filesystem::path& batch_root)
	{
	namespace fs = std::filesystem;
	static constexpr std::string_view INFO_SUFFIX = ".info.json";
	auto ends_with = [](const std::string& s, std::string_view suf) {
		return s.size() > suf.size()
		    && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		};

	// Collected before anything is removed: mutating the tree under a
	// recursive_directory_iterator is undefined.
	std::error_code ec;
	std::vector<fs::path> found;
	for (auto& e : fs::recursive_directory_iterator(
	         batch_root, fs::directory_options::skip_permission_denied, ec)) {
		if (!e.is_regular_file()) continue;
		if (ends_with(e.path().filename().string(), INFO_SUFFIX))
			found.push_back(e.path());
		}

	for (const auto& info : found) {
		std::vector<Chapter> chapters;
		try {
			std::ifstream f(info, std::ios::binary);
			if (f) {
				std::string text((std::istreambuf_iterator<char>(f)),
				                  std::istreambuf_iterator<char>());
				// Through jsonread, never value(): this is third-party JSON
				// reached from fetch_worker()'s own thread, where an escaping
				// exception is std::terminate rather than a failed request --
				// and a video with no chapters sends "chapters": null, which
				// is exactly the shape value() throws on. A discarded parse is
				// not null either, and jsub() answers for that too.
				auto j = nlohmann::json::parse(text, nullptr, false);
				for (const auto& c : jsub(j, "chapters")) {
					if (chapters.size() >= MediaStore::MAX_CHAPTERS) break;
					const auto& st = jsub(c, "start_time");
					if (!st.is_number()) continue;
					Chapter ch;
					ch.start = st.get<double>();
					// Rejects a negative and, because the test is written the
					// positive way round, a NaN.
					if (!(ch.start >= 0)) continue;
					ch.name = chapter_clean_name(jstr(c, "title"));
					chapters.push_back(std::move(ch));
					}
				}
			}
		catch (const std::exception& ex) {
			std::cout << stamp() << "batch sidecars: cannot read " << info
			          << ": " << ex.what() << std::endl;
			}

		// Removed whether or not it yielded anything: it is the tool's
		// exhaust, and the library keeps the media and the sidecar.
		std::error_code rec;
		fs::remove(info, rec);

		// No chapters is not the same as *no* chapters: an empty sidecar is
		// the tombstone that overrules a container's own list, and a fetch has
		// no business asserting that on someone's behalf.
		if (chapters.empty()) continue;

		const std::string name = info.filename().string();
		fs::path target = info.parent_path()
		    / (name.substr(0, name.size() - INFO_SUFFIX.size())
		       + std::string(MediaStore::CHAPTERS_SUFFIX));

		// Never over a file that is already there: on a re-fetch that would
		// destroy markers somebody had corrected by hand.
		std::error_code xec;
		if (fs::exists(target, xec)) {
			std::cout << stamp() << "batch sidecars: keeping the existing "
			          << target.filename() << std::endl;
			continue;
			}

		std::ofstream out(target, std::ios::binary | std::ios::trunc);
		if (!out) {
			std::cout << stamp() << "batch sidecars: cannot write " << target
			          << std::endl;
			continue;
			}
		const std::string body = format_chapters(chapters);
		out.write(body.data(), static_cast<std::streamsize>(body.size()));
		out.close();
		if (!out) {
			std::cout << stamp() << "batch sidecars: write failed for "
			          << target << std::endl;
			fs::remove(target, rec);
			continue;
			}
		std::cout << stamp() << "batch sidecars: " << chapters.size()
		          << " chapter(s) -> " << target.filename() << std::endl;
		}
	}

// Pushes anything still loose in a batch down to <artist>/<album>/file.
//
// Two invariants downstream want exactly that depth and neither of them says so
// out loud. The batch is scanned by enumerating its *directories* — a file
// sitting at the batch root is never scanned at all — and deleteUpload accepts
// only a five-component path, so an album one level too shallow can never be
// removed by its owner. Handing scan_dirs() the batch directory itself
// would fix the first and make the second permanent: whatever directory it is
// given becomes an artist row named by its basename, and the user would be
// looking at a UUID in their artist list.
//
// A file directly in the batch is filed under `fallback_artist` — a handler's
// name reads far better than a UUID; a file one level down keeps the directory
// it is in, since something chose that name. Grouping by stem is what keeps a
// sidecar image with the video it belongs to.
//
// reorganise_by_tags() has usually done this already for audio. This is for
// what it cannot help with: it reads tags, and no video container has any.
static int reparent_loose_media(const std::filesystem::path& batch_root,
                                 const std::string& fallback_artist)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	std::vector<fs::path> loose;
	for (auto& e : fs::recursive_directory_iterator(batch_root, ec)) {
		if (!e.is_regular_file(ec)) continue;
		auto rel   = e.path().lexically_relative(batch_root);
		int  depth = (int)std::distance(rel.begin(), rel.end()) - 1;
		if (depth >= 2) continue;
		loose.push_back(e.path());
		}
	if (loose.empty()) return 0;

	std::string artist = sanitise_component(fallback_artist);
	if (artist.empty()) artist = "Unknown Artist";

	int moved = 0;
	for (const auto& p : loose) {
		// split_sidecar_name, not stem(): a loose "Track.chapters.txt" belongs
		// to the album "Track", beside the media it describes, not to one
		// called "Track.chapters" of its own.
		std::string album = sanitise_component(split_sidecar_name(p).first);
		if (album.empty()) album = "Unknown Album";
		fs::path parent = p.parent_path() == batch_root
		    ? batch_root / artist : p.parent_path();
		fs::path target = parent / album;
		fs::create_directories(target, ec);
		fs::rename(p, target / p.filename(), ec);
		if (ec)
			std::cout << stamp() << "reparent: rename failed for " << p << ": "
			          << ec.message() << std::endl;
		else
			moved++;
		}
	// Worth a line: for a URL fetch this means the handler's output template
	// wrote too shallow, which is the operator's to fix.
	if (moved)
		std::cout << stamp() << "reparent: filed " << moved
		          << " loose file(s) under " << batch_root << std::endl;
	return moved;
	}

// Moves everything in `src` into `dst`, which may already hold entries of the
// same name, then removes the emptied `src`.
//
// A plain fs::rename refuses a non-empty target, which is exactly the case
// merging exists for, so the move is done entry by entry. It recurses only
// where both sides are directories; doing a whole subtree in one call would
// abandon half a batch with nothing said about it.
static void batch_merge_into(const std::filesystem::path& src,
                             const std::filesystem::path& dst)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// Collected before anything moves: the first rename mutates the directory
	// this iterator is walking, and that is undefined.
	std::vector<fs::path> entries;
	for (auto& e : fs::directory_iterator(src, ec)) entries.push_back(e.path());
	if (ec) {
		std::cout << stamp() << "batch names: cannot read " << src << ": "
		          << ec.message() << std::endl;
		return;
		}

	fs::create_directories(dst, ec);
	if (ec) {
		std::cout << stamp() << "batch names: cannot create " << dst << ": "
		          << ec.message() << std::endl;
		return;
		}

	for (const auto& p : entries) {
		fs::path target = dst / p.filename();
		std::error_code sec, tec;
		if (fs::is_directory(p, sec) && fs::is_directory(target, tec)) {
			batch_merge_into(p, target);
			continue;
			}
		// Two sources really do collide: the built-in handler writes a
		// cover.<ext> into every album directory, so merging two albums under
		// one name brings two of them, and a rename would destroy one silently.
		for (int n = 2; n < 100; n++) {
			std::error_code xec;
			if (!fs::exists(target, xec)) break;
			// The number goes *before* a sidecar's double extension, so
			// "Track.chapters.txt" becomes "Track (2).chapters.txt" and stays
			// paired with the "Track (2).opus" beside it.
			auto [base, ext] = split_sidecar_name(p);
			target = dst / (base + " (" + std::to_string(n) + ")" + ext);
			}
		std::error_code rec;
		fs::rename(p, target, rec);
		if (rec)
			std::cout << stamp() << "batch names: rename failed for " << p
			          << " -> " << target << ": " << rec.message() << std::endl;
		}

	// Only when it really is empty: a rename that failed above left a file
	// behind, and removing the directory anyway would delete it.
	std::error_code eec;
	if (fs::is_empty(src, eec) && !eec) fs::remove(src, eec);
	}

// Renames every directory directly inside `parent` to `name`, merging where
// that collides. Never recurses — see apply_batch_names for why the depth
// matters.
static void batch_rename_level(const std::filesystem::path& parent,
                               const std::string& name)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// Two error codes on purpose: is_directory() would otherwise overwrite the
	// iterator's, so a directory that could not be opened would be reported as
	// whatever the last entry's type test happened to say.
	std::vector<fs::path> dirs;
	std::error_code dec;
	for (auto& e : fs::directory_iterator(parent, ec))
		if (e.is_directory(dec)) dirs.push_back(e.path());
	if (ec) {
		std::cout << stamp() << "batch names: cannot read " << parent << ": "
		          << ec.message() << std::endl;
		return;
		}

	const fs::path target = parent / name;

	// A regular file already sitting under the wanted name is not something to
	// merge into. Nothing should put one there, since reparent_loose_media ran
	// first, but iterating it would only set an error code and the sources
	// would be left renamed nowhere with no explanation.
	std::error_code fec;
	if (fs::exists(target, fec) && !fs::is_directory(target, fec)) {
		std::cout << stamp() << "batch names: " << target
		          << " is not a directory; leaving the names alone" << std::endl;
		return;
		}

	for (const auto& d : dirs) {
		if (d.filename() == name) continue;   // already the typed name

		// On a case-insensitive filesystem — macOS by default — exists() is
		// true for "artist" while "Artist" is what is on disk. Merging a
		// directory into itself would move each child into the directory it is
		// already in and then remove it, so the two are told apart by identity
		// rather than by name: equivalent() compares device and inode. A plain
		// rename is right there, and is what corrects the case.
		std::error_code xec, qec;
		bool exists = fs::exists(target, xec);
		bool same   = exists && fs::equivalent(d, target, qec) && !qec;

		if (exists && !same) {
			batch_merge_into(d, target);
			continue;
			}
		std::error_code rec;
		fs::rename(d, target, rec);
		if (rec)
			std::cout << stamp() << "batch names: rename failed for " << d
			          << " -> " << target << ": " << rec.message() << std::endl;
		}
	}

// Files a whole batch under names a person typed before pressing Fetch, by
// renaming the two directory levels the producers create. An empty name means
// "keep whatever the handler chose", so a blank artist with an album given
// renames only the second level.
//
// **This is a plain fs::rename and must never become relocate_prefix().**
// Nothing under the batch has been indexed yet: scan_batch() is the only thing
// that ever hands a batch to scan_dirs() and does so after this; scan() skips
// the uploads root outright; and FolderWatcher never watches it, so no inotify
// event can name one. There is therefore no row holding any of these paths to
// repair — and relocate_prefix's plain UPDATEs are safe only because its
// callers first checked the destination was free, which merging deliberately
// does not do.
//
// Renaming rather than substituting the names into the handler's -o template is
// also deliberate, and the second reason is the stronger one. urlfetch_expand()
// replaces whole argv elements, so a placeholder inside -o would need a
// substring rewrite; and -o is yt-dlp's own format language, where '%' is
// significant, so a name interpolated there would be expanded by the tool
// rather than by us. The guarantee that a hostile *field* cannot escape the
// batch says nothing about hostile template text.
//
// **Only the top two levels are touched.** A handler that wrote a third
// (Artist/Album/Disc 2/track) keeps it: that is a disc directory to the
// scanner, and renaming it would fuse two discs into one. Leaving the depth
// alone is also what preserves deleteUpload's exact-five-component check.
//
// Several directories at a level are merged into the one typed name, because
// "fetch this playlist as Artist X, Album Y" is the request being answered.
//
// Tags are deliberately *not* rewritten, unlike moveAlbum. The scanner takes
// artist and album from directory names and never from tags, so nothing in the
// API is wrong; there is no folder id yet to keep consistent; and running
// TagLib over a whole batch inside the fetch worker would add minutes to a path
// whose entire point is that "done" means done.
static void apply_batch_names(const std::filesystem::path& batch_root,
                              const std::string& artist,
                              const std::string& album)
	{
	namespace fs = std::filesystem;
	if (artist.empty() && album.empty()) return;

	// Belt and braces over sanitise_component's traversal guard, the same
	// pairing moveAlbum keeps. It cannot fire for a name that came through
	// the endpoint; it is here so that stops being an accident.
	auto one_component = [](const std::string& s) {
		return !s.empty() && s != "." && s != ".."
		    && s.find('/')  == std::string::npos
		    && s.find('\\') == std::string::npos;
		};
	if ((!artist.empty() && !one_component(artist))
	    || (!album.empty() && !one_component(album))) {
		std::cout << stamp() << "batch names: refusing a name that is not one "
		             "path component" << std::endl;
		return;
		}

	if (!artist.empty()) batch_rename_level(batch_root, artist);

	// The album level is renamed inside *every* artist directory, not just one.
	// With no artist given there may be several, and "call the album X" is as
	// true of each; with an artist given there is exactly one by now, so the
	// two cases are the same loop.
	if (!album.empty()) {
		std::error_code ec, dec;
		std::vector<fs::path> artists;
		for (auto& e : fs::directory_iterator(batch_root, ec))
			if (e.is_directory(dec)) artists.push_back(e.path());
		for (const auto& a : artists) batch_rename_level(a, album);
		}

	std::cout << stamp() << "batch names: " << batch_root << " filed under "
	          << (artist.empty() ? "<handler>" : artist) << " / "
	          << (album.empty()  ? "<handler>" : album) << std::endl;
	}

// Folds a finished batch into the user's earlier ones, and fills [to_scan] with
// the stored-form path each of its artist directories ended up at.
//
// **The batch UUID isolates a fetch while it runs; it is not how the uploads
// area is organised.** Without this, every fetch is its own island:
// scan_artist_dir() parents an artist directory straight to the root
// (`upsert_folder(artist_path, root_id)`), skipping the <user>/<uuid> levels, so
// two batches naming the same artist become two folder rows with the same name
// and the personal listing shows both. Fetching six tracks of one concert — the
// case the sticky name fields exist for — produced six artists holding one
// one-track album each.
//
// So the isolation is kept exactly where it is needed and dropped afterwards.
// The batch must stay its own directory *during* the fetch: a failed, cancelled
// or timed-out job does remove_all() on it, and a shared directory would let a
// failure delete files an earlier fetch had already put there.
//
// batch_merge_into() does the work and already has the right semantics — it
// recurses where both sides are directories, so an album inside a merged artist
// merges too, and it suffixes colliding *files* rather than overwriting them,
// which it does because the built-in handler writes a cover.<ext> into every
// album directory.
//
// Callers must serialise this: two batches folding into each other at once would
// each move the other's contents away. See batch_fold_mu_.
static void fold_batch_into_siblings(const std::filesystem::path& batch_root,
                                     const std::string& rel_batch,
                                     std::set<std::string>& to_scan)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// "<uploads root>/<user>", the stored-form prefix every batch of this user
	// shares. rel_batch always has at least two components, being built as
	// "<root>/<user>/<uuid>".
	auto cut = rel_batch.rfind('/');
	if (cut == std::string::npos) return;
	const std::string rel_user = rel_batch.substr(0, cut);

	// The user's other batches, in a fixed order so that repeated fetches
	// converge on the same one rather than picking a different target each
	// time. In practice at most one holds any given name, because this runs
	// after every successful batch — the ordering matters only for batches
	// that predate it.
	std::vector<fs::path> siblings;
	for (auto& e : fs::directory_iterator(batch_root.parent_path(), ec)) {
		std::error_code dec;
		if (!e.is_directory(dec)) continue;
		// Identity, not name: the same reason batch_rename_level uses it.
		std::error_code qec;
		if (fs::equivalent(e.path(), batch_root, qec) && !qec) continue;
		siblings.push_back(e.path());
		}
	std::sort(siblings.begin(), siblings.end());

	// Collected before anything moves: merging mutates the directory this would
	// otherwise still be iterating.
	std::vector<fs::path> mine;
	for (auto& e : fs::directory_iterator(batch_root, ec)) {
		std::error_code dec;
		if (e.is_directory(dec)) mine.push_back(e.path());
		}

	for (const auto& a : mine) {
		const std::string name = a.filename().string();

		// Every earlier batch already holding this name. Normally at most one,
		// since this runs after every successful batch — but a library that
		// predates the fold can hold several, and merging *all* of them is what
		// clears those up instead of leaving the strays there for ever.
		std::vector<fs::path> holders;
		for (const auto& s : siblings) {
			std::error_code xec;
			if (fs::is_directory(s / name, xec)) holders.push_back(s);
			}
		if (holders.empty()) {
			to_scan.insert(rel_batch + "/" + name);
			continue;
			}

		const fs::path keep       = holders.front() / name;
		const std::string keep_rel =
			rel_user + "/" + holders.front().filename().string() + "/" + name;

		// The strays first, so the survivor holds everything before this batch
		// joins it. Each of *these* was scanned when it was made, unlike the
		// batch being folded — so its own path goes into the scan set too: the
		// directory is about to stop existing, and scan_artist_dir() finding it
		// gone is what prunes the folder row that still names it. Leaving that
		// out would swap one visible duplicate for one invisible phantom.
		//
		// A stray's stars and play counts do not survive, and cannot: they are
		// keyed on the path, and the one tool for moving that key —
		// relocate_prefix() — is safe only when the destination is free, which
		// is precisely what merging is not. Accepted rather than worked around,
		// because this is pre-promotion staging: the rows can only exist if
		// somebody played a duplicate they had not filed yet, and the
		// alternative is leaving the duplicate on screen for ever.
		for (size_t i = 1; i < holders.size(); i++) {
			batch_merge_into(holders[i] / name, keep);
			to_scan.insert(rel_user + "/" + holders[i].filename().string()
			               + "/" + name);
			}

		// Nothing under *this* batch was ever indexed — scan_batch() is the only
		// thing that hands a batch to scan_dirs() and does so after this — so
		// the source needs no prune, only the destination a rescan.
		batch_merge_into(a, keep);
		to_scan.insert(keep_rel);
		std::cout << stamp() << "batch merge: " << rel_batch << "/" << name
		          << " -> " << keep_rel
		          << (holders.size() > 1
		              ? " (and " + std::to_string(holders.size() - 1) + " stray)"
		              : "")
		          << std::endl;
		}

	// Only when the fold emptied it. A batch holding an artist nobody else had
	// stays exactly where it is.
	std::error_code eec;
	if (fs::is_empty(batch_root, eec) && !eec) fs::remove(batch_root, eec);
	}

// How many fetches may be waiting at once. One worker runs the queue, so this
// is a bound on how far behind a user can get the server, not on throughput.
static constexpr size_t FETCH_QUEUE_MAX = 20;

// How many finished jobs are kept per user, and for how long. Per user rather
// than server-wide: a shared cap lets one busy account evict another's results
// before that person's browser has polled for them.
static constexpr size_t FETCH_KEEP_PER_USER = 10;
static constexpr int64_t FETCH_KEEP_S       = 15 * 60;

// Grace period before the SSE-as-heartbeat watchdog tears down a cast
// session whose listener has gone away. Long enough to ride out a page
// reload or a brief network blip; short enough that closing the tab
// actually stops the cast.
static constexpr int CAST_IDLE_GRACE_S = 30;

// How many SSE listeners one cast session may hold open at once. Each one is
// a worker thread parked for the life of the connection; see castEvents.
static constexpr int CAST_SSE_MAX_LISTENERS = 8;

GainDrive::GainDrive(const std::string& db_path,
                     const std::vector<MediaStore::Root>& roots,
                     const std::string& upload_dir,
                     bool no_scan,
                     bool debug,
                     bool flat_multi_disc,
                     const std::string& user_db_path,
                     const std::string& transcode_cache_dir,
                     int transcode_cache_mb,
                     int transcode_jobs,
                     int video_art_px,
                     bool video_art_frames,
                     bool video_art_embedded,
                     const std::vector<CastManager::CastDevice>& cast_devices,
                     const std::optional<std::vector<UrlHandler>>& url_handlers,
                     int url_fetch_timeout_s,
                     int scan_jobs)
	: debug_(debug), flat_multi_disc_(flat_multi_disc), upload_dir_(upload_dir),
	  store_(db_path, roots, user_db_path, video_art_px, video_art_frames,
	         video_art_embedded, scan_jobs),
	  transcode_cache_(
	      transcode_cache_dir.empty()
	          ? std::filesystem::path(db_path).parent_path() / "transcodes"
	          : std::filesystem::path(transcode_cache_dir),
	      static_cast<int64_t>(transcode_cache_mb) * 1024 * 1024,
	      transcode_jobs > 0 ? transcode_jobs
	          : std::max(2u, std::thread::hardware_concurrency() / 2)),
	  cover_cache_(store_),
	  url_fetcher_(url_handlers, url_fetch_timeout_s),
	  watcher_(store_)
	{
	cast_manager_.set_manual_devices(cast_devices);

	namespace fs = std::filesystem;
	// A cache inside any root would be rescanned and indexed, and the
	// transcodes would then appear in the library as tracks of their own.
	if (store_.path_is_within_root(
	        fs::absolute(transcode_cache_dir.empty()
	            ? fs::path(db_path).parent_path() / "transcodes"
	            : fs::path(transcode_cache_dir)).string())) {
		std::cerr << stamp()
		          << "Error: the transcode cache must not live inside "
		          << "a library root." << std::endl;
		std::exit(1);
		}
	// Socket options, including the deliberate SO_REUSEADDR-not-SO_REUSEPORT
	// choice that stops a second gaindrive silently sharing this port, are set
	// further down alongside the TCP keepalive settings — set_socket_options()
	// takes a single callback, so they have to live together.

	// A cache miss now blocks its request thread for the whole transcode
	// (seconds, not milliseconds), so the default pool of 8 is too small to
	// absorb a client that pins an album and fans out downloads.  The ffmpeg
	// count is bounded separately by --transcode-jobs; this only bounds waiting.
	// The queue behind the pool is bounded (third argument): unbounded, a
	// slowloris that pinned all 32 workers would go on accepting connections
	// into an ever-growing deque, each holding a file descriptor, until
	// EMFILE. Refusing the 129th queued request instead sheds load at the
	// edge, which is the recoverable failure. max_n stays 0 — a fixed pool.
	server_.new_task_queue = []{ return new httplib::ThreadPool(32, 0, 128); };

	// httplib's payload cap defaults to SIZE_MAX. Since 0.54 the body of a
	// routed request is read only after the route has matched and the
	// pre-request handler has approved it, so the cap no longer has to be
	// small to protect unauthenticated callers — the pre-request handler
	// below is that protection. What this still has to clear is the one
	// legitimately large body, an archive streamed to /upload, because the
	// content-reader path enforces the same cap on what it hands the
	// receiver.
	server_.set_payload_max_length(MAX_REQUEST_BYTES);

	// The per-route body bound, enforced before a byte of body is read.
	// Every request that carries content must name its length, and every
	// route but /upload gets a small one: the largest legitimate small body
	// is a saveChapters file, which MAX_CHAPTERS bounds far below a megabyte.
	// /upload is exempt because its handler streams the body through a
	// content reader to disk under its own cap — it never buffers it — and
	// authenticates from the query string before reading at all.
	//
	// Chunked (or otherwise length-less) bodies are refused outside /upload
	// rather than read-and-measured: read_content would buffer up to the
	// global cap before anyone could object, which is the allocation this
	// handler exists to prevent. No Subsonic client sends one.
	server_.set_pre_request_handler([](const httplib::Request& req,
	                                   httplib::Response& res) {
		if (req.matched_route == "/upload")
			return httplib::Server::HandlerResponse::Unhandled;
		const bool has_body = req.method == "POST" || req.method == "PUT"
		                   || req.method == "PATCH"
		                   || req.has_header("Content-Length")
		                   || req.has_header("Transfer-Encoding");
		if (!has_body)
			return httplib::Server::HandlerResponse::Unhandled;
		if (req.has_header("Transfer-Encoding")) {
			res.status = 411;   // Length Required
			return httplib::Server::HandlerResponse::Handled;
			}
		// setCoverArt takes an uploaded image, bounded where it is read by
		// MAX_COVER_BYTES; the slack on top is multipart framing.
		const size_t cap = req.matched_route == "/rest/setCoverArt.view"
		    ? MAX_COVER_BYTES + 64 * 1024
		    : MAX_SMALL_BODY_BYTES;
		const auto len = req.get_header_value_u64("Content-Length", 0);
		if (len > cap) {
			res.status = 413;
			return httplib::Server::HandlerResponse::Handled;
			}
		return httplib::Server::HandlerResponse::Unhandled;
		});

	if (!fs::exists(upload_dir_))
		fs::create_directories(upload_dir_);
	// Personal uploads live in their own root rather than a hidden directory
	// inside a library, so nothing there can be mistaken for someone's album.
	// With no uploads root configured both stay empty and the upload endpoints
	// refuse — better than silently writing into a library root.
	if (const auto* up = store_.uploads_root()) {
		users_dir_         = up->path;
		uploads_root_name_ = up->name;
		fs::create_directories(users_dir_);
		}
	else
		std::cout << stamp() << "No uploads root configured; personal uploads "
		          << "are disabled." << std::endl;

	// Normalise /rest/foo → /rest/foo.view so clients that omit the suffix still work.
	// In debug mode also strip Accept-Encoding: httplib swaps compressed bytes into
	// res.body before firing the logger, making it unreadable (cpp-httplib#1656).
	//
	// Also the one place CORS is answered — and it is answered only for the
	// endpoints a Cast receiver fetches for itself. **A Chromecast needs this
	// to show a subtitle.** The receiver fetches a side-loaded WebVTT track by
	// XHR, and declaring any track at all puts its media element into
	// anonymous cross-origin mode — so the *film* needs the header as much as
	// the captions do, on its 206 responses as much as its 200s.
	//
	// It used to be `*` on every endpoint, on the reasoning that credentials
	// ride in query parameters so there is no ambient authority for a hostile
	// page to borrow. That was true and still incomplete: the Subsonic
	// envelope is HTTP 200 for success and failure alike, so only a
	// CORS-readable body distinguishes a right password from a wrong one —
	// `*` on ping.view made every web page a visitor opens a password oracle
	// against this server, rate-limited only by the login throttle. And a
	// leaked credentialed URL (a proxy log, a pasted link) was readable from
	// any origin. The receiver's three endpoints keep `*` because a Cast
	// token is scoped to one song of one LOAD; everything else answers
	// cross-origin reads with nothing, which is what a same-origin web client
	// and every native client expect anyway.
	server_.set_pre_routing_handler([this](const httplib::Request& req,
	                                       httplib::Response& res) {
		auto& r = const_cast<httplib::Request&>(req);
		if (r.path.rfind("/rest/", 0) == 0 && r.path.find('.') == std::string::npos)
			r.path += ".view";
		if (debug_)
			r.headers.erase("Accept-Encoding");

		// On every response, CORS or not: a cover is client-supplied bytes
		// served back, so it must never be content-sniffed into something
		// else, and a Referer must not carry this origin to the provider
		// links the client renders.
		res.set_header("X-Content-Type-Options", "nosniff");
		res.set_header("Referrer-Policy", "no-referrer");

		const bool cast_fetched = r.path == "/rest/stream.view"
		                       || r.path == "/rest/getCaptions.view"
		                       || r.path == "/rest/hls.m3u8"
		                       || r.path == "/rest/hls.view";
		if (cast_fetched) {
			res.set_header("Access-Control-Allow-Origin",  "*");
			res.set_header("Access-Control-Allow-Methods", "GET, HEAD, OPTIONS");
			res.set_header("Access-Control-Allow-Headers", "Range, Content-Type");
			// Without this a cross-origin reader is allowed the body but not
			// the headers that say how long it is or which part this was.
			res.set_header("Access-Control-Expose-Headers",
			               "Content-Length, Content-Range, Accept-Ranges");
			}

		// Answered here because nothing routes it: there is no Options()
		// handler for any path, so a preflight would otherwise fall through to
		// the 404 and take the real request with it.
		if (r.method == "OPTIONS") {
			res.status = 204;
			return httplib::Server::HandlerResponse::Handled;
			}
		return httplib::Server::HandlerResponse::Unhandled;
		});

	// Without one of these, httplib's fallback answers 500 and puts the
	// exception's what() into an `EXCEPTION_WHAT` **response header**. For a
	// SQLite exception that string carries filesystem paths, and root paths
	// are private to MediaStore and are deliberately never surfaced in an API
	// response — so the fallback quietly undid that rule for every unexpected
	// throw. The detail belongs in the log, where it is useful, and the client
	// gets an ordinary Subsonic error.
	server_.set_exception_handler([](const httplib::Request& req,
	                                  httplib::Response& res,
	                                  std::exception_ptr ep) {
		std::string what = "unknown";
		try { std::rethrow_exception(ep); }
		catch (const std::exception& e) { what = e.what(); }
		catch (...) {}
		std::cout << stamp(client_addr(req)) << "unhandled exception in "
		          << log_safe(req.path) << ": " << what << std::endl;
		const bool use_json = (fmt_of(req) == "json");
		res.status = 500;
		res.set_content(use_json
		                ? subsonic_error_json(0, "Internal server error.")
		                : subsonic_error(0, "Internal server error."),
		                use_json ? "application/json" : "application/xml");
		});

	// The access log names every parameter, which is what makes it useful for
	// diagnosing a client — and is why it has to redact. Credentials ride in
	// the query string on every single request, so without this the journal is
	// a second complete plaintext credential store, kept for longer than the
	// database and usually readable by more people. `p` is the password
	// itself, `password` is a *new* one on its way through changePassword,
	// createUser or updateUser, `t`/`s` are the token pair, which is
	// replayable for as long as the password behind it lives, and `castToken`
	// is a bearer credential in its own right.
	//
	// Values are also stripped of CR and LF: they are attacker-supplied and
	// were written raw, so a parameter could forge whole log lines — which
	// matters more once something is reading this log to decide who to block.
	server_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
		static const std::set<std::string> secret_params = {
			"p", "password", "t", "s", "castToken",
			// Server-side API keys arrive as ordinary GET parameters on
			// saveServerSettings, and getServerSettings goes to some lengths
			// never to read them back — a log line that printed them would
			// undo that for every proxy and journal on the path.
			"discogsToken", "tmdbKey"
			};
		std::cout << stamp(client_addr(req))
		          << req.method << " " << log_safe(req.path);
		if (!req.params.empty()) {
			std::cout << "?";
			bool first = true;
			for (auto& [k, v] : req.params) {
				if (!first) std::cout << "&";
				std::cout << log_safe(k, 64) << "="
				          << (secret_params.count(k) ? "<redacted>" : log_safe(v));
				first = false;
				}
			}
		std::cout << " -> " << res.status << std::endl;
		if (debug_ && !res.body.empty()) {
			auto ct = res.get_header_value("Content-Type");
			std::cout << ct << "\n";
//			if (ct == "application/json" || ct == "application/xml")
//				std::cout << res.body << "\n";
			}
		});

	// Audio streams are consumed at playback speed, so the send buffer can stay
	// full for a long time while the Chromecast plays through its local buffer.
	// Increase the write timeout well beyond the longest expected track to prevent
	// httplib from closing the connection mid-stream.
	server_.set_write_timeout(3600, 0);   // 1 hour

	// Enable TCP keepalives so the NAT table entry stays alive while the Cast
	// receiver has its TCP window at zero (buffer full, not reading).  Without
	// this the router drops the idle connection after ~60-90 s.
	// Accepted sockets inherit SO_KEEPALIVE from the listening socket on Linux.
	server_.set_socket_options([](int sock) {
		// SO_REUSEADDR rather than httplib::default_socket_options(), which
		// sets SO_REUSEPORT on Linux — see the note above and in CLAUDE.md.
		// SO_REUSEPORT would let a second gaindrive bind this same port
		// successfully and have the kernel split traffic between the two.
		int reuse = 1;
		setsockopt(sock, SOL_SOCKET,  SO_REUSEADDR,   &reuse, sizeof(reuse));
		int on = 1;
		setsockopt(sock, SOL_SOCKET,  SO_KEEPALIVE,   &on, sizeof(on));
		int idle  = 10;   // start probing after 10 s of silence
		int intvl =  5;   // probe every 5 s
		int cnt   =  3;   // give up after 3 missed probes
		// Darwin spells the idle timer TCP_KEEPALIVE; same units (seconds).
		// Keyed on the macro rather than __APPLE__ so any platform that
		// happens to use either name works without another #ifdef here.
#ifdef TCP_KEEPIDLE
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE,   &idle,  sizeof(idle));
#elif defined(TCP_KEEPALIVE)
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPALIVE,  &idle,  sizeof(idle));
#endif
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL,  &intvl, sizeof(intvl));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT,    &cnt,   sizeof(cnt));
		});

	// Web client — serve embedded static files.
	//
	// A track link opened on an Android device is answered with a chooser
	// page — an intent:// anchor aimed at the app, and a browser link —
	// instead of the SPA.  web=1 is the loop-breaker: the chooser's own
	// browser link and the intent's fallback URL both carry it, or an
	// Android browser would be handed the chooser again for ever.  The page
	// composes its anchors from location.href itself, so nothing from the
	// query string is spliced into HTML here, and the public scheme and
	// host — which behind a proxy this process does not know — come free.
	// The two HTML pages carry a Content-Security-Policy; assets and API
	// responses need none, since a policy governs only the document that a
	// browser renders. frame-ancestors 'none' is the load-bearing directive:
	// without it the Settings pane — delete user, move album — can be framed
	// and overlaid by any origin, and one click on the overlay is a click on
	// this UI. The SPA loads nothing external and keeps its one inline script
	// in theme.js precisely so script-src can be 'self' with no carve-out;
	// link.html is deliberately self-contained (it must survive with no other
	// asset loading), so its policy allows its own inline script and style
	// and nothing else. X-Frame-Options is the same rule for browsers that
	// predate frame-ancestors.
	auto index_page = [](const httplib::Request& req, httplib::Response& res) {
		res.set_header("X-Frame-Options", "DENY");
		if (req.has_param("track") && !req.has_param("web")
		    && req.get_header_value("User-Agent").find("Android")
		       != std::string::npos) {
			res.set_header("Content-Security-Policy",
			               "default-src 'none'; script-src 'unsafe-inline'; "
			               "style-src 'unsafe-inline'; base-uri 'none'; "
			               "form-action 'none'; frame-ancestors 'none'");
			res.set_content(embedded::link_html.data(), embedded::link_html.size(),
			                embedded::link_html_mime.data());
			}
		else {
			res.set_header("Content-Security-Policy",
			               "default-src 'none'; script-src 'self'; "
			               "style-src 'self' 'unsafe-inline'; img-src 'self'; "
			               "media-src 'self'; connect-src 'self'; "
			               "font-src 'self'; base-uri 'none'; "
			               "form-action 'self'; frame-ancestors 'none'");
			res.set_content(embedded::index_html.data(), embedded::index_html.size(),
			                embedded::index_html_mime.data());
			}
		};
	server_.Get("/",           index_page);
	server_.Get("/index.html", index_page);
	server_.Get("/style.css", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::style_css.data(), embedded::style_css.size(),
		                embedded::style_css_mime.data());
		});
	server_.Get("/app.js", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::app_js.data(), embedded::app_js.size(),
		                embedded::app_js_mime.data());
		});
	server_.Get("/theme.js", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::theme_js.data(), embedded::theme_js.size(),
		                embedded::theme_js_mime.data());
		});
	server_.Get("/favicon.svg", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(embedded::favicon_svg.data(), embedded::favicon_svg.size(),
		                embedded::favicon_svg_mime.data());
		});
	// Half a megabyte that only changes when the binary does, so it is worth
	// telling the browser not to ask again.
	server_.Get("/material-symbols-rounded.woff2",
	            [](const httplib::Request&, httplib::Response& res) {
		res.set_header("Cache-Control", "public, max-age=31536000, immutable");
		res.set_content(embedded::material_symbols_woff2.data(),
		                embedded::material_symbols_woff2.size(),
		                embedded::material_symbols_woff2_mime.data());
		});

	// ping
	server_.Get("/rest/ping.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getOpenSubsonicExtensions — no auth required; clients call this before login.
	server_.Get("/rest/getOpenSubsonicExtensions.view",
	            [this](const httplib::Request& req, httplib::Response& res) {
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				// One version until the first public release: everything the
				// extension offers is version 1. A new *endpoint* will earn a
				// new number, since a client cannot discover one without being
				// told; a new optional parameter on an endpoint an existing
				// version already names will not, since a client that ignores
				// it gets exactly what it got before. doc/api.toml is the
				// contract.
				//
				// transcodeOffset is the standard extension meaning no more
				// than that stream honours timeOffset for audio, which it has
				// always done — so it names behaviour rather than adding any.
				r["openSubsonicExtensions"] = {
					{{"name", "gaindrive"},
					 {"versions", nlohmann::json::array({1})}},
					{{"name", "transcodeOffset"},
					 {"versions", nlohmann::json::array({1})}}};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				auto* exts = doc.NewElement("openSubsonicExtensions");
				for (const char* name : {"gaindrive", "transcodeOffset"}) {
					auto* ext = doc.NewElement("extension");
					ext->SetAttribute("name", name);
					ext->SetAttribute("versions", "1");
					exts->InsertEndChild(ext);
					}
				root->InsertEndChild(exts);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getLicense — perpetually-valid dummy.
	server_.Get("/rest/getLicense.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				r["license"] = {
					{"valid",          true},
					{"email",          "gaindrive@example.com"},
					{"licenseExpires", "2099-01-01T00:00:00"}
					};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				auto* lic = doc.NewElement("license");
				lic->SetAttribute("valid",          "true");
				lic->SetAttribute("email",          "gaindrive@example.com");
				lic->SetAttribute("licenseExpires", "2099-01-01T00:00:00");
				root->InsertEndChild(lic);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// ---- Media library scanning (standard, Subsonic 1.15.0) ---------------
	//
	// Both take no parameters and answer with the same <scanStatus> element.
	// `scanning` is a real boolean and `count` a real number in JSON: a strict
	// client throws on a type mismatch before any of the response is usable,
	// which is what the whole of SPEC-AUDIT.md is about.
	auto scan_status_body = [this](bool use_json) {
		auto st = store_.scan_status();
		if (use_json)
			return subsonic_ok_json([&](nlohmann::json& r) {
				r["scanStatus"] = {{"scanning", st.scanning},
				                   {"count",    st.count}};
				});
		return subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
			auto* el = doc.NewElement("scanStatus");
			el->SetAttribute("scanning", st.scanning);
			el->SetAttribute("count",    static_cast<int64_t>(st.count));
			root->InsertEndChild(el);
			});
		};

	server_.Get("/rest/getScanStatus.view",
	            [this, scan_status_body](const httplib::Request& req,
	                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		res.set_content(scan_status_body(use_json),
		                use_json ? "application/json" : "application/xml");
		});

	// startScan — admin only, matching the rule that settingsRole mirrors
	// adminRole. Note --no-scan suppresses only the scan at start-up; asking
	// for one explicitly still works.
	server_.Get("/rest/startScan.view",
	            [this, scan_status_body](const httplib::Request& req,
	                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto ui = store_.get_user(req.get_param_value("u"));
		if (!ui || !ui->is_admin) {
			const char* msg = "Scanning the library requires admin role.";
			res.set_content(use_json ? subsonic_error_json(50, msg)
			                         : subsonic_error(50, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		// Already scanning: report that rather than starting a second walk
		// over the same tree. Nothing would break — the scan is idempotent and
		// SQLite serialises the writes — but it would double the I/O and make
		// `count` jump about between two walks sharing one counter.
		if (!store_.scan_status().scanning)
			std::thread([this]{
				try { store_.scan(); }
				catch (const std::exception& e) {
					std::cout << stamp() << "Scan aborted: " << e.what()
					          << std::endl;
					}
				catch (...) {
					std::cout << stamp() << "Scan aborted: unknown exception"
					          << std::endl;
					}
				}).detach();

		res.set_content(scan_status_body(use_json),
		                use_json ? "application/json" : "application/xml");
		});

	// getUser
	server_.Get("/rest/getUser.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k) {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : "";
			};

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string target = qp("username");
		if (target.empty()) { err(10, "Required parameter missing: username."); return; }

		std::string requester = qp("u");
		if (target != requester) {
			auto ri = store_.get_user(requester);
			if (!ri || !ri->is_admin) {
				err(50, "User is not authorized for this operation.");
				return;
				}
			}

		auto ui = store_.get_user(target);
		if (!ui) { err(70, "User not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&ui](nlohmann::json& r) {
				r["user"] = {
					{"username",          ui->username},
					{"email",             ui->email},
					{"scrobblingEnabled", false},
					{"adminRole",         ui->is_admin},
					{"settingsRole",      ui->is_admin},
					{"downloadRole",      true},
					{"uploadRole",        ui->upload_allowed},
					{"playlistRole",      true},
					{"coverArtRole",      true},
					{"commentRole",       false},
					{"podcastRole",       false},
					{"streamRole",        true},
					{"jukeboxRole",       false},
					{"shareRole",         false},
					{"maxBitRate",        ui->max_bitrate},
					{"disabled",          ui->disabled},
					{"castRole",          ui->cast_allowed}
					};
				});
		else
			body = subsonic_ok([&ui](XMLDocument& doc, XMLElement* root) {
				auto* u = doc.NewElement("user");
				u->SetAttribute("username",          ui->username.c_str());
				u->SetAttribute("email",             ui->email.c_str());
				u->SetAttribute("scrobblingEnabled", false);
				u->SetAttribute("adminRole",         ui->is_admin);
				u->SetAttribute("settingsRole",      ui->is_admin);
				u->SetAttribute("downloadRole",      true);
				u->SetAttribute("uploadRole",        ui->upload_allowed);
				u->SetAttribute("playlistRole",      true);
				u->SetAttribute("coverArtRole",      true);
				u->SetAttribute("commentRole",       false);
				u->SetAttribute("podcastRole",       false);
				u->SetAttribute("streamRole",        true);
				u->SetAttribute("jukeboxRole",       false);
				u->SetAttribute("shareRole",         false);
				u->SetAttribute("maxBitRate",        ui->max_bitrate);
				u->SetAttribute("disabled",          ui->disabled);
				u->SetAttribute("castRole",          ui->cast_allowed);
				root->InsertEndChild(u);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getUsers — returns all users; admin only.
	server_.Get("/rest/getUsers.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto ri = store_.get_user(req.params.find("u")->second);
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		auto users = store_.list_users();
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&users](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& u : users)
					arr.push_back({
						{"username",          u.username},
						{"email",             u.email},
						{"scrobblingEnabled", false},
						{"adminRole",         u.is_admin},
						{"settingsRole",      u.is_admin},
						{"downloadRole",      true},
						{"uploadRole",        u.upload_allowed},
						{"playlistRole",      true},
						{"coverArtRole",      true},
						{"commentRole",       false},
						{"podcastRole",       false},
						{"streamRole",        true},
						{"jukeboxRole",       false},
						{"shareRole",         false},
						{"maxBitRate",        u.max_bitrate},
						{"disabled",          u.disabled},
				{"castRole",          u.cast_allowed}
						});
				r["users"] = {{"user", arr}};
				});
		else
			body = subsonic_ok([&users](XMLDocument& doc, XMLElement* root) {
				auto* us = doc.NewElement("users");
				for (const auto& u : users) {
					auto* ue = doc.NewElement("user");
					ue->SetAttribute("username",          u.username.c_str());
					ue->SetAttribute("email",             u.email.c_str());
					ue->SetAttribute("scrobblingEnabled", false);
					ue->SetAttribute("adminRole",         u.is_admin);
					ue->SetAttribute("settingsRole",      u.is_admin);
					ue->SetAttribute("downloadRole",      true);
					ue->SetAttribute("uploadRole",        u.upload_allowed);
					ue->SetAttribute("playlistRole",      true);
					ue->SetAttribute("coverArtRole",      true);
					ue->SetAttribute("commentRole",       false);
					ue->SetAttribute("podcastRole",       false);
					ue->SetAttribute("streamRole",        true);
					ue->SetAttribute("jukeboxRole",       false);
					ue->SetAttribute("shareRole",         false);
					ue->SetAttribute("maxBitRate",        u.max_bitrate);
					ue->SetAttribute("disabled",          u.disabled);
					ue->SetAttribute("castRole",          u.cast_allowed);
					us->InsertEndChild(ue);
					}
				root->InsertEndChild(us);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createUser — creates a new user; admin only.
	server_.Get("/rest/createUser.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		std::string username = qp("username");
		// enc: decoded before storing — validate_auth decodes it on the way
		// in, so storing the encoded spelling stores a different password.
		auto decoded = MediaStore::decode_enc_password(qp("password"));
		if (!decoded) { err(10, "Malformed enc: password."); return; }
		std::string password = *decoded;
		if (username.empty()) { err(10, "Required parameter missing: username."); return; }
		if (password.empty()) { err(10, "Required parameter missing: password."); return; }
		if (!MediaStore::valid_username(username)) {
			err(0, "Username may contain only letters, digits, '.', '_' and '-'.");
			return;
			}

		bool is_admin       = (qp("adminRole")  == "true");
		bool upload_allowed = (qp("uploadRole") == "true");
		bool disabled       = (qp("disabled")   == "true");
		bool cast_allowed   = (qp("castRole")   == "true");
		int  max_bitrate    = 0;
		if (!qp("maxBitRate").empty()) max_bitrate = to_int(qp("maxBitRate"), 0);

		if (!store_.add_user(username, password, is_admin)) {
			err(0, "User already exists.");
			return;
			}
		store_.update_user(username, "", qp("email"), is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// updateUser — updates an existing user; admin only.
	server_.Get("/rest/updateUser.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		std::string username = qp("username");
		if (username.empty()) { err(10, "Required parameter missing: username."); return; }

		// Fetch current values so unspecified params keep their existing value.
		auto existing = store_.get_user(username);
		if (!existing) { err(70, "User not found."); return; }

		bool is_admin       = qp("adminRole").empty()  ? existing->is_admin       : (qp("adminRole")  == "true");
		bool upload_allowed = qp("uploadRole").empty() ? existing->upload_allowed : (qp("uploadRole") == "true");
		bool disabled       = qp("disabled").empty()   ? existing->disabled       : (qp("disabled")   == "true");
		bool cast_allowed   = qp("castRole").empty()   ? existing->cast_allowed   : (qp("castRole")   == "true");
		int  max_bitrate    = qp("maxBitRate").empty()  ? existing->max_bitrate    : std::max(0, to_int(qp("maxBitRate"), existing->max_bitrate));
		std::string email   = qp("email").empty()       ? existing->email          : qp("email");
		// enc: decoded before storing, as in createUser.
		auto pw_dec = MediaStore::decode_enc_password(qp("password"));
		if (!pw_dec) { err(10, "Malformed enc: password."); return; }
		std::string pw      = *pw_dec;

		// An admin cannot disable their own account.
		if (username == qp("u") && disabled)
			{ err(0, "You cannot disable your own account."); return; }

		store_.update_user(username, pw, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// changePassword — change a user's password; admins can change any user's,
	// regular users can only change their own.
	server_.Get("/rest/changePassword.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};

		std::string requester = qp("u");
		std::string target    = qp("username");
		// enc: decoded before storing, as in createUser.
		auto decoded = MediaStore::decode_enc_password(qp("password"));
		if (!decoded) { err(10, "Malformed enc: password."); return; }
		std::string password  = *decoded;
		if (target.empty())   { err(10, "Required parameter missing: username."); return; }
		if (password.empty()) { err(10, "Required parameter missing: password."); return; }

		if (target != requester) {
			auto ri = store_.get_user(requester);
			if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }
			}

		auto existing = store_.get_user(target);
		if (!existing) { err(70, "User not found."); return; }

		store_.update_user(target, password, existing->email, existing->is_admin,
		                   existing->max_bitrate, existing->upload_allowed, existing->disabled,
		                   existing->cast_allowed);

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getServerSettings / saveServerSettings — admin-only server configuration.
	server_.Get("/rest/getServerSettings.view", [this](const httplib::Request& req,
	                                                    httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};
		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		// Whether each is set, never what it is.  A secret the server can
		// hand back is a secret in an API response, in the browser's memory
		// and in whatever cache sits between — and, because the web client
		// filled a masked box with it, one the browser's own password manager
		// offered to store as a login.  Nothing needs to read these back: they
		// are written once and used server-side, so the client only has to
		// know whether to say "(set)" or "(not set)".
		//
		// saveServerSettings already writes only the keys the caller sent, so
		// "unchanged" needs no sentinel: the client simply omits the field it
		// did not touch.  Clearing one stays possible by sending it empty.
		bool has_token = !store_.get_setting("discogs_token").empty();
		bool has_tmdb  = !store_.get_setting("tmdb_key").empty();
		std::string body = subsonic_ok_json([&](nlohmann::json& r) {
			r["serverSettings"]["discogsTokenSet"] = has_token;
			r["serverSettings"]["tmdbKeySet"]      = has_tmdb;
			});
		res.set_content(body, "application/json");
		});

	server_.Get("/rest/saveServerSettings.view", [this](const httplib::Request& req,
	                                                     httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k); return it != req.params.end() ? it->second : "";
			};
		auto ri = store_.get_user(qp("u"));
		if (!ri || !ri->is_admin) { err(50, "User is not authorized for this operation."); return; }

		// Only settings the caller actually sent are written. With one field
		// that distinction did not exist; with two, a client saving just one
		// of them would otherwise blank the other.
		if (req.params.count("discogsToken"))
			store_.set_setting("discogs_token", qp("discogsToken"));
		if (req.params.count("tmdbKey"))
			store_.set_setting("tmdb_key", qp("tmdbKey"));
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getMusicFolders — returns the configured music root(s).
	server_.Get("/rest/getMusicFolders.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		auto folders = store_.get_music_folders();
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&folders](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& f : folders)
					// contentType is a gaindrive extension: "artists" or "categories".
					arr.push_back({{"id", sid(f.id)}, {"name", f.name},
					               {"contentType", f.type}});
				r["musicFolders"]["musicFolder"] = arr;
				});
		else
			body = subsonic_ok([&folders](XMLDocument& doc, XMLElement* root) {
				auto* mf = doc.NewElement("musicFolders");
				for (auto& f : folders) {
					auto* el = doc.NewElement("musicFolder");
					el->SetAttribute("id",   f.id);
					el->SetAttribute("name", f.name.c_str());
				el->SetAttribute("contentType", f.type.c_str());
					mf->InsertEndChild(el);
					}
				root->InsertEndChild(mf);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getIndexes — all artists grouped by first letter.
	server_.Get("/rest/getIndexes.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		std::string pu;
		if (!personal_scope(req, res, store_, use_json, pu)) return;
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		// contentType is a gaindrive extension: it narrows to a *kind* of
		// root, which musicFolderId cannot express because a kind may span
		// several roots. Absent means every root, mixed, which is what
		// third-party clients expect and get.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0),
			req.get_param_value("contentType"));

		auto buckets = index_buckets(artists, pu == MediaStore::PERSONAL_ALL_USERS);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&buckets](nlohmann::json& r) {
				nlohmann::json idx_arr = nlohmann::json::array();
				for (auto& [letter, vec] : buckets) {
					nlohmann::json artist_arr = nlohmann::json::array();
					for (auto* a : vec)
						artist_arr.push_back({{"id", sid(a->id)}, {"name", a->name},
						                      {"coverArt", sid(a->id)}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["indexes"] = {
					{"lastModified",    0},
					{"ignoredArticles", "The El La Los Las Le Les A An Die Das Ein Eine"},
					{"index",           idx_arr}
					};
				});
		else
			body = subsonic_ok([&buckets](XMLDocument& doc, XMLElement* root) {
				auto* indexes = doc.NewElement("indexes");
				indexes->SetAttribute("lastModified",    "0");
				indexes->SetAttribute("ignoredArticles",
					"The El La Los Las Le Les A An Die Das Ein Eine");
				for (auto& [letter, vec] : buckets) {
					auto* idx = doc.NewElement("index");
					idx->SetAttribute("name", letter.c_str());
					for (auto* a : vec) {
						auto* artist = doc.NewElement("artist");
						artist->SetAttribute("id",       a->id);
						artist->SetAttribute("name",     a->name.c_str());
						artist->SetAttribute("coverArt", a->id);
						idx->InsertEndChild(artist);
						}
					indexes->InsertEndChild(idx);
					}
				root->InsertEndChild(indexes);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtists — same artists as getIndexes but with albumCount per artist.
	server_.Get("/rest/getArtists.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		std::string pu;
		if (!personal_scope(req, res, store_, use_json, pu)) return;
		// musicFolderId restricts to one root; absent means all of them,
		// which is what every existing client sends.
		// contentType is a gaindrive extension: it narrows to a *kind* of
		// root, which musicFolderId cannot express because a kind may span
		// several roots. Absent means every root, mixed, which is what
		// third-party clients expect and get.
		auto artists = store_.get_artist_dirs(
			pu, to_int(req.get_param_value("musicFolderId"), 0),
			req.get_param_value("contentType"));

		auto buckets = index_buckets(artists, pu == MediaStore::PERSONAL_ALL_USERS);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&buckets](nlohmann::json& r) {
				nlohmann::json idx_arr = nlohmann::json::array();
				for (auto& [letter, vec] : buckets) {
					nlohmann::json artist_arr = nlohmann::json::array();
					for (auto* a : vec)
						artist_arr.push_back({{"id", sid(a->id)}, {"name", a->name},
						                      {"albumCount", a->album_count},
						                      {"coverArt",   sid(a->id)}});
					idx_arr.push_back({{"name", letter}, {"artist", artist_arr}});
					}
				r["artists"] = {
					{"lastModified",    0},
					{"ignoredArticles", "The El La Los Las Le Les A An Die Das Ein Eine"},
					{"index",           idx_arr}
					};
				});
		else
			body = subsonic_ok([&buckets](XMLDocument& doc, XMLElement* root) {
				auto* artists_el = doc.NewElement("artists");
				artists_el->SetAttribute("lastModified",    "0");
				artists_el->SetAttribute("ignoredArticles",
					"The El La Los Las Le Les A An Die Das Ein Eine");
				for (auto& [letter, vec] : buckets) {
					auto* idx = doc.NewElement("index");
					idx->SetAttribute("name", letter.c_str());
					for (auto* a : vec) {
						auto* artist = doc.NewElement("artist");
						artist->SetAttribute("id",         a->id);
						artist->SetAttribute("name",       a->name.c_str());
						artist->SetAttribute("albumCount", a->album_count);
						artist->SetAttribute("coverArt",   a->id);
						idx->InsertEndChild(artist);
						}
					artists_el->InsertEndChild(idx);
					}
				root->InsertEndChild(artists_el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtist — single artist with album list.
	server_.Get("/rest/getArtist.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		std::string user = req.params.find("u")->second;
		const int artist_fid = to_int(it->second, -1);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(artist_fid), use_json)) return;
		auto info = store_.get_artist(artist_fid, user);
		if (!info) { err(70, "Artist not found."); return; }

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& al : info->albums) {
					nlohmann::json entry = {
						{"id",        sid(al.id)},
						{"parent",    sid(al.parent_id)},
						{"artistId",  sid(al.parent_id)},
						{"isDir",     true},
						{"title",     al.title},
						{"name",      al.title},
						{"artist",    al.artist},
						{"songCount", al.song_count},
						{"duration",  al.duration},
						{"videoCount", al.video_count}
						};
					if (!al.created.empty()) entry["created"] = iso8601(al.created);
					if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
					if (al.year > 0)          entry["year"]     = al.year;
					if (!al.genre.empty())    entry["genre"]    = al.genre;
					if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
					arr.push_back(std::move(entry));
					}
				r["artist"] = {
					{"id",         sid(info->artist.id)},
					{"name",       info->artist.name},
					{"albumCount", info->artist.album_count},
					{"album",      arr}
					};
				});
		else
			body = subsonic_ok([&info](XMLDocument& doc, XMLElement* root) {
				auto* artist_el = doc.NewElement("artist");
				artist_el->SetAttribute("id",         info->artist.id);
				artist_el->SetAttribute("name",       info->artist.name.c_str());
				artist_el->SetAttribute("albumCount", info->artist.album_count);
				for (auto& al : info->albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",        al.id);
					el->SetAttribute("parent",    al.parent_id);
					el->SetAttribute("isDir",     true);
					el->SetAttribute("title",     al.title.c_str());
					el->SetAttribute("name",      al.title.c_str());
					el->SetAttribute("artist",    al.artist.c_str());
					if (al.cover_art_id >= 0)
						el->SetAttribute("coverArt", al.cover_art_id);
					el->SetAttribute("songCount", al.song_count);
					el->SetAttribute("duration",  al.duration);
					el->SetAttribute("videoCount", al.video_count);
					if (!al.created.empty()) el->SetAttribute("created", iso8601(al.created).c_str());
					if (al.year > 0)         el->SetAttribute("year",    al.year);
					if (!al.genre.empty())   el->SetAttribute("genre",   al.genre.c_str());
					if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
					artist_el->InsertEndChild(el);
					}
				root->InsertEndChild(artist_el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getMusicDirectory — contents of a folder (album dirs or song files).
	server_.Get("/rest/getMusicDirectory.view", [this](const httplib::Request& req,
	                                                    httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		const int dir_id = to_int(it->second, -1);
		// Checked on the folder's own path, before the listing is built: the
		// uploads root is skipped by get_music_folders() but is still a
		// folders row, so without this getMusicDirectory on its id lists every
		// user's name, and on a batch id lists that user's library.
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(dir_id), use_json)) return;

		auto dir = store_.get_directory(dir_id, flat_multi_disc_);
		if (!dir) { err(70, "Directory not found."); return; }

		int mbr = request_max_bitrate(req, store_);
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&dir, mbr](nlohmann::json& r) {
				nlohmann::json children = nlohmann::json::array();
				for (auto& c : dir->children) {
					nlohmann::json child;
					if (c.is_dir) {
						child = {{"id",sid(c.id)},{"parent",sid(c.parent_id)},
						         {"isDir",true},
						         {"title",c.title},{"artist",c.artist},{"album",c.album}};
						if (c.cover_art_id >= 0) child["coverArt"] = sid(c.cover_art_id);
						if (c.year > 0)          child["year"]     = c.year;
						} else {
						child = song_entry_json(c, mbr);
						}
					children.push_back(child);
					}
				r["directory"] = {
					{"id",    sid(dir->id)},
					{"name",  dir->name},
					{"child", children}
					};
				if (dir->parent_id >= 0)    r["directory"]["parent"]   = sid(dir->parent_id);
				if (dir->cover_art_id >= 0) r["directory"]["coverArt"] = sid(dir->cover_art_id);
				});
		else
			body = subsonic_ok([&dir, mbr](XMLDocument& doc, XMLElement* root) {
				auto* directory = doc.NewElement("directory");
				directory->SetAttribute("id",   dir->id);
				directory->SetAttribute("name", dir->name.c_str());
				if (dir->parent_id >= 0)
					directory->SetAttribute("parent", dir->parent_id);
				if (dir->cover_art_id >= 0)
					directory->SetAttribute("coverArt", dir->cover_art_id);

				for (auto& c : dir->children) {
					XMLElement* child;
					if (c.is_dir) {
						child = doc.NewElement("child");
						child->SetAttribute("id",     c.id);
						child->SetAttribute("parent", c.parent_id);
						child->SetAttribute("isDir",  true);
						child->SetAttribute("title",  c.title.c_str());
						child->SetAttribute("artist", c.artist.c_str());
						child->SetAttribute("album",  c.album.c_str());
						if (c.cover_art_id >= 0)
							child->SetAttribute("coverArt", c.cover_art_id);
						if (c.year > 0)
							child->SetAttribute("year", c.year);
						} else {
						child = song_entry_xml(doc, c, "child", mbr);
						}
					directory->InsertEndChild(child);
					}

				root->InsertEndChild(directory);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbumList / getAlbumList2 — both use the same folder-based logic.
	server_.Get("/rest/getAlbumList.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, "albumList");
		});
	server_.Get("/rest/getAlbumList2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_list(req, res, store_, "albumList2");
		});

	// getRecentSongs — gaindrive extension; not in the OpenSubsonic spec.
	// Returns songs ordered by most recently played (per-user play_counts).
	server_.Get("/rest/getRecentSongs.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		std::string user = qp("u");
		int size   = std::min(500, std::max(1, to_int(qp("size", "50"), 50)));
		int offset = std::clamp(to_int(qp("offset", "0"), 0), 0, MAX_LIST_OFFSET);

		auto entries = store_.get_recent_songs(user, size, offset);
		int max_br   = request_max_bitrate(req, store_);

		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&entries, max_br](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& e : entries) {
					auto s = song_entry_json(e.song, max_br);
					if (!e.last_played.empty()) s["lastPlayed"] = e.last_played;
					arr.push_back(std::move(s));
					}
				r["recentSongs"] = {{"song", arr}};
				});
		else
			body = subsonic_ok([&entries, max_br](XMLDocument& doc, XMLElement* root) {
				auto* rs = doc.NewElement("recentSongs");
				for (const auto& e : entries) {
					auto* el = song_entry_xml(doc, e.song, "song", max_br);
					if (!e.last_played.empty())
						el->SetAttribute("lastPlayed", e.last_played.c_str());
					rs->InsertEndChild(el);
					}
				root->InsertEndChild(rs);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getArtistInfo / getArtistInfo2 — answered from artist_info_cache, with the
	// lookup itself queued onto the portrait resolver. Both endpoints share
	// identical logic; only the response key name differs.
	server_.Get("/rest/getArtistInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, "artistInfo");
		});
	server_.Get("/rest/getArtistInfo2.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_artist_info(req, res, "artistInfo2");
		});

	// getCoverArt — serve a cover image, optionally scaled.
	server_.Get("/rest/getCoverArt.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		int folder_id = to_int(it->second, -1);
		std::string rel_path = store_.get_cover_path(folder_id);

		// A cover is as personal as the item it belongs to, and every stored
		// path begins with its root's name — so `rel_path` answers the question
		// on its own whenever there is one, whether it names an image, a loose
		// file's sidecar or a video the art was extracted from.
		//
		// **Deliberately not an extra get_folder_path() here.** This is the
		// album-grid hot path — a client asks for every cover at once and each
		// query takes db_mutex_, which a scan holds across whole album
		// transactions — so the only branch that costs a lookup is the one
		// with no cover_path at all, an artist folder, which the handler is
		// about to look up anyway.
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          rel_path.empty()
		                              ? store_.get_folder_path(folder_id)
		                              : rel_path,
		                          fmt_of(req) == "json")) return;

		namespace fs = std::filesystem;

		// The size the client asked for, rounded to the ladder — see
		// CoverArtCache::ladder_size. 0 means "serve the source".
		auto size_it = req.params.find("size");
		int  ladder  = size_it != req.params.end()
		    ? CoverArtCache::ladder_size(to_int(size_it->second, 200)) : 0;

		// Optional index: 0 (default) = main cover, 1+ = extra images sorted.
		auto idx_it = req.params.find("index");

		// Resolve the id into something with bytes, a key and a stamp. The
		// three kinds of art differ only here; everything below is shared, so
		// a video poster and an artist portrait are scaled, cached and
		// revalidated by exactly the code that handles an ordinary cover.
		CoverArtCache::Source src;
		std::string           orig_mime;
		int64_t               orig_len = 0;

		if (rel_path.empty()) {
			// An artist folder. Never fall back to album cover art: an artist
			// showing one of their album covers as a portrait looks like a bug
			// in whichever client drew it.
			//
			// This reads the database and nothing else. It used to run the
			// whole MusicBrainz -> Wikidata -> Wikipedia -> TheAudioDB ->
			// Discogs chain right here, in the request thread, pacing sleeps
			// included — so a client showing a grid of artists could hold the
			// entire HTTP pool in network waits.
			std::string fpath = store_.get_folder_path(folder_id);
			std::string name  = store_.get_folder_name(folder_id);
			if (fpath.empty() || name.empty()) {
				res.status = 404;
				return;
				}
			// **An album is not an artist, even when it has no cover.**  This
			// branch is reached whenever get_cover_path() came back empty, and
			// an album folder with no image at all lands here as readily as a
			// real artist folder does — so a coverless album has always been
			// answered with an artist portrait and pushed onto the MusicBrainz
			// queue under its own title.  A pre-existing bug, but one this had
			// to grow a guard for: a loose file is its own album now, so every
			// loose track without a sidecar image would ask MusicBrainz about
			// "Track.mp4".
			//
			// The extra lookup is affordable because it is inside the already
			// cold empty-cover branch, which does two of its own; the album
			// grid's hot path never reaches it.
			if (store_.folder_is_album(folder_id)) {
				res.status = 404;
				return;
				}
			auto state = store_.get_artist_art_state(fpath);

			if (!state || state->status == "error") {
				// Not resolved yet, or the network failed last time. Push this
				// artist to the front of the queue — what somebody is looking
				// at beats the alphabet — and say so at once.
				//
				// no-store, not no-cache: a client must be able to re-ask in a
				// few seconds and get the picture. A cached 404 is how "the
				// portraits never appear until you restart the browser" would
				// happen.
				if (!store_.is_category_folder(folder_id))
					portrait_request_front(folder_id, fpath, name);
				res.set_header("Cache-Control", "no-store");
				res.status = 404;
				return;
				}
			if (state->status != "ok") {
				// "none": every provider was asked and none had a picture.
				// Cacheable on purpose — a client retrying on a timer would
				// otherwise poll for ever over an artist nobody has a portrait
				// of, and on a real library that is many of them.
				//
				// An hour, not a day. This is the one answer here a client is
				// allowed to keep without asking, so its lifetime is also how
				// long a *wrong* "none" survives on the device after the
				// server has stopped believing it — and a wrong one is
				// entirely possible, since it is what a provider outage looks
				// like if anything upstream mistakes silence for a verdict.
				// Deleting the row server-side cannot reach a cache on a
				// phone. An hour still silences any retry loop; a day meant a
				// correction took a day to arrive.
				res.set_header("Cache-Control", "public, max-age=3600");
				res.status = 404;
				return;
				}

			auto art = store_.get_artist_art(fpath);
			if (!art) {
				res.status = 404;
				return;
				}
			src.kind  = CoverArtCache::Source::Kind::Blob;
			src.key   = fpath;
			src.stamp = art->fetched_at;
			orig_len  = static_cast<int64_t>(art->bytes.size());
			orig_mime = art->mime.empty() ? "image/jpeg" : art->mime;
			src.bytes = std::move(art->bytes);
			}
		else {
			if (idx_it != req.params.end()) {
				int idx = to_int(idx_it->second, 0);
				if (idx > 0) {
					auto extras = store_.get_extra_image_paths(folder_id);
					if (idx - 1 >= static_cast<int>(extras.size())) {
						res.status = 404;
						return;
						}
					rel_path = extras[idx - 1];
					}
				}

			// A cover_path naming a *media* file means the art was manufactured
			// from that file and lives in the video_art table — see videoart.hh.
			// Storing the media path rather than inventing a marker is what lets
			// every cover-art query, and the whole web client, stay unchanged.
			if (is_video_ext(ext_of(rel_path))) {
				auto art = store_.get_video_art(rel_path);
				if (!art) {
					res.status = 404;
					return;
					}
				src.kind  = CoverArtCache::Source::Kind::Blob;
				src.key   = rel_path;
				src.stamp = art->file_modified;
				orig_len  = static_cast<int64_t>(art->bytes.size());
				orig_mime = art->mime.empty() ? "image/jpeg" : art->mime;
				src.bytes = std::move(art->bytes);
				}
			else {
				// Compose absolute filesystem path for the actual file open.
				std::string path = store_.abs_path(rel_path);
				if (!store_.path_is_within_root(path)) {
					std::cout << stamp()
					          << "getCoverArt: refusing path outside every root: "
					          << path << std::endl;
					res.status = 403;
					return;
					}
				std::error_code mec, sec;
				auto  mtime = fs::last_write_time(path, mec);
				auto  fsize = fs::file_size(path, sec);
				// Each stat gets its own error_code, and both are resolved before
				// they are read: sharing one would read and write it within a
				// single unsequenced expression.
				src.kind     = CoverArtCache::Source::Kind::File;
				src.key      = rel_path;
				src.abs_path = path;
				src.stamp    = mec ? 0 : static_cast<int64_t>(
					mtime.time_since_epoch().count());
				orig_len     = sec ? 0 : static_cast<int64_t>(fsize);
				}
			}

		// Revalidate rather than trust the cache.  A cover URL is identified
		// only by `id`, and folders.id is a rowid that is NOT stable across a
		// rescan — which is exactly why stars and playlists key on paths
		// instead.  Without a validator a browser caches the image
		// heuristically and indefinitely, so after a rebuild reassigns ids it
		// keeps showing the previous album's art with no way to notice.
		// "no-cache" means "keep it, but check first", so this stays fast.
		//
		// The "t2-" is a scheme marker, and it is not decoration: a client
		// holds ETags for bytes made under the *previous* scaling rule, and
		// the same URL now answers with different ones. Without a marker a
		// browser would 304 its way into keeping the old thumbnail for ever.
		// Bump it if the encoder, the quality, the ladder or the fit rule
		// changes again. t1- was ffmpeg's output, then stb fitting the long
		// edge; t2- is stb fitting the short one.
		//
		// This is the client half. The server holds its own copies in
		// cover_thumbs, keyed on nothing the fit rule moves, so they are
		// dropped by the cache-scheme user_version in MediaStore instead.
		//
		// The *ladder* value goes in, not what the client asked for: two
		// requests that round to the same rung are the same bytes and must
		// share a validator.
		std::string etag = "\"t2-" + std::to_string(src.stamp)
		    + "-" + std::to_string(orig_len)
		    + "-" + (ladder > 0 ? std::to_string(ladder) : std::string("full"))
		    + "-" + std::to_string(idx_it != req.params.end()
		                           ? to_int(idx_it->second, 0) : 0) + "\"";
		res.set_header("Cache-Control", "no-cache");
		res.set_header("ETag", etag);
		if (req.get_header_value("If-None-Match") == etag) {
			res.status = 304;
			return;
			}

		if (ladder > 0) {
			// nullopt means the source is already small enough, or nothing
			// could decode it. Both fall through to serving it whole, and both
			// were recorded, so neither is worked out again.
			if (auto out = cover_cache_.scaled(src, ladder)) {
				res.set_content(out->bytes, out->mime);
				return;
				}
			}

		if (src.kind == CoverArtCache::Source::Kind::Blob) {
			res.set_content(src.bytes, orig_mime);
			return;
			}

		// Serve the full-size image directly. This keeps the length-carrying
		// content provider rather than buffering: a full-size cover can be
		// twenty megabytes, and no UI asks for one — only the lightbox does.
		//
		// The MIME comes from the magic bytes. It used to be image/jpeg
		// whatever was on disk, which every browser renders anyway and every
		// cache and proxy in between believes.
		std::string head;
		{
		std::ifstream hf(src.abs_path, std::ios::binary);
		char          hb[12];
		if (hf) {
			hf.read(hb, sizeof(hb));
			head.assign(hb, static_cast<size_t>(hf.gcount()));
			}
		}
		std::string mime = imagescale::sniff_mime(head);
		if (mime.empty()) mime = "application/octet-stream";

		std::string path = src.abs_path;
		res.set_content_provider(
			static_cast<size_t>(orig_len), mime,
			[path](size_t offset, size_t length, httplib::DataSink& sink) {
				std::ifstream f(path, std::ios::binary);
				if (!f) return false;
				f.seekg(static_cast<std::streamoff>(offset));
				char   buf[65536];
				size_t remaining = length;
				while (remaining > 0) {
					auto to_read = static_cast<std::streamsize>(
					    std::min(remaining, sizeof(buf)));
					f.read(buf, to_read);
					auto n = static_cast<size_t>(f.gcount());
					if (n == 0) break;
					if (!sink.write(buf, n)) return false;
					remaining -= n;
					}
				return true;
				});
		});

	// getAlbumTexts — list .txt files in an album folder.
	server_.Get("/rest/getAlbumTexts.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		std::string folder_rel = store_.get_folder_path(to_int(it->second, -1));
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, fmt_of(req) == "json")) return;
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}
		std::string folder = store_.abs_path(folder_rel);
		if (!store_.path_is_within_root(folder)) {
			std::cout << stamp() << "getAlbumTexts: refusing path outside every root: "
			          << folder << std::endl;
			res.status = 403;
			return;
			}

		// Well-known utility files that are not human-readable liner notes.
		static const std::set<std::string> excluded = {
			"fingerprints.txt",
			};
		// Chapter sidecars are matched by suffix rather than by name, since
		// the stem is the video's. The constant is MediaStore's so there is
		// one spelling of it: this listing and sidecar_chapters_path() are the
		// two places that know the name, and the day they disagreed chapter
		// files would silently start appearing as prose.
		//
		// Note the ".part" a save writes first needs no entry -- it fails the
		// ".txt" extension test below. That is luck rather than design, so do
		// not lean on it if that test ever loosens.
		const std::string chapters_suffix(MediaStore::CHAPTERS_SUFFIX);

		namespace fs = std::filesystem;
		nlohmann::json files = nlohmann::json::array();
		std::error_code fec;
		if (fs::is_regular_file(folder, fec)) {
			// A file-album: its liner notes are the sidecar named after it,
			// <stem>.txt, and there is no directory to list.  Handled
			// explicitly rather than left to the directory_iterator below
			// throwing into the catch — which returns the right answer for the
			// wrong reason, and returns *nothing* where a real note exists.
			//
			// It cannot collide with the chapter sidecar: that is
			// <stem>.chapters.txt, a different filename, which is the whole
			// point of sidecar_chapters_path() not being replace_extension().
			std::string note = MediaStore::sidecar_text_path(folder);
			if (fs::exists(note, fec)) {
				// utf8_clean before json: a filename is arbitrary bytes on
				// Linux, and one invalid sequence makes dump() throw — a
				// permanent 500 for this album's listing. A name the clean
				// changes cannot be fetched back through getAlbumText anyway,
				// so it is dropped rather than listed under a lie.
				const std::string fname = fs::path(note).filename().string();
				const std::string clean = utf8_clean(fname, 255);
				if (clean == fname)
					files.push_back({{"name", clean}});
				}
			}
		else try {
			for (auto& entry : fs::directory_iterator(folder)) {
				if (!entry.is_regular_file() || entry.path().extension() != ".txt") continue;
				const std::string fname = entry.path().filename().string();
				if (excluded.count(fname)) continue;
				if (fname.size() > chapters_suffix.size()
				        && fname.compare(fname.size() - chapters_suffix.size(),
				                         chapters_suffix.size(),
				                         chapters_suffix) == 0)
					continue;
				// Same rule as the file-album branch above.
				const std::string clean = utf8_clean(fname, 255);
				if (clean != fname) continue;
				files.push_back({{"name", clean}});
				}
			}
		catch (...) {}

		// Sort alphabetically so the order is stable.
		std::sort(files.begin(), files.end(), [](const nlohmann::json& a, const nlohmann::json& b){
			return a["name"].get<std::string>() < b["name"].get<std::string>();
			});

		res.set_content(subsonic_ok_json([&files](nlohmann::json& r) {
			r["albumTexts"] = {{"textFile", files}};
			}), "application/json");
		});

	// getAlbumImages — return total image count for an album folder (cover + extras).
	server_.Get("/rest/getAlbumImages.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error_json(10, "Required parameter missing: id."),
			                "application/json");
			return;
			}

		const int images_fid = to_int(it->second, -1);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(images_fid),
		                          fmt_of(req) == "json")) return;
		int count = store_.get_image_count(images_fid);
		res.set_content(subsonic_ok_json([count](nlohmann::json& r) {
			r["albumImages"] = {{"count", count}};
			}), "application/json");
		});

	// getAlbumText — serve a single .txt file from an album folder.
	server_.Get("/rest/getAlbumText.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto id_it   = req.params.find("id");
		auto name_it = req.params.find("name");
		if (id_it == req.params.end() || name_it == req.params.end()) {
			res.status = 400;
			return;
			}

		// Reject any path traversal attempts. The NUL test is not
		// theoretical: a query parameter arrives percent-decoded, so
		// `cover.jpg%00.txt` passes the suffix test as a C++ string and is
		// then truncated at the NUL by every c_str() the filesystem layer
		// takes — the file opened is cover.jpg, served as text/plain.
		const std::string& name = name_it->second;
		if (name.find('/') != std::string::npos  ||
		    name.find('\\') != std::string::npos ||
		    name.find('\0') != std::string::npos ||
		    name.find("..") != std::string::npos ||
		    name.size() < 5 ||
		    name.substr(name.size() - 4) != ".txt") {
			res.status = 400;
			return;
			}

		std::string folder_rel = store_.get_folder_path(to_int(id_it->second, -1));
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, fmt_of(req) == "json")) return;
		if (folder_rel.empty()) {
			res.status = 404;
			return;
			}

		namespace fs = std::filesystem;
		// A file-album has no directory to compose against, so the note is
		// taken from beside the media file — and `name` has to *be* that
		// sidecar's filename.  Composing from the parent directory instead
		// would let this endpoint read any album's notes out of the section
		// the file happens to sit in, which is a different album's content.
		std::string album_abs = store_.abs_path(folder_rel);
		std::error_code fec;
		fs::path full;
		if (fs::is_regular_file(album_abs, fec)) {
			fs::path note = MediaStore::sidecar_text_path(album_abs);
			if (note.filename().string() != name) {
				res.status = 404;
				return;
				}
			full = note;
			}
		else
			full = fs::path(album_abs) / name;
		if (!store_.path_is_within_root(full)) {
			std::cout << stamp() << "getAlbumText: refusing path outside every root: "
			          << full.string() << std::endl;
			res.status = 403;
			return;
			}
		// Bounded before it is read: liner notes are kilobytes, and the file
		// is whatever an uploader put beside the album — an 8 GiB .txt inside
		// an archive would otherwise become an 8 GiB allocation per request.
		std::error_code sec;
		const auto fsize = fs::file_size(full, sec);
		if (sec) { res.status = 404; return; }
		if (fsize > MAX_SMALL_BODY_BYTES) {
			res.status = 413;
			return;
			}

		std::ifstream f(full);
		if (!f) {
			res.status = 404;
			return;
			}

		std::string content((std::istreambuf_iterator<char>(f)),
		                     std::istreambuf_iterator<char>());
		res.set_content(content, "text/plain; charset=utf-8");
		});

	// savePlayQueue — persist the client's current queue and playback position.
	server_.Get("/rest/savePlayQueue.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		// Collect all song IDs — the parameter may be repeated. Resolve
		// each to its filesystem path (the durable key the user-state DB
		// stores). Unresolvable ids are dropped silently.
		std::vector<std::string> paths;
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(to_int(it->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					paths.push_back(*p);
			}

		int         current_id   = to_int(qp("current", "0"), 0);
		std::string current_path = "";
		if (current_id != 0) {
			if (auto p = store_.song_path_by_id(current_id))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					current_path = *p;
			}
		int64_t offset_ms  = to_int64(qp("position", "0"), 0);
		std::string client = qp("c");
		std::string user   = qp("u");

		store_.save_play_queue(user, paths, current_path, offset_ms, client);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlayQueue — retrieve the user's saved play queue and position.
	server_.Get("/rest/getPlayQueue.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto pq = store_.get_play_queue(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&pq, &user, mbr](nlohmann::json& r) {
				if (!pq) { r["playQueue"] = nlohmann::json::object(); return; }
				nlohmann::json entries = nlohmann::json::array();
				for (auto& s : pq->songs)
					entries.push_back(song_entry_json(s, mbr));
				r["playQueue"] = {
					{"current",    pq->current_id},
					{"position",   pq->offset_ms},
					{"username",   user},
					{"changed",    iso8601(pq->changed)},
					{"changedBy",  pq->client},
					{"entry",      entries}
					};
				});
		else
			body = subsonic_ok([&pq, &user, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("playQueue");
				if (pq) {
					el->SetAttribute("current",   pq->current_id);
					el->SetAttribute("position",  (int64_t)pq->offset_ms);
					el->SetAttribute("username",  user.c_str());
					if (!pq->changed.empty())
						el->SetAttribute("changed",   iso8601(pq->changed).c_str());
					if (!pq->client.empty())
						el->SetAttribute("changedBy", pq->client.c_str());
					for (auto& s : pq->songs)
						el->InsertEndChild(song_entry_xml(doc, s, "entry", mbr));
					}
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// createBookmark — mark a playback position within a song.
	// scrobble — record a play (submission=true) or now-playing event (submission=false).
	server_.Get("/rest/scrobble.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		std::string user = qp("u");
		std::string client = qp("c");
		// submission defaults to true per the Subsonic spec.
		bool submission = (qp("submission", "true") != "false");

		// The read-permission filter on every id: these client-state writes
		// come back out through getStarred/getPlaylist/getPlayQueue with the
		// full song entry, so an id resolved here without the check is a
		// metadata read of somebody else's uploads by integer-guessing.
		// Foreign ids are dropped like unresolvable ones rather than failing
		// the request, matching how a deleted id already behaves.
		auto range = req.params.equal_range("id");
		for (auto it = range.first; it != range.second; ++it) {
			if (auto p = store_.song_path_by_id(to_int(it->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					store_.scrobble(user, *p, submission, client);
			}

		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	server_.Get("/rest/createBookmark.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}

		int     song_id     = to_int(it->second, -1);
		int64_t position_ms = to_int64(qp("position", "0"), 0);
		std::string comment = qp("comment");
		std::string user    = qp("u");

		auto song_path = store_.song_path_by_id(song_id);
		if (!song_path
		    || !item_read_allowed(req, store_, uploads_root_name_, *song_path)) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}
		store_.create_bookmark(user, *song_path, position_ms, comment);
		bool use_json = (fmt_of(req) == "json");
		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// stream — serve audio file directly or transcode via ffmpeg.
	// In cast mode: redirect playback to the Chromecast and return 204 to the
	// calling client.  The Chromecast authenticates its own request with a
	// castToken query parameter instead of normal credentials.
	server_.Get("/rest/stream.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(subsonic_error(10, "Required parameter missing: id."),
			                "application/xml");
			return;
			}
		// The id is read before the token is checked, because the token is
		// scoped to one song: it authorises *this* id or nothing.
		const int req_song_id = to_int(it->second, -1);
		auto tok_it = req.params.find("castToken");
		bool cast_authed = tok_it != req.params.end()
		                && cast_manager_.valid_token(tok_it->second, req_song_id);
		if (!cast_authed && !check_auth(req, res, store_)) return;

		auto song = store_.get_song(req_song_id);
		if (!song) {
			res.set_content(subsonic_error(70, "Song not found."), "application/xml");
			return;
			}

		// A cast stream carries no user, so the token has to be the authority
		// for it — CastManager::valid_token() has already bound it to this
		// exact song id. For everyone else, another user's uploads are not
		// readable by id.
		if (!cast_authed
		    && !check_item_read_perm(req, res, store_, uploads_root_name_,
		                             song->path, fmt_of(req) == "json")) return;

		// Compose-and-validate the absolute song path once. Streamer reads from
		// it (via std::ifstream and ffmpeg argv) — refuse anything outside
		// the configured roots before handing it off.
		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "stream: refusing path outside every root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		// If cast mode is active and the caller is not the Chromecast itself,
		// instruct the Chromecast to fetch the stream and return 204 here.
		//
		// Only for the client that *owns* the session. Without that test this
		// is a process-global redirect: every stream request in the server —
		// the phone, a third-party client, curl, a different account entirely
		// — was answered 204 and pushed onto whatever receiver anyone had most
		// recently picked. A client that sent no castController is never the
		// owner, so it simply plays locally, which is what every client that
		// does not drive the server's cast endpoints wants.
		//
		// The owner can decline it with castRedirect=false, and one caller
		// needs to: when the receiver has no screen it is sent the film's
		// *soundtrack*, and the web client keeps the picture, muted and in
		// step with it.  That request is not the client about to play the
		// track a second time — it is the other half of one playback — and
		// answering it 204 both loses the picture and, through the
		// cast_load_song() below, re-issues the LOAD as a side effect.
		if (cast_manager_.active() && !cast_authed
		    && req.get_param_value("castRedirect") != "false"
		    && cast_owned_by(req)) {
			// Native seek: the URL serves the full file, and the LOAD message
			// tells the receiver where to seek.  No timeOffset in the URL.
			auto to_it = req.params.find("timeOffset");
			float cast_offset = to_it != req.params.end()
			    ? to_float(to_it->second, 0.0f) : 0.0f;
			// No caption: a client redirected here never asked for one, and
			// the picker sends its choice through castLoad.
			cast_load_song(req, *song, to_int(it->second, -1), cast_offset, 0);
			res.status = 204;
			return;
			}

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};

		int         max_bitrate = to_int(qp("maxBitRate"), 0);
		std::string format      = qp("format");
		// Reject a format we have no encoder for here, where the Subsonic error
		// helpers live.  Letting it reach ffmpeg produced a 200 with an empty
		// body, which every client reports as a corrupt file rather than as a
		// bad request.
		if (!format.empty() && format != "raw") {
			auto t = target_for(format);
			if (!t || t->encoder.empty()) {
				bool as_json = fmt_of(req) == "json";
				std::string msg = "Unsupported format: " + format + ".";
				res.set_content(as_json ? subsonic_error_json(10, msg.c_str())
				                        : subsonic_error(10, msg.c_str()),
				                as_json ? "application/json" : "application/xml");
				return;
				}
			}

		// Enforce the user account's max_bitrate as a ceiling (0 = unlimited).
		// Cast requests authenticate via token and have no 'u' param; skip for those.
		//
		// Skipped for video, and that exemption is load-bearing rather than a
		// policy preference.  Any non-zero max_bitrate sets `constrained` in
		// serve_video and disqualifies both the direct and remux tiers — while
		// nativeSeek is a pure function of the codec pair and never sees it.  A
		// capped account would therefore be told every video is Range-seekable
		// and handed a chunked stream with Accept-Ranges: none, and would have
		// H.264/AAC MP4s re-encoded that could have been served off disk
		// untouched.  Subsonic defines maxBitRate as an audio ceiling anyway,
		// and a client that really wants a smaller picture still says so with
		// size= or maxBitRate=, which constrains exactly as before.
		//
		// An audio-only request is *not* a video request — it produces an
		// ordinary audio transcode through the ordinary audio path — so the
		// ceiling applies to it exactly as it does to a music track.  That is
		// why the format is parsed above rather than below: the ceiling now
		// depends on it.
		bool audio_only = audio_only_request(song->is_video, format,
		                                     song->audio_codec);
		if (!cast_authed && (!song->is_video || audio_only)) {
			int acct_max = request_max_bitrate(req, store_);
			if (acct_max > 0 && (max_bitrate == 0 || max_bitrate > acct_max))
				max_bitrate = acct_max;
			}
		// The Chromecast sometimes probes the stream URL with timeOffset stripped.
		// Always use the authoritative offset stored at castLoad time for cast
		// requests so the probe and the real request both start at the right position.
		int         time_offset = cast_authed
		    ? static_cast<int>(last_cast_offset_)
		    : to_int(qp("timeOffset"), 0);

		// Chromecast metadata probes arrive as Range requests against the stream URL.
		// For seeked streams (time_offset > 0) these would otherwise hit serve_transcoded
		// which returns chunked output with no Content-Length, making it impossible for
		// the receiver to read the format's duration/seek tables.  Force time_offset=0
		// so these Range requests are served directly from the raw file.
		if (cast_authed && !req.get_header_value("Range").empty() && time_offset > 0)
			time_offset = 0;

		// Seeked-stream probe: the Chromecast strips timeOffset from its probe
		// request.  We must not return a full transcoded stream (blasts megabytes
		// at LAN speed while throttle is suppressed during BUFFERING), but we also
		// must not return an empty body (Content-Length: 0 makes the receiver treat
		// the track as finished and abort the real seeked request).  Serve a small
		// slice of the raw file from t=0 — enough for the receiver to validate the
		// URL, well within the pre-buffer window (prebuf ≈ 30 s of audio).
		if (cast_authed
		        && req.params.find("timeOffset") == req.params.end()
		        && last_cast_offset_ > 0.0f
		        && req.get_header_value("Range").empty()) {
			std::cout << stamp() << "cast probe: id=" << log_safe(it->second, 64)
			          << " offset=" << last_cast_offset_ << std::endl;
			auto probe_si = streamer_song(*song, song_abs,
			                              std::min(song->file_size,
			                                       (int64_t)32768));
			// No VideoOptions, deliberately rather than by omission: this call
			// stops before it, so the one path that must never honour a client's
			// container declaration gets the empty set by construction.
			Streamer::serve(req, res, probe_si, transcode_cache_, 0, "", 0, true, {});
			return;
			}

		if (cast_authed) {
			// Log Range header so we can see what the Cast receiver is requesting.
			auto range = req.get_header_value("Range");
			std::cout << stamp() << "cast stream: id=" << log_safe(it->second, 64)
			          << " size=" << song->file_size
			          << " range=[" << (range.empty() ? "none" : range) << "]"
			          << std::endl;
			}

		auto si = streamer_song(*song, song_abs);

		// For Cast streams, pass a callback that returns the receiver's current
		// playback position from the cached status (updated every ~0.5 s by the
		// web client's poll).  The streamer uses this to keep the buffer at a
		// stable level without relying on any device-specific buffer size.
		std::function<float()> get_pos;
		if (cast_authed) {
			int gen = cast_manager_.load_generation();
			get_pos = [this, gen]{
				// Return -2 when a newer stream has started — the streamer treats
				// this as a stop signal so the old thread exits promptly.
				if (cast_manager_.load_generation() != gen) return CAST_POS_STOP;
				auto s = cast_manager_.get_status();
				// BUFFERING means "seeking to this position", not "played up to here".
				// Return CAST_POS_BUFFERING to suppress throttle until playback starts.
				if (s.player_state != "PLAYING") return CAST_POS_BUFFERING;
				return s.current_time;
				};
			}

		if (!cast_authed)
			std::cout << stamp() << "stream: id=" << log_safe(it->second, 64)
			          << " codec=" << song->codec
			          << " size=" << song->file_size
			          << " duration=" << song->duration
			          << (time_offset > 0 ? " offset=" + std::to_string(time_offset) : "")
			          << (!format.empty() ? " fmt=" + format : "")
			          << std::endl;
		bool estimate_length = qp("estimateContentLength") == "true";
		// Video-only per the spec, and ignored for audio by Streamer::serve().
		// `duration` is what makes an HLS segment a segment: hls.m3u8 points
		// every segment back here with a timeOffset and a length.
		// Validated here rather than in Streamer, so the one caller that can be
		// reached from outside is the one that checks. Anything unparseable
		// becomes empty, which means "do not scale" — the same as omitting it.
		VideoOptions vopts{
			.size             = sane_video_size(qp("size")),
			.segment_duration = to_int(qp("duration"), 0),
			// Never for a cast token, and that is not caution.  A
			// server-driven cast URL is fetched by the *receiver*, which
			// declared none of this, and the LOAD it is playing announced a
			// contentType that cast_mime_for() computed before a byte was
			// served.  Honouring a capability the controller claimed would
			// send Matroska to a receiver told video/mp4, which refuses the
			// media outright and reads as a broken file.
			.client_containers = cast_authed
			    ? ClientContainers{}
			    : parse_client_containers(qp("playableContainers")),
			// Same exclusion and the same reason: a receiver fetches for
			// itself, and a stream with no Content-Length is the one thing it
			// must not be handed after a LOAD announced video/mp4.  The
			// literal "true" only, as pace and estimateContentLength read it.
			.start_immediately = !cast_authed
			                  && qp("startImmediately") == "true",
			};
		Streamer::serve(req, res, si, transcode_cache_, max_bitrate, format,
		                time_offset, cast_authed, std::move(get_pos),
		                estimate_length, vopts);
		});

	// download — the original file, never transcoded and never bitrate-capped.
	// The per-user max_bitrate is deliberately not consulted: "download" is
	// defined by the API as the original media data, and a capped download
	// would silently hand the user a different file than the one they asked
	// for.  Only song ids are supported; zipping a folder is out of scope.
	server_.Get("/rest/download.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		std::string song_abs = store_.abs_path(song->path);
		if (!store_.path_is_within_root(song_abs)) {
			std::cout << stamp() << "download: refusing path outside every root: "
			          << song_abs << std::endl;
			res.status = 403;
			return;
			}

		std::string name = std::filesystem::path(song->path).filename().string();
		// Two spellings of the filename: a sanitised ASCII one for clients that
		// only read the bare parameter, and the RFC 5987 form for the rest.
		// A quote or newline left in the ASCII form would let a filename break
		// out of the header.
		std::string ascii;
		for (unsigned char c : name)
			ascii += (c < 0x20 || c == 0x7f || c == '"' || c == '\\'
			          || c >= 0x80) ? '_' : static_cast<char>(c);
		std::ostringstream enc;
		enc << std::hex << std::uppercase << std::setfill('0');
		for (unsigned char c : name) {
			if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
				enc << static_cast<char>(c);
			else
				enc << '%' << std::setw(2) << static_cast<int>(c);
			}
		res.set_header("Content-Disposition",
		               "attachment; filename=\"" + ascii + "\"; "
		               "filename*=UTF-8''" + enc.str());

		std::cout << stamp() << "download: id=" << log_safe(it->second, 64)
		          << " path=" << song->path
		          << " size=" << song->file_size << std::endl;

		auto si = streamer_song(*song, song_abs);
		Streamer::serve_raw(req, res, si);
		});

	// createPlaylist
	server_.Get("/rest/createPlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("name");
		// clean_name for the same reason updateSong's title gets it: this is
		// typed text on its way into a column both response formats serve.
		std::string name = it != req.params.end() ? clean_name(it->second)
		                                          : std::string();
		if (name.empty()) {
			err(10, "Required parameter missing: name.");
			return;
			}
		std::string user = req.params.find("u")->second;

		std::vector<std::string> song_paths;
		auto range = req.params.equal_range("songId");
		for (auto i = range.first; i != range.second; ++i) {
			if (auto p = store_.song_path_by_id(to_int(i->second, -1)))
				if (item_read_allowed(req, store_, uploads_root_name_, *p))
					song_paths.push_back(*p);
			}

		auto pl = store_.create_playlist(user, name, song_paths);
		std::string body = playlist_body(pl, use_json, request_max_bitrate(req, store_));
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlaylist
	server_.Get("/rest/getPlaylist.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		std::string user = req.params.find("u")->second;
		// One message for "no such playlist" and "not yours", the same
		// reasoning as deleteUpload's: the difference is only useful to
		// somebody walking the id space.
		auto pl = store_.get_playlist(to_int(it->second, -1), user);
		if (!pl) { err(70, "Playlist not found."); return; }

		std::string body = playlist_body(*pl, use_json, request_max_bitrate(req, store_));
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getPlaylists
	server_.Get("/rest/getPlaylists.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto pls = store_.get_playlists(user);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&pls](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& pl : pls)
					arr.push_back({
						{"id",        sid(pl.id)},
						{"name",      pl.name},
						{"comment",   pl.comment},
						{"owner",     pl.owner},
						{"public",    pl.is_public},
						{"songCount", pl.song_count},
						{"duration",  pl.duration},
						{"created",   iso8601(pl.created)},
						{"changed",   iso8601(pl.updated)}
						});
				r["playlists"] = {{"playlist", arr}};
				});
		else
			body = subsonic_ok([&pls](XMLDocument& doc, XMLElement* root) {
				auto* playlists = doc.NewElement("playlists");
				for (auto& pl : pls) {
					auto* el = doc.NewElement("playlist");
					el->SetAttribute("id",        pl.id);
					el->SetAttribute("name",      pl.name.c_str());
					el->SetAttribute("comment",   pl.comment.c_str());
					el->SetAttribute("owner",     pl.owner.c_str());
					el->SetAttribute("public",    pl.is_public);
					el->SetAttribute("songCount", pl.song_count);
					el->SetAttribute("duration",  pl.duration);
					el->SetAttribute("created",   iso8601(pl.created).c_str());
					el->SetAttribute("changed",   iso8601(pl.updated).c_str());
					playlists->InsertEndChild(el);
					}
				root->InsertEndChild(playlists);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getStarred / getStarred2 — identical content, only the response key differs.
	auto starred_handler = [this](const httplib::Request& req, httplib::Response& res,
	                               const char* key) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto sr = store_.get_starred(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&sr, key, mbr](nlohmann::json& r) {
				nlohmann::json artists = nlohmann::json::array();
				for (auto& a : sr.artists)
					artists.push_back({{"id", sid(a.id)}, {"name", a.name}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     sid(c.id)},
						{"parent", sid(c.parent_id)},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.album}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = sid(c.cover_art_id);
					albums.push_back(al);
					}

				nlohmann::json songs = nlohmann::json::array();
				for (auto& c : sr.songs)
					songs.push_back(song_entry_json(c, mbr));

				r[key] = {
					{"artist", artists},
					{"album",  albums},
					{"song",   songs}
					};
				});
		else
			body = subsonic_ok([&sr, key, mbr](XMLDocument& doc, XMLElement* root) {
				auto* starred = doc.NewElement(key);

				for (auto& a : sr.artists) {
					auto* el = doc.NewElement("artist");
					el->SetAttribute("id",   a.id);
					el->SetAttribute("name", a.name.c_str());
					starred->InsertEndChild(el);
					}

				for (auto& c : sr.albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",     c.id);
					el->SetAttribute("parent", c.parent_id);
					el->SetAttribute("isDir",  true);
					el->SetAttribute("title",  c.title.c_str());
					el->SetAttribute("artist", c.artist.c_str());
					el->SetAttribute("album",  c.album.c_str());
					if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
					starred->InsertEndChild(el);
					}

				for (auto& c : sr.songs)
					starred->InsertEndChild(song_entry_xml(doc, c, "song", mbr));

				root->InsertEndChild(starred);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		};
	server_.Get("/rest/getStarred.view",  [starred_handler](const httplib::Request& req,
	                                                         httplib::Response& res) {
		starred_handler(req, res, "starred");
		});
	server_.Get("/rest/getStarred2.view", [starred_handler](const httplib::Request& req,
	                                                          httplib::Response& res) {
		starred_handler(req, res, "starred2");
		});

	// updatePlaylist
	server_.Get("/rest/updatePlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("playlistId");
		if (it == req.params.end()) { err(10, "Required parameter missing: playlistId."); return; }
		int playlist_id = to_int(it->second, -1);
		std::string user = req.params.find("u")->second;

		std::optional<std::string> name, comment;
		std::optional<bool> is_public;
		if (req.params.count("name"))    name      = clean_name(req.params.find("name")->second);
		if (req.params.count("comment")) comment   = clean_prose(req.params.find("comment")->second,
		                                                         MAX_PROSE_BYTES);
		if (req.params.count("public"))  is_public = (req.params.find("public")->second == "true");

		std::vector<std::string> paths_to_add;
		std::vector<int> to_remove;
		for (auto& [k, v] : req.params) {
			if (k == "songIdToAdd") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (item_read_allowed(req, store_, uploads_root_name_, *p))
						paths_to_add.push_back(*p);
				}
			else if (k == "songIndexToRemove") to_remove.push_back(to_int(v, -1));
			}

		if (!store_.update_playlist(playlist_id, user, name, comment, is_public,
		                             paths_to_add, to_remove)) {
			err(70, "Playlist not found.");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// star
	server_.Get("/rest/star.view", [this](const httplib::Request& req,
	                                      httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		// item_read_allowed on every kind of id — see scrobble.
		auto readable = [&](const std::string& path) {
			return item_read_allowed(req, store_, uploads_root_name_, path);
			};
		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.add_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// unstar
	server_.Get("/rest/unstar.view", [this](const httplib::Request& req,
	                                        httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto readable = [&](const std::string& path) {
			return item_read_allowed(req, store_, uploads_root_name_, path);
			};
		for (auto& [k, v] : req.params) {
			if (k == "id") {
				if (auto p = store_.song_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, *p, "", "");
				}
			else if (k == "albumId") {
				if (auto p = store_.album_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, "", *p, "");
				}
			else if (k == "artistId") {
				if (auto p = store_.artist_folder_path_by_id(to_int(v, -1)))
					if (readable(*p)) store_.remove_star(user, "", "", *p);
				}
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbum — single album with its track list.
	server_.Get("/rest/getAlbum.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;

		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		std::string user = req.params.find("u")->second;
		const int album_fid = to_int(it->second, -1);
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          store_.get_folder_path(album_fid), use_json)) return;
		auto info = store_.get_album(album_fid, flat_multi_disc_, user);
		if (!info) { err(70, "Album not found."); return; }
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&info, mbr](nlohmann::json& r) {
				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : info->songs)
					songs.push_back(song_entry_json(s, mbr));
				auto& al = info->album;
				nlohmann::json entry = {
					{"id",        sid(al.id)},
					{"parent",    sid(al.parent_id)},
					{"artistId",  sid(al.parent_id)},
					{"name",      al.title},
					{"artist",    al.artist},
					{"songCount", al.song_count},
					{"duration",  al.duration},
					{"videoCount", al.video_count},
					{"song",      songs}
					};
				if (!al.created.empty()) entry["created"] = iso8601(al.created);
				if (al.cover_art_id >= 0) entry["coverArt"] = sid(al.cover_art_id);
				if (al.year > 0)          entry["year"]     = al.year;
				if (!al.genre.empty())    entry["genre"]    = al.genre;
				if (!al.starred.empty())  entry["starred"]  = iso8601(al.starred);
				r["album"] = std::move(entry);
				});
		else
			body = subsonic_ok([&info, mbr](XMLDocument& doc, XMLElement* root) {
				auto& al = info->album;
				auto* el = doc.NewElement("album");
				el->SetAttribute("id",        al.id);
				el->SetAttribute("parent",    al.parent_id);
				el->SetAttribute("name",      al.title.c_str());
				el->SetAttribute("artist",    al.artist.c_str());
				el->SetAttribute("songCount", al.song_count);
				el->SetAttribute("duration",  al.duration);
				el->SetAttribute("videoCount", al.video_count);
				if (al.cover_art_id >= 0) el->SetAttribute("coverArt", al.cover_art_id);
				if (!al.created.empty())  el->SetAttribute("created",  iso8601(al.created).c_str());
				if (al.year > 0)          el->SetAttribute("year",     al.year);
				if (!al.genre.empty())    el->SetAttribute("genre",    al.genre.c_str());
				if (!al.starred.empty())
					el->SetAttribute("starred", iso8601(al.starred).c_str());
				for (auto& s : info->songs)
					el->InsertEndChild(song_entry_xml(doc, s, "song", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getAlbumInfo / getAlbumInfo2 — MusicBrainz lookup, result cached in DB.
	// Both endpoints share identical logic; only the response key name differs.
	// An album id here *is* a folder id, so there is no id3 album to look up
	// separately and the older name answers the same question as the newer.
	server_.Get("/rest/getAlbumInfo.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, store_, uploads_root_name_, "albumInfo");
		});
	server_.Get("/rest/getAlbumInfo2.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		handle_album_info(req, res, store_, uploads_root_name_, "albumInfo2");
		});

	// getTopSongs — play-count tracking not implemented; return empty list.
	server_.Get("/rest/getTopSongs.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string body;
		if (use_json)
			body = subsonic_ok_json([](nlohmann::json& r) {
				r["topSongs"] = {{"song", nlohmann::json::array()}};
				});
		else
			body = subsonic_ok([](XMLDocument& doc, XMLElement* root) {
				root->InsertEndChild(doc.NewElement("topSongs"));
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getSong — full metadata for a single track.
	server_.Get("/rest/getSong.view", [this](const httplib::Request& req,
	                                         httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }

		auto song = store_.get_song_entry(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&song, mbr](nlohmann::json& r) {
				r["song"] = song_entry_json(*song, mbr);
				});
		else
			body = subsonic_ok([&song, mbr](XMLDocument& doc, XMLElement* root) {
				root->InsertEndChild(song_entry_xml(doc, *song, "song", mbr));
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getGenres — every distinct genre in the shared library, with counts.
	//
	// The tags are reported as they are, typos and "unknown" included: this
	// endpoint is the only view anyone gets of their own tagging, and a list
	// filtered down to the plausible entries would hide the very thing that
	// needs fixing. Only case and padding are folded away, which loses
	// nothing. See MediaStore::get_genres().
	server_.Get("/rest/getGenres.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto genres   = store_.get_genres();

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&genres](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& g : genres)
					arr.push_back({{"value",      g.name},
					               {"songCount",  g.song_count},
					               {"albumCount", g.album_count}});
				r["genres"] = {{ "genre", arr }};
				});
		else
			body = subsonic_ok([&genres](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("genres");
				for (auto& g : genres) {
					auto* ge = doc.NewElement("genre");
					ge->SetAttribute("songCount",  g.song_count);
					ge->SetAttribute("albumCount", g.album_count);
					// The name is the element's text, not an attribute --
					// Subsonic spells this one differently from every other
					// listing, and a client reading it as `name` gets nothing.
					ge->SetText(g.name.c_str());
					el->InsertEndChild(ge);
					}
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getSongsByGenre — the songs carrying one genre.
	server_.Get("/rest/getSongsByGenre.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string genre = req.get_param_value("genre");
		if (genre.empty()) { err(10, "Required parameter missing: genre."); return; }

		// Clamped like search: count is an unvalidated client integer and the
		// whole response is built in memory as one document.
		int count  = std::clamp(to_int(req.get_param_value("count"), 10),
		                        1, MAX_SEARCH_COUNT);
		int offset = std::clamp(to_int(req.get_param_value("offset"), 0),
		                        0, MAX_LIST_OFFSET);

		auto songs = store_.get_songs_by_genre(genre, count, offset);
		int  mbr   = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&songs, mbr](nlohmann::json& r) {
				nlohmann::json entries = nlohmann::json::array();
				for (auto& s : songs)
					entries.push_back(song_entry_json(s, mbr));
				r["songsByGenre"] = {{ "song", entries }};
				});
		else
			body = subsonic_ok([&songs, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("songsByGenre");
				for (auto& s : songs)
					el->InsertEndChild(song_entry_xml(doc, s, "song", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getVideos — every video in the library, as Child entries.  Videos share
	// the songs table with audio, so this is the ordinary song serialiser with
	// a different envelope key; isVideo and type are derived from the codec.
	server_.Get("/rest/getVideos.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto videos   = store_.get_videos();
		int  mbr      = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&videos, mbr](nlohmann::json& r) {
				nlohmann::json entries = nlohmann::json::array();
				for (auto& v : videos)
					entries.push_back(song_entry_json(v, mbr));
				r["videos"] = {{ "video", entries }};
				});
		else
			body = subsonic_ok([&videos, mbr](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("videos");
				for (auto& v : videos)
					el->InsertEndChild(song_entry_xml(doc, v, "video", mbr));
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getVideoInfo — the subtitle and audio tracks inside one video file.
	// Runs ffprobe per call rather than caching: it is a per-playback lookup,
	// not a browse path, and a stale track list is worse than a slow one.
	// No <conversion> child is emitted — nothing pre-transcodes today, and
	// advertising a conversion that does not exist is worse than silence.
	server_.Get("/rest/getVideoInfo.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song || !song->is_video) { err(70, "Video not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto streams = store_.get_video_streams(song_id);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&](nlohmann::json& r) {
				nlohmann::json caps = nlohmann::json::array();
				for (auto& c : streams.captions)
					caps.push_back({ {"id",   sid(c.index)},
					                 {"name", c.title.empty() ? c.language
					                                          : c.title},
					                 {"source", caption_source(c)} });
				nlohmann::json tracks = nlohmann::json::array();
				for (auto& a : streams.audio_tracks)
					tracks.push_back({ {"id",           sid(a.index)},
					                   {"name",         a.title},
					                   {"languageCode", a.language} });
				r["videoInfo"] = {
					{"id",         sid(song_id)},
					{"captions",   caps},
					{"audioTrack", tracks}
					};
				});
		else
			body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* vi = doc.NewElement("videoInfo");
				vi->SetAttribute("id", song_id);
				for (auto& c : streams.captions) {
					auto* el = doc.NewElement("captions");
					el->SetAttribute("id",   c.index);
					el->SetAttribute("name",
						(c.title.empty() ? c.language : c.title).c_str());
					el->SetAttribute("source", caption_source(c));
					vi->InsertEndChild(el);
					}
				for (auto& a : streams.audio_tracks) {
					auto* el = doc.NewElement("audioTrack");
					el->SetAttribute("id",           a.index);
					el->SetAttribute("name",         a.title.c_str());
					el->SetAttribute("languageCode", a.language.c_str());
					vi->InsertEndChild(el);
					}
				root->InsertEndChild(vi);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getCaptions — WebVTT for one subtitle track.  Returns the file itself,
	// not a Subsonic envelope, which is what the spec asks for.  `format` is
	// accepted and ignored: WebVTT is what a <track> element can consume, and
	// handing back SRT would only push the conversion onto the client.
	server_.Get("/rest/getCaptions.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		bool use_json = (fmt_of(req) == "json");

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(use_json
			                ? subsonic_error_json(10, "Required parameter missing: id.")
			                : subsonic_error(10, "Required parameter missing: id."),
			                use_json ? "application/json" : "application/xml");
			return;
			}
		// captionId selects an embedded stream; absent means the sidecar file.
		auto cid_it = req.params.find("captionId");
		int  index  = cid_it != req.params.end()
		    ? to_int(cid_it->second, -1) : -1;

		// The Chromecast fetches its own subtitle track and has no credentials
		// to do it with — the same problem stream.view solves the same way.
		// Handing the television the account's password instead would work and
		// is what the Android app does for its stream URLs; it is not something
		// to spread further.
		//
		// Both ids are read before the token is checked, for the same reason
		// stream.view reads its id first: the token authorises one song and one
		// of the caption ids that song's LOAD declared, so there is nothing to
		// check it against until both are known.
		const int req_song_id = to_int(it->second, -1);
		auto tok_it = req.params.find("castToken");
		bool cast_authed = tok_it != req.params.end()
		                && cast_manager_.valid_caption_token(tok_it->second,
		                                                     req_song_id, index);
		if (!cast_authed && !check_auth(req, res, store_)) return;

		// As in stream.view: the cast token is its own authority, everyone
		// else may not read another user's uploads by id.
		if (!cast_authed) {
			auto song = store_.get_song(req_song_id);
			if (song && !check_item_read_perm(req, res, store_, uploads_root_name_,
			                                  song->path, use_json)) return;
			}

		auto vtt = store_.get_captions_vtt(to_int(it->second, -1), index);
		if (vtt.empty()) {
			std::cout << stamp() << "getCaptions: nothing for id=" << log_safe(it->second, 64)
			          << " captionId=" << index << std::endl;
			res.status = 404;
			return;
			}
		// Success is logged too, and only here does it matter who asked: a
		// Chromecast fetches its own subtitle track, so this line arriving from
		// the television's address is the proof that the receiver accepted the
		// tracks the LOAD declared and went looking for one. Its absence is the
		// single most useful fact when captions do not appear on a cast.
		std::cout << stamp() << "getCaptions: id=" << log_safe(it->second, 64)
		          << " captionId=" << index << " " << vtt.size() << " bytes"
		          << (cast_authed ? " (cast token)" : "")
		          << " to " << client_addr(req) << std::endl;
		res.set_content(vtt, "text/vtt");
		});

	// getChapters / saveChapters — the song markers inside one video.
	//
	// A full concert is one file, and these are what let a client say where
	// each song starts.  They live in a sidecar `<stem>.chapters.txt` beside
	// the video, never inside the container: MP4 chapters are a track within
	// the file, so writing one would be an `ffmpeg -c copy` rewrite of every
	// byte of a multi-gigabyte concert on every save.  Nothing about them is
	// stored in either database — the file on disk is the only copy, which is
	// what keeps the music DB a cache and relocate_prefix() unchanged.
	//
	// `name` is reported exactly as the file holds it, empty included.  A
	// client draws its own "Chapter 3" placeholder for a bare marker; filling
	// one in here would look harmless and is not, because a client that saved
	// what it read would write the placeholder into a line somebody had
	// deliberately left blank, and the next save would find it real.
	server_.Get("/rest/getChapters.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto vc = store_.get_chapters(song_id);
		bool writable = item_write_allowed(req, store_, uploads_root_name_,
		                                   song->path);
		write_chapters_response(res, song_id, vc, song->duration, writable,
		                        use_json);
		});

	// getAlbumChapters — every chaptered video in one album folder.
	//
	// A second endpoint rather than an albumId mode on getChapters, because
	// the two read different things and the difference is the point:
	// getChapters reads the sidecar and is therefore always right, which is
	// what a playback path needs; this reads the `chapters` index the scan
	// maintains, which is what a browse path needs. Listing a concert's songs
	// must not cost a file read per video — or, for a rip with no sidecar, an
	// ffprobe.
	//
	// The consequence, stated because it looks like a bug: a video whose
	// markers live only in its container appears in the panel and *not* here,
	// until somebody saves them, which writes the sidecar the scan indexes.
	server_.Get("/rest/getAlbumChapters.view", [this](const httplib::Request& req,
	                                                   httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int folder_id = to_int(it->second, -1);

		std::string folder_rel = store_.get_folder_path(folder_id);
		if (folder_rel.empty()) { err(70, "Album folder not found."); return; }
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          folder_rel, use_json)) return;

		auto vids = store_.get_album_chapters(folder_id);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&](nlohmann::json& r) {
				nlohmann::json list = nlohmann::json::array();
				for (const auto& v : vids)
					list.push_back({
						{"id",      sid(v.song_id)},
						{"title",   v.title},
						{"chapter", chapter_array_json(v.chapters, v.duration)}
						});
				r["albumChapters"] = {
					{"id",   sid(folder_id)},
					{"song", list}
					};
				});
		else
			body = subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* parent = doc.NewElement("albumChapters");
				parent->SetAttribute("id", folder_id);
				for (const auto& v : vids) {
					auto* el = doc.NewElement("song");
					el->SetAttribute("id",    v.song_id);
					el->SetAttribute("title", v.title.c_str());
					chapter_array_xml(doc, el, v.chapters, v.duration);
					parent->InsertEndChild(el);
					}
				root->InsertEndChild(parent);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// The body is the chapter file itself, as text/plain, and it goes through
	// the same parse_chapters() that reads one off disk.  That is one
	// definition of the format rather than two, and it makes the round trip
	// trivially a fixed point — but it is also the only shape that works here.
	// Repeated start=/name= parameters cannot be used:
	//
	//  * httplib's parse_query_text() keeps a set of each whole "key=value"
	//    token and silently drops an exact repeat, so two chapters both called
	//    "Encore" would lose one name= and rename every marker after it.
	//  * CPPHTTPLIB_FORM_URL_ENCODED_PAYLOAD_MAX_LENGTH is 8192 for urlencoded
	//    bodies specifically, and set_payload_max_length() does not raise it.
	//
	// Both are undocumented behaviour of a pinned vendored copy, which is the
	// failure class third_party/README.md pins that copy to avoid.
	//
	// An empty body writes an empty file rather than removing it.  That empty
	// file is a tombstone: without it, clearing the markers of a rip whose
	// container carries its own would make the container's list come back, and
	// there would be no way to say "this film has no chapters" at all.
	server_.Post("/rest/saveChapters.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}
		int  song_id = to_int(it->second, -1);
		auto song    = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           song->path, use_json)) return;

		auto parsed = parse_chapters(req.body);
		if (parsed.chapters.size() > MediaStore::MAX_CHAPTERS) {
			err(0, "Too many chapters."); return;
			}

		// utf8_clean is not the same job as the control-character strip
		// parse_chapters already did: that one keeps the file's line format
		// intact, this one keeps invalid UTF-8 out of a JSON document, where
		// dump() throws.  Both are needed, and this is the boundary the other
		// arbitrary strings are cleaned at.
		for (auto& c : parsed.chapters)
			c.name = utf8_clean(c.name, MAX_CHAPTER_NAME_BYTES);

		if (!store_.save_chapters(song_id, parsed.chapters)) {
			err(0, "Could not write the chapter file."); return;
			}
		std::cout << stamp() << "saveChapters: id=" << song_id << " "
		          << parsed.chapters.size() << " marker(s), "
		          << parsed.skipped << " line(s) skipped, from "
		          << client_addr(req) << std::endl;

		// Re-read rather than echo: the server sorts, sanitises and may skip
		// lines, so the client must adopt what was actually written or the
		// panel and the file disagree until the next load.  Same rule as
		// castLoad's reply reporting the decision instead of letting the
		// client re-derive it.
		auto vc = store_.get_chapters(song_id);
		write_chapters_response(res, song_id, vc, song->duration, true,
		                        use_json);
		});

	// hls.m3u8 — a playlist computed from the stored duration.  Deliberately
	// stateless: no segment directory, no session, no temp files.  Every
	// segment URL is an ordinary stream.view transcode bounded by timeOffset
	// and duration, which is exactly how Subsonic does it.  Nothing here needs
	// cleaning up if a client walks away mid-playlist.
	//
	// Registered at two paths.  `hls.m3u8` is the spec's spelling and the one
	// the Android and iOS clients build, because ExoPlayer and AVFoundation
	// infer HLS from that extension; `hls.view` is what a client composing
	// every URL as <name>.view asks for, and it reached the "Not implemented"
	// catch-all before.  One handler can serve both only because every URI in
	// the body is *relative* — a segment resolves against /rest/ whichever
	// path was fetched — and the one place that is not true, a variant URI
	// naming this endpoint again, spells itself from req.path.
	auto hls_handler = [this](const httplib::Request& req,
	                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		const bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id.");
			return;
			}
		auto song = store_.get_song(to_int(it->second, -1));
		if (!song || !song->is_video) {
			err(70, "Video not found.");
			return;
			}
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it2 = req.params.find(k);
			return it2 != req.params.end() ? it2->second : def;
			};
		// bitRate is the spec's spelling here; it may carry an "@WxH" suffix
		// (e.g. "1000@640x480") naming the frame size for that variant, and
		// the spec allows it more than once, which is the request for a master
		// playlist.  Reading it with params.find() was wrong on both counts:
		// std::multimap does not promise *which* of several equal keys it
		// hands back, so the answer to a repeated bitRate was unspecified.
		//
		// Both halves are normalised through the same validators stream.view
		// uses, and not merely escaped. They are written into the playlist
		// *body* — a variant's URI and its RESOLUTION attribute as much as a
		// segment's query string — which is not a URL and not XML, so nothing
		// downstream would have caught a newline in either: a forged "#EXT-X-"
		// line, or a second URL of the sender's choosing. Round-tripping
		// through to_int/sane_video_size means only a number and a WxH can
		// ever be emitted, whatever arrived.
		struct Variant { int kbps; std::string size; };
		std::vector<Variant> variants;
		auto [blo, bhi] = req.params.equal_range("bitRate");
		for (auto b = blo; b != bhi; ++b) {
			std::string kb = b->second, size;
			if (auto at = kb.find('@'); at != std::string::npos) {
				size = kb.substr(at + 1);
				kb   = kb.substr(0, at);
				}
			const Variant v{ to_int(kb, 0), sane_video_size(size) };
			// Nothing to say: neither half survived validation.  Note a bare
			// "bitRate=0@640x480" does survive — 0 is the spec's "no limit",
			// and the frame size still governs.
			if (v.kbps <= 0 && v.size.empty()) continue;
			if (std::none_of(variants.begin(), variants.end(),
			                 [&](const Variant& o) {
			                 	return o.kbps == v.kbps && o.size == v.size;
			                 	}))
				variants.push_back(v);
			}

		const int SEGMENT = 10;
		int total = static_cast<int>(song->duration);
		if (total <= 0) {
			err(70, "Video has no known duration.");
			return;
			}

		// Credentials ride along on every URL in this body: the player fetches
		// the segments — and any variant playlist — itself, and carries none
		// of this request's context.  Only the parameters actually present are
		// echoed — an empty p= alongside t=/s= would send check_auth down the
		// password branch with a blank password and fail every segment.
		std::string auth;
		for (const char* k : { "u", "p", "t", "s", "c" }) {
			auto v = qp(k);
			if (!v.empty())
				auth += "&" + std::string(k) + "=" + url_encode(v);
			}
		auth += "&v=" + std::string(SUBSONIC_VER);

		// A master playlist announces each variant's BANDWIDTH, so only a
		// variant that named a bitrate can be one; a frame size alone still
		// governs a media playlist, as it always did.
		std::vector<Variant> announced;
		for (const auto& v : variants)
			if (v.kbps > 0) announced.push_back(v);
		// A total order rather than one on kbps alone: two variants may name
		// the same bitrate at different frame sizes, and std::sort is not
		// stable, so ordering on the bitrate alone would let two identical
		// requests produce two different playlists.
		std::sort(announced.begin(), announced.end(),
		          [](const Variant& a, const Variant& c) {
		          	return a.kbps != c.kbps ? a.kbps < c.kbps
		          	                        : a.size < c.size;
		          	});

		if (announced.size() >= 2) {
			// The variant URIs point back at this endpoint.  Stay on the
			// spelling the client used: req.path has already been through the
			// pre-routing handler, so a bare /rest/hls arrives here as
			// /rest/hls.view and self-references a path that is registered.
			const std::string self = req.path.substr(req.path.rfind('/') + 1);

			std::ostringstream mst;
			mst << "#EXTM3U\n"
			    << "#EXT-X-VERSION:3\n";
			for (const auto& v : announced) {
				// BANDWIDTH is honest for this encoder: video_ffmpeg_argv()
				// caps the picture at max(200, kbps-128) and adds a 128 kbps
				// AAC track on top.  PROGRAM-ID is gone from protocol version
				// 6, but is legal at the version 3 declared above and is what
				// Subsonic and Airsonic emit, so older players still get it.
				mst << "#EXT-X-STREAM-INF:PROGRAM-ID=1,BANDWIDTH="
				    << static_cast<long long>(v.kbps) * 1000;
				if (!v.size.empty()) mst << ",RESOLUTION=" << v.size;
				mst << "\n" << self << "?id=" << song->id
				    << "&bitRate=" << v.kbps;
				if (!v.size.empty()) mst << "@" << v.size;
				mst << auth << "\n";
				}
			// No #EXT-X-ENDLIST here: that tag terminates a media playlist,
			// and a master holds no segments to terminate.

			std::cout << stamp() << "hls: id=" << log_safe(it->second, 64)
			          << " variants=" << announced.size()
			          << " path=" << req.path << std::endl;
			res.set_header("Cache-Control", "no-store");
			res.set_content(mst.str(), "application/vnd.apple.mpegurl");
			return;
			}

		const std::string bitrate = (!variants.empty() && variants[0].kbps > 0)
		                          ? std::to_string(variants[0].kbps) : "";
		const std::string size    = variants.empty() ? "" : variants[0].size;

		std::ostringstream m3u;
		m3u << "#EXTM3U\n"
		    << "#EXT-X-VERSION:3\n"
		    << "#EXT-X-TARGETDURATION:" << SEGMENT << "\n"
		    << "#EXT-X-MEDIA-SEQUENCE:0\n"
		    << "#EXT-X-PLAYLIST-TYPE:VOD\n";
		// song->id rather than the raw id parameter: it is the same value once
		// resolved, and it is an int rather than whatever the client sent.
		for (int off = 0; off < total; off += SEGMENT) {
			int len = std::min(SEGMENT, total - off);
			m3u << "#EXTINF:" << len << ".0,\n"
			    << "stream.view?id=" << song->id
			    << "&timeOffset=" << off
			    << "&duration="   << len;
			if (!bitrate.empty()) m3u << "&maxBitRate=" << bitrate;
			if (!size.empty())    m3u << "&size=" << size;
			m3u << auth << "\n";
			}
		m3u << "#EXT-X-ENDLIST\n";

		std::cout << stamp() << "hls: id=" << log_safe(it->second, 64)
		          << " duration=" << total
		          << " segments=" << ((total + SEGMENT - 1) / SEGMENT)
		          << " path=" << req.path << std::endl;
		// This body contains the caller's credentials, once per segment, and
		// the player writes it to disk. It is also served under
		// Access-Control-Allow-Origin: * like everything else here. Nothing
		// should keep a copy of it.
		res.set_header("Cache-Control", "no-store");
		res.set_content(m3u.str(), "application/vnd.apple.mpegurl");
		};
	server_.Get("/rest/hls.m3u8", hls_handler);
	server_.Get("/rest/hls.view", hls_handler);

	// search2 / search3 — title/name substring search across artists, albums, songs.
	// Both share identical logic; only the response envelope key differs.
	auto search_handler = [this](const httplib::Request& req, httplib::Response& res,
	                              const char* key) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto qp = [&](const std::string& k, const std::string& def = "") {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : def;
			};

		auto err = [&](int code, const char* msg) {
			if (use_json)
				res.set_content(subsonic_error_json(code, msg), "application/json");
			else
				res.set_content(subsonic_error(code, msg),      "application/xml");
			};

		std::string query = qp("query");
		if (query.empty()) { err(10, "Required parameter missing: query."); return; }

		// Clamped as handle_album_list and getRecentSongs already clamp, and
		// through to_int so a non-numeric count is a default rather than a 500.
		auto count_param = [&](const char* k) {
			return std::clamp(to_int(qp(k, "20"), 20), 0, MAX_SEARCH_COUNT);
			};
		auto offset_param = [&](const char* k) {
			return std::clamp(to_int(qp(k, "0"), 0), 0, MAX_LIST_OFFSET);
			};
		int artist_count  = count_param("artistCount");
		int artist_offset = offset_param("artistOffset");
		int album_count   = count_param("albumCount");
		int album_offset  = offset_param("albumOffset");
		int song_count    = count_param("songCount");
		int song_offset   = offset_param("songOffset");
		// Defaults to 0, unlike its three siblings: a client that does not
		// know about chapters must not be made to pay for a fourth scan, and
		// one that does asks for them by name.
		int chapter_count  = std::clamp(to_int(qp("chapterCount", "0"), 0),
		                                0, MAX_SEARCH_COUNT);
		int chapter_offset = offset_param("chapterOffset");

		bool personal = qp("personal") == "true";
		std::string pu = personal ? qp("u") : "";
		auto sr = store_.search(query,
		                        artist_count, artist_offset,
		                        album_count,  album_offset,
		                        song_count,   song_offset,
		                        chapter_count, chapter_offset,
		                        pu);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&sr, key, mbr](nlohmann::json& r) {
				nlohmann::json artists = nlohmann::json::array();
				for (auto& a : sr.artists)
					artists.push_back({{"id", sid(a.id)}, {"name", a.title}});

				nlohmann::json albums = nlohmann::json::array();
				for (auto& c : sr.albums) {
					nlohmann::json al = {
						{"id",     sid(c.id)},
						{"parent", sid(c.parent_id)},
						{"isDir",  true},
						{"title",  c.title},
						{"artist", c.artist},
						{"album",  c.title}
						};
					if (c.cover_art_id >= 0) al["coverArt"] = sid(c.cover_art_id);
					albums.push_back(al);
					}

				nlohmann::json songs = nlohmann::json::array();
				for (auto& s : sr.songs)
					songs.push_back(song_entry_json(s, mbr));

				// A list of its own, never entries in `song`. A chapter has
				// no id anything can stream, star or queue, so a client told
				// it was a song would be handed a track that does not work.
				nlohmann::json chapters = nlohmann::json::array();
				for (auto& h : sr.chapters)
					chapters.push_back({
						{"songId",   sid(h.song_id)},
						{"parent",   sid(h.parent_id)},
						{"index",    h.index},
						{"start",    std::llround(h.start * 1000.0) / 1000.0},
						{"name",     h.name},
						{"track",    h.track},
						{"album",    h.album},
						{"artist",   h.artist} });

				r[key] = {{"artist", artists}, {"album", albums},
				          {"song", songs}};
				// Absent rather than empty when it was not asked for, so a
				// client that never sends chapterCount sees the response it
				// has always seen.
				if (!sr.chapters.empty()) r[key]["chapter"] = chapters;
				});
		else
			body = subsonic_ok([&sr, key, mbr](XMLDocument& doc, XMLElement* root) {
				auto* result = doc.NewElement(key);

				for (auto& a : sr.artists) {
					auto* el = doc.NewElement("artist");
					el->SetAttribute("id",   a.id);
					el->SetAttribute("name", a.title.c_str());
					result->InsertEndChild(el);
					}

				for (auto& c : sr.albums) {
					auto* el = doc.NewElement("album");
					el->SetAttribute("id",     c.id);
					el->SetAttribute("parent", c.parent_id);
					el->SetAttribute("isDir",  true);
					el->SetAttribute("title",  c.title.c_str());
					el->SetAttribute("artist", c.artist.c_str());
					el->SetAttribute("album",  c.title.c_str());
					if (c.cover_art_id >= 0) el->SetAttribute("coverArt", c.cover_art_id);
					result->InsertEndChild(el);
					}

				for (auto& s : sr.songs)
					result->InsertEndChild(song_entry_xml(doc, s, "song", mbr));

				for (auto& h : sr.chapters) {
					auto* el = doc.NewElement("chapter");
					el->SetAttribute("songId", h.song_id);
					el->SetAttribute("parent", h.parent_id);
					el->SetAttribute("index",  h.index);
					char buf[32];
					std::snprintf(buf, sizeof buf, "%.3f",
					              std::llround(h.start * 1000.0) / 1000.0);
					el->SetAttribute("start",  buf);
					el->SetAttribute("name",   h.name.c_str());
					el->SetAttribute("track",  h.track.c_str());
					el->SetAttribute("album",  h.album.c_str());
					el->SetAttribute("artist", h.artist.c_str());
					result->InsertEndChild(el);
					}

				root->InsertEndChild(result);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		};
	server_.Get("/rest/search2.view", [search_handler](const httplib::Request& req,
	                                                    httplib::Response& res) {
		search_handler(req, res, "searchResult2");
		});
	server_.Get("/rest/search3.view", [search_handler](const httplib::Request& req,
	                                                    httplib::Response& res) {
		search_handler(req, res, "searchResult3");
		});

	// getBookmarks — list all bookmarks for the authenticated user.
	server_.Get("/rest/getBookmarks.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;
		auto bms = store_.get_bookmarks(user);
		int mbr = request_max_bitrate(req, store_);

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&bms, mbr](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (auto& bm : bms) {
					nlohmann::json b = {
						{"position", bm.position},
						{"username", bm.username},
						{"comment",  bm.comment},
						{"created",  iso8601(bm.created)},
						{"changed",  iso8601(bm.changed)},
						{"entry",    song_entry_json(bm.entry, mbr)}
						};
					arr.push_back(b);
					}
				r["bookmarks"] = {{"bookmark", arr}};
				});
		else
			body = subsonic_ok([&bms, mbr](XMLDocument& doc, XMLElement* root) {
				auto* bookmarks = doc.NewElement("bookmarks");
				for (auto& bm : bms) {
					auto* b = doc.NewElement("bookmark");
					b->SetAttribute("position", bm.position);
					b->SetAttribute("username", bm.username.c_str());
					b->SetAttribute("comment",  bm.comment.c_str());
					b->SetAttribute("created",  iso8601(bm.created).c_str());
					b->SetAttribute("changed",  iso8601(bm.changed).c_str());
					b->InsertEndChild(song_entry_xml(doc, bm.entry, "entry", mbr));
					bookmarks->InsertEndChild(b);
					}
				root->InsertEndChild(bookmarks);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deleteBookmark — remove a bookmark by song id.
	server_.Get("/rest/deleteBookmark.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			auto msg = "Required parameter missing: id.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		auto song_path = store_.song_path_by_id(to_int(it->second, -1));
		if (!song_path || !store_.delete_bookmark(user, *song_path)) {
			auto msg = "Bookmark not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deletePlaylist — remove a playlist owned by the authenticated user.
	server_.Get("/rest/deletePlaylist.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		std::string user = req.params.find("u")->second;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			auto msg = "Required parameter missing: id.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		if (!store_.delete_playlist(to_int(it->second, -1), user)) {
			auto msg = "Playlist not found.";
			res.set_content(use_json ? subsonic_error_json(70, msg)
			                         : subsonic_error(70, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		std::string body = use_json ? subsonic_ok_json() : subsonic_ok();
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// listCastDevices — return the cached device list and kick off a background
	// refresh so the next call will have up-to-date results.
	server_.Get("/rest/listCastDevices.view", [this](const httplib::Request& req,
	                                                  httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto devices = cast_manager_.cached_devices();
		cast_manager_.discover_background();

		// Read here rather than inside the two rendering arms so JSON and XML
		// cannot disagree about a device, and so the settings lookups happen
		// once each.
		std::vector<std::string> prefs;
		prefs.reserve(devices.size());
		for (auto& d : devices) {
			std::string p = store_.get_setting("cast_video:" + d.id);
			prefs.push_back(p.empty() ? "auto" : p);
			}

		std::string body;
		if (use_json) {
			body = subsonic_ok_json([&devices, &prefs](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (size_t i = 0; i < devices.size(); i++) {
					auto& d = devices[i];
					arr.push_back({{"id", d.id}, {"name", d.name},
					               {"model", d.model},
					               {"address", d.address}, {"port", d.port},
					               {"manual", d.manual},
					               // What the device's `ca` TXT record said it
					               // can do; true when it announced nothing.
					               {"videoOut", d.video_out()},
					               // What a person decided about it, which
					               // overrides the record above.
					               {"videoPref", prefs[i]}});
					}
				r["castDevices"] = arr;
				});
			}
		else {
			body = subsonic_ok([&devices, &prefs](XMLDocument& doc,
			                                      XMLElement* root) {
				auto* el = doc.NewElement("castDevices");
				for (size_t i = 0; i < devices.size(); i++) {
					auto& d = devices[i];
					auto* dev = doc.NewElement("castDevice");
					dev->SetAttribute("id",      d.id.c_str());
					dev->SetAttribute("name",    d.name.c_str());
					dev->SetAttribute("model",   d.model.c_str());
					dev->SetAttribute("address", d.address.c_str());
					dev->SetAttribute("port",    d.port);
					dev->SetAttribute("manual",  d.manual);
					dev->SetAttribute("videoOut", d.video_out());
					dev->SetAttribute("videoPref", prefs[i].c_str());
					el->InsertEndChild(dev);
					}
				root->InsertEndChild(el);
				});
			}

		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// setCastDevicePref — record what a person decided about one device.
	//
	// Server-side rather than in each client's storage because it is a fact
	// about the device, not about the viewer: whether a WiiM plays a video
	// file's sound is the same answer in every browser and on the phone.  The
	// per-viewer knob next to it in spirit, castSyncDelay, is client-side for
	// exactly the opposite reason — it is about the screen you are watching.
	//
	// Gated on castRole and deliberately not on admin, though the row is
	// process-global: the person standing in front of the device is the one
	// who knows whether it has a screen, and that is a cast user.  The write
	// is bounded — the id must name a device in cached_devices(), the value
	// is one of three enumerated strings, and the worst a cast user can do
	// with it is what the setting exists to do.  Decided during the security
	// pass, not overlooked.
	server_.Get("/rest/setCastDevicePref.view", [this](const httplib::Request& req,
	                                                   httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		std::string id   = req.get_param_value("deviceId");
		std::string pref = req.get_param_value("videoPref");
		if (id.empty())   { err(10, "Required parameter missing: deviceId."); return; }
		if (pref.empty()) { err(10, "Required parameter missing: videoPref."); return; }

		// Refused rather than stored, because an unrecognised value would be
		// written, read back as neither "send" nor "sound", and so behave as
		// `auto` with nothing anywhere saying the setting had not taken.
		if (pref != "auto" && pref != "send" && pref != "sound") {
			err(10, "videoPref must be auto, send or sound.");
			return;
			}

		// The id has to name a device we know about.  Not for authorisation —
		// there is nothing to authorise — but because the key is composed from
		// it, and an unvalidated one is an unbounded write into client.settings
		// by any account with cast permission.  Same bound ladder_size() puts
		// on the thumbnail table, for the same reason.
		auto devices = cast_manager_.cached_devices();
		bool known = false;
		for (auto& d : devices) if (d.id == id) { known = true; break; }
		if (!known) { err(70, "Cast device not found."); return; }

		// `auto` is stored as an empty value, which get_setting() cannot tell
		// from an absent row — which is the point: a device nobody has decided
		// about and one reset to auto read the same, so `auto` needs no
		// spelling of its own on the way back out.
		store_.set_setting("cast_video:" + id, pref == "auto" ? "" : pref);
		// Changing your mind about a device is the natural place to make it
		// reconsider, and the only one: a codec pair recorded as refused is
		// never re-tried otherwise, so new firmware — or a different device
		// that inherited the address, and with it the id — would be judged for
		// ever on what its predecessor could not play.
		store_.set_setting(CAST_NOVIDEO_KEY + id, "");
		std::cout << stamp() << "Cast: device " << id << " videoPref=" << pref
		          << " (any refused codecs forgotten)" << std::endl;

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// startCast — enter cast mode: subsequent stream requests go to the Chromecast.
	server_.Get("/rest/startCast.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			res.set_content(
				use_json ? subsonic_error_json(10, "Required parameter missing: id.")
				         : subsonic_error(10, "Required parameter missing: id."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// Required, not optional with a fallback to `c=`. A client asking the
		// server to drive a Chromecast is already speaking the gaindrive
		// extension, so it can be asked to name itself — and refusing the
		// nameless case is what guarantees that "sent no castController" can
		// never own a session, and so can never collide with another client
		// that also sent none. A `c=` fallback would put every install of one
		// app under a single identity, which is the bug this endpoint is being
		// fixed for, one scale down.
		std::string controller = req.get_param_value("castController");
		if (controller.empty()) {
			const char* msg = "Required parameter missing: castController.";
			res.set_content(use_json ? subsonic_error_json(10, msg)
			                         : subsonic_error(10, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}

		auto devices = cast_manager_.cached_devices();
		CastManager::CastDevice chosen;
		bool found = false;
		for (auto& d : devices)
			if (d.id == it->second) { chosen = d; found = true; break; }

		if (!found) {
			res.set_content(
				use_json ? subsonic_error_json(70, "Cast device not found.")
				         : subsonic_error(70, "Cast device not found."),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// There is one control channel, so a second owner claiming it displaces
		// the first rather than running beside it. Tearing the old session down
		// first is what sends the receiver a STOP and clears the previous
		// owner's song and offset; the generation bump inside cast_teardown()
		// is what drops that owner's castEvents connection, so its UI leaves
		// cast mode on its own. This is also the only way back in for a browser
		// that cleared its site data and lost the id it started the session
		// with.
		if (cast_manager_.active() && !cast_owned_by(req)) {
			std::cout << stamp() << "Cast: session taken over by user="
			          << req.get_param_value("u") << " device=" << chosen.name
			          << std::endl;
			cast_teardown();
			}

		cast_manager_.start(chosen);
		cast_claim(req.get_param_value("u"), controller);
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// stopCast — stop Chromecast playback and exit cast mode.
	server_.Get("/rest/stopCast.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		// A non-owner stops nothing and is told it succeeded. Deliberately not
		// an error: a client that has just been displaced by a takeover runs
		// its own cleanup path, and that must neither kill the session that
		// replaced it nor raise a dialog about a session it no longer has.
		if (cast_owned_by(req)) cast_teardown();
		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castEvents — SSE stream that pushes MEDIA_STATUS updates to the browser.
	// Each event is a JSON object with playerState, currentTime, duration.
	// The connection is kept alive by the Chromecast heartbeat; a 15-second
	// keepalive comment is sent if no real update arrives in that window.
	//
	// This connection also acts as the cast session's heartbeat: when the
	// browser disconnects (tab closed, network drop, OS sleep) and no new
	// listener reconnects within CAST_IDLE_GRACE_S seconds, the watchdog
	// armed in the RAII guard's destructor tears the cast session down.
	server_.Get("/rest/castEvents.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		{
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		}
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			res.status = 204;
			return;
			}
		// The session this connection belongs to. A takeover bumps it, which is
		// how a displaced listener learns it has been replaced: `active_` is
		// true either side of the stop()/start() pair, and this thread spends
		// most of its life blocked inside wait_status(), so the flag alone
		// would never show the gap.
		const int session_gen = cast_session_gen_.load();

		// A cap on concurrent SSE listeners. Each one parks a worker thread
		// in wait_status() for the life of the connection, so without a
		// ceiling the session's one authorised owner could occupy the whole
		// pool with a loop over EventSource(). A real client holds one; the
		// allowance covers a reload race and a second tab.
		if (cast_sse_listeners_.load() >= CAST_SSE_MAX_LISTENERS) {
			std::cout << stamp() << "Cast: refusing SSE listener, "
			          << CAST_SSE_MAX_LISTENERS << " already attached"
			          << std::endl;
			res.status = 503;
			return;
			}

		res.set_header("Cache-Control",    "no-cache");
		res.set_header("X-Accel-Buffering","no");   // disable nginx/apache buffering

		// RAII helper: increments the listener counter on construction and
		// arms the auto-stop watchdog on destruction.  Captured via
		// shared_ptr because httplib stores the content provider in a
		// std::function (which requires copyable callables); the guard's
		// destructor still fires exactly once, when the last copy of the
		// lambda is dropped — i.e. when the connection ends, regardless of
		// whether it ended via sink.write returning false (client gone),
		// wait_status seeing !active(), or normal completion.
		struct ListenerGuard {
			GainDrive* self;
			int        gen;
			ListenerGuard(GainDrive* s, int g) : self(s), gen(g) {
				int n = ++self->cast_sse_listeners_;
				++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener attached, count="
					          << n << std::endl;
				}
			~ListenerGuard() {
				int n = --self->cast_sse_listeners_;
				int g = ++self->cast_wd_gen_;
				if (self->debug_)
					std::cout << stamp() << "Cast: SSE listener detached, count="
					          << n << std::endl;
				if (n != 0 || !self->cast_manager_.active()) return;
				// A listener displaced by a takeover must not arm a watchdog
				// against the session that replaced it: the new owner may not
				// have opened its own stream yet, so the listener count is
				// legitimately 0 for a moment.
				if (self->cast_session_gen_.load() != gen) return;
				GainDrive* gd = self;
				int        sg = gen;
				std::thread([gd, g, sg] {
					std::this_thread::sleep_for(std::chrono::seconds(CAST_IDLE_GRACE_S));
					if (gd->cast_wd_gen_.load() != g)        return; // newer event
					if (gd->cast_sse_listeners_.load() != 0) return; // listener back
					if (!gd->cast_manager_.active())         return; // already stopped
					if (gd->cast_session_gen_.load() != sg)  return; // another session
					std::cout << stamp() << "Cast: no SSE listener for "
					          << CAST_IDLE_GRACE_S << "s, auto-stopping"
					          << std::endl;
					gd->cast_teardown();
					}).detach();
				}
			};
		auto guard = std::make_shared<ListenerGuard>(this, session_gen);

		res.set_chunked_content_provider("text/event-stream",
			[this, guard, session_gen](size_t, httplib::DataSink& sink) -> bool {
				auto s = cast_manager_.wait_status(15000);
				if (!cast_manager_.active())                 return false;
				if (cast_session_gen_.load() != session_gen) return false;
				// The same description castLoad's reply carried, repeated on
				// every push.  Not redundancy: a LOAD that was refused is
				// retried one rung down the ladder — a film becoming its
				// soundtrack — and the reply the client read describes the
				// attempt that failed.  Without this the info panel goes on
				// claiming "as stored, MP4" over a FLAC soundtrack until the
				// page is reloaded, which is precisely the "reporting the
				// reload rather than the stream" that castSession exists to
				// prevent.
				const CastStreamInfo st = cast_stream();
				std::string event = "data: " + nlohmann::json({
					{"playerState", s.player_state},
					{"currentTime", s.current_time},
					{"duration",    s.duration},
					{"idleReason",  s.idle_reason},
					{"startOffset", last_cast_offset_},
					{"audioOnly",          st.audio_only},
					{"receiverShowsVideo", st.receiver_video},
					{"contentType",        st.mime},
					{"sentSuffix",         st.suffix},
					{"sentBitRate",        st.bitrate},
					{"tier",               st.tier}}).dump() + "\n\n";
				return sink.write(event.data(), event.size());
				});
		});

	// castSession — non-blocking snapshot of current cast session state.
	// Used by the browser on page load to restore the cast UI after a reload.
	server_.Get("/rest/castSession.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		// Someone else's session is not visible here. This is what stops a
		// second browser adopting it wholesale on page load — showShell()
		// restores the cast UI from whatever this returns.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			res.set_content(subsonic_ok_json([](nlohmann::json& r) {
				r["castSession"]["active"] = false;
				}), "application/json");
			return;
			}

		auto st = cast_manager_.get_status();
		double song_duration = 0.0;
		if (!last_cast_song_id_.empty()) {
			auto song = store_.get_song(std::stoi(last_cast_song_id_));
			if (song) song_duration = song->duration;
			}
		res.set_content(subsonic_ok_json([&](nlohmann::json& r) {
			r["castSession"]["active"]       = true;
			r["castSession"]["deviceId"]     = cast_manager_.get_device_id();
			r["castSession"]["deviceName"]   = cast_manager_.get_device_name();
			r["castSession"]["songId"]       = last_cast_song_id_;
			r["castSession"]["startOffset"]  = last_cast_offset_;
			r["castSession"]["playerState"]  = st.player_state;
			r["castSession"]["currentTime"]  = st.current_time;
			r["castSession"]["duration"]     = st.duration;
			r["castSession"]["songDuration"] = song_duration;
			// The same description castLoad's reply carries, and it must stay
			// the same: a reloaded page has no castLoad response to have read
			// it from, and a client that drew one thing before the reload and
			// another after would be reporting the reload rather than the
			// stream.
			const CastStreamInfo st = cast_stream();
			r["castSession"]["audioOnly"]          = st.audio_only;
			r["castSession"]["receiverShowsVideo"] = st.receiver_video;
			r["castSession"]["contentType"]        = st.mime;
			r["castSession"]["sentSuffix"]         = st.suffix;
			r["castSession"]["sentBitRate"]        = st.bitrate;
			r["castSession"]["tier"]               = st.tier;
			// Which subtitle track is on, in the same 1..n numbering castLoad
			// and castControl take, so a client that has just reloaded can
			// mark its picker without asking the receiver anything.
			auto cap = cast_manager_.caption_state();
			r["castSession"]["trackId"] = cap.active_track_ids.empty()
			    ? 0 : cap.active_track_ids.front();
			}), "application/json");
		});

	// castControl — send play/pause/seek to the Chromecast.
	server_.Get("/rest/castControl.view", [this](const httplib::Request& req,
	                                             httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;
		// As castLoad: a non-owner controls nothing and is told the session it
		// thinks it has is not there.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			const char* msg = "Cast not active.";
			res.set_content(use_json ? subsonic_error_json(0, msg)
			                         : subsonic_error(0, msg),
			                use_json ? "application/json" : "application/xml");
			return;
			}
		std::string action;
		auto ai = req.params.find("action");
		if (ai != req.params.end()) action = ai->second;

		if (action == "pause")      cast_manager_.cast_pause();
		else if (action == "play") {
			auto st = cast_manager_.get_status();
			if (st.player_state == "PAUSED") {
				cast_manager_.cast_play();
				} else if (st.player_state == "IDLE" && !last_cast_song_id_.empty()) {
				// Session timed out during a long pause — re-issue a full load from
				// the saved position so the Chromecast can restart the stream.
				int  sid  = to_int(last_cast_song_id_, -1);
				auto song = store_.get_song(sid);
				if (song) {
					float pos = cast_manager_.last_known_time();
					// The caption the viewer had on, not none: this recovers a
					// session that timed out mid-film, and coming back without
					// the subtitles would be a second thing to fix by hand.
					auto cap = cast_manager_.caption_state();
					int  track = cap.active_track_ids.empty()
					    ? 0 : cap.active_track_ids.front();
					cast_load_song(req, *song, sid,
					               pos > 0.5f ? pos : 0.0f, track);
					}
				}
			}
		else if (action == "seek") {
			auto ti = req.params.find("time");
			if (ti != req.params.end())
				cast_manager_.cast_seek(to_float(ti->second, 0.0f));
			}
		else if (action == "captions") {
			// trackId numbers the caption tracks 1..n as getVideoInfo lists
			// them; 0 or absent turns them off.
			int track_id = to_int(req.get_param_value("trackId"), 0);
			std::vector<int> ids;
			if (track_id > 0) ids.push_back(track_id);
			if (!cast_manager_.cast_tracks(ids))
				std::cout << stamp() << "Cast: captions trackId=" << track_id
				          << " not applied — no media session yet" << std::endl;
			}

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// castLoad — instruct the Chromecast to fetch and play a song.
	// Separate from stream.view so the browser triggers the cast load without
	// making a Range request that httplib would reject (stream.view returns 204,
	// but httplib overrides 204+Range to 416 when content_length is 0).
	server_.Get("/rest/castLoad.view", [this](const httplib::Request& req,
	                                          httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_cast_perm(req, res, store_, use_json)) return;

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		// "Cast not active" is the honest answer for a non-owner too: from that
		// client's point of view it has no session.
		if (!cast_manager_.active() || !cast_owned_by(req)) {
			err(0, "Cast not active.");
			return;
			}

		auto it = req.params.find("id");
		if (it == req.params.end()) {
			err(10, "Required parameter missing: id."); return;
			}

		auto song = store_.get_song(to_int(it->second, -1));
		if (!song) { err(70, "Song not found."); return; }
		// The uploads root is personal, and this is an id-addressed read like
		// stream.view — more so: the cast token it mints is then accepted by
		// stream.view with no account and no bitrate cap at all, so skipping
		// the check here skips it everywhere downstream.
		if (!check_item_read_perm(req, res, store_, uploads_root_name_,
		                          song->path, use_json)) return;

		auto to_it = req.params.find("timeOffset");
		float cast_offset = to_it != req.params.end()
		    ? to_float(to_it->second, 0.0f) : 0.0f;
		// trackId, not captionId: the cast API numbers caption tracks 1..n in
		// the order getVideoInfo lists them, and 0 means none.  captionId
		// cannot say "none" — SIDECAR_CAPTION_INDEX is -1 and so is a missing
		// parameter, so "off" and "the sidecar file" would be one value.
		int track_id = to_int(req.get_param_value("trackId"), 0);
		// The reply says what was actually sent — the picture or only the
		// soundtrack, the contentType declared, and the container, bitrate and
		// tier the receiver will get — so the client draws what happened
		// rather than deciding it a second time from the device list and the
		// codec pair.
		CastStreamInfo sent = cast_load_song(req, *song,
		                                     to_int(it->second, -1),
		                                     cast_offset, track_id);
		res.set_content(
			use_json
			    ? subsonic_ok_json([&sent](nlohmann::json& r) {
			          r["castLoad"]["audioOnly"]   = sent.audio_only;
			          r["castLoad"]["receiverShowsVideo"] = sent.receiver_video;
			          r["castLoad"]["contentType"] = sent.mime;
			          r["castLoad"]["sentSuffix"]  = sent.suffix;
			          r["castLoad"]["sentBitRate"] = sent.bitrate;
			          r["castLoad"]["tier"]        = sent.tier;
			          })
			    : subsonic_ok([&sent](XMLDocument& doc, XMLElement* root) {
			          auto* e = doc.NewElement("castLoad");
			          e->SetAttribute("audioOnly",   sent.audio_only);
			          e->SetAttribute("receiverShowsVideo",
			                          sent.receiver_video);
			          e->SetAttribute("contentType", sent.mime.c_str());
			          e->SetAttribute("sentSuffix",  sent.suffix.c_str());
			          e->SetAttribute("sentBitRate", sent.bitrate);
			          e->SetAttribute("tier",        sent.tier.c_str());
			          root->InsertEndChild(e);
			          }),
			use_json ? "application/json" : "application/xml");
		});

	// updateSong — update title and/or track number for a single song.
	//
	// For audio the file is authoritative: the tags are written first and the
	// database only mirrors them.  For video there is no tag worth writing —
	// the scanner reads a video's title, year and episode number from its
	// filename and never opens it with TagLib — so the edit is recorded in the
	// client DB and re-applied by every scan instead.  Either way the music DB
	// stays a cache that a full rescan can rebuild.
	server_.Get("/rest/updateSong.view", [this](const httplib::Request& req,
	                                            httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }
		int song_id = to_int(it->second, -1);

		std::optional<std::string> title;
		std::optional<int> track_number;
		std::optional<int> year;
		std::optional<int> disc_number;
		// Typed by a person and stored raw until now: invalid UTF-8 in
		// songs.title made every later JSON response holding the song throw
		// out of dump() — a permanent 500 — and a C0 byte makes the XML
		// envelope unparseable for every conformant client.
		if (req.params.count("title")) title        = clean_name(req.params.find("title")->second);
		if (req.params.count("track")) track_number = to_int(req.params.find("track")->second, 0);
		if (req.params.count("year"))  year         = to_int(req.params.find("year")->second, 0);
		if (req.params.count("disc"))  disc_number  = to_int(req.params.find("disc")->second, 0);

		// Resolve the file path before touching anything.
		auto song = store_.get_song(song_id);
		if (!song) { err(70, "Song not found."); return; }

		// This rewrites a tag on disk, so it is a library modification and not
		// merely a read — check_auth alone would let any account retag any
		// file in any root by guessing an id.
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           song->path, use_json)) return;

		if (song->is_video) {
			// Record it where a rescan can find it again.  Note what is
			// deliberately *not* done: TagLib can write an .mp4, but nothing
			// ever reads that tag back — read_song_metadata() returns after
			// the ffprobe branch — so writing it would leave two copies of one
			// fact, and the unread copy would be the one on disk.
			store_.set_song_meta_override(song->path, title, track_number,
			                               year, disc_number);
			}
		else {
			// Write tags first — if this fails we must not update the database.
			try {
				std::string song_abs = store_.abs_path(song->path);
				if (!store_.path_is_within_root(song_abs)) {
					std::cout << stamp() << "updateSong: refusing path outside every root: "
					          << song_abs << std::endl;
					err(0, "Refusing to write outside the configured roots.");
					return;
					}
				TagLib::FileRef f(song_abs.c_str());
				if (f.isNull() || !f.tag()) {
					err(0, "Could not open file for tag editing.");
					return;
					}
				if (title)        f.tag()->setTitle(TagLib::String(*title, TagLib::String::UTF8));
				if (track_number) f.tag()->setTrack(*track_number);
				if (year)         f.tag()->setYear(*year);
				if (disc_number) {
					// PropertyMap gives portable access to DISCNUMBER across all formats.
					TagLib::PropertyMap props = f.file()->properties();
					props.replace("DISCNUMBER",
						TagLib::StringList(TagLib::String(std::to_string(*disc_number))));
					f.file()->setProperties(props);
					}
				if (!f.save()) {
					err(0, "Could not write tags to file.");
					return;
					}
				}
			catch (...) {
				err(0, "Exception while writing tags to file.");
				return;
				}
			}

		// Persisted successfully — now mirror the change in the database.
		store_.update_song_meta(song_id, title, track_number, year, disc_number);

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// setCoverArt — set a cover image for an album (identified by folder_id).
	// Accepts either a multipart "file" part or a "url" form field; for the
	// latter the server fetches the URL and validates it returned an image.
	server_.Post("/rest/setCoverArt.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto it = req.params.find("id");
		if (it == req.params.end()) { err(10, "Required parameter missing: id."); return; }
		int folder_id = to_int(it->second, -1);

		// Resolved and permission-checked before anything is fetched: this
		// writes cover.jpg into a library folder and, with a url=, makes the
		// server issue an outbound request. Neither should happen for a caller
		// who is not allowed to change this folder in the first place.
		std::string folder_rel = store_.get_folder_path(folder_id);
		if (folder_rel.empty()) { err(70, "Album folder not found."); return; }
		if (!check_item_write_perm(req, res, store_, uploads_root_name_,
		                           folder_rel, use_json)) return;

		std::string bytes;
		std::string url;
		if (req.form.has_file("url")) url = req.form.get_file("url").content;
		else if (req.has_param("url")) url = req.get_param_value("url");

		if (req.form.has_file("file")) {
			// A reference into the form map, not get_file(), which returns a
			// copy — this part can be a whole image.
			const auto& fp = req.form.files.find("file")->second;
			if (fp.content_type.rfind("image/", 0) != 0) {
				err(0, "Uploaded file must be an image."); return;
				}
			bytes = fp.content;
			}
		else if (!url.empty()) {
			// Redirects are followed by hand rather than with
			// set_follow_location(true), because the check below has to run
			// again for each hop: a public URL that 302s to 127.0.0.1 is the
			// ordinary way an address check that only looks at what was typed
			// is defeated.
			std::string next = url;
			httplib::Result r;
			for (int hop = 0; ; ++hop) {
				if (hop > MAX_COVER_REDIRECTS) {
					err(0, "Too many redirects."); return;
					}
				bool https = next.rfind("https://", 0) == 0;
				bool http  = next.rfind("http://",  0) == 0;
				if (!https && !http) { err(0, "URL must be http(s)."); return; }
				size_t scheme_end = https ? 8 : 7;
				size_t slash = next.find('/', scheme_end);
				std::string host = next.substr(scheme_end, slash == std::string::npos
				                               ? std::string::npos : slash - scheme_end);
				std::string path = (slash == std::string::npos) ? "/" : next.substr(slash);

				// One message for "would not resolve" and "resolves somewhere
				// private", deliberately: the difference is exactly what makes
				// this endpoint a port scanner otherwise.
				if (!host_is_global(host)) {
					std::cout << stamp() << "setCoverArt: refusing non-global host: "
					          << host << std::endl;
					err(0, "Failed to fetch URL."); return;
					}

				auto fetch = [&](auto& cli) {
					cli.set_follow_location(false);
					cli.set_connection_timeout(5);
					cli.set_read_timeout(15);
					cli.set_default_headers({
						{"User-Agent", USER_AGENT}
						});
					return cli.Get(path.c_str());
					};
				auto [hname, hport] = split_host_port(host, https ? 443 : 80);
				if (https) { httplib::SSLClient cli(hname, hport); r = fetch(cli); }
				else       { httplib::Client    cli(hname, hport); r = fetch(cli); }
				if (!r) { err(0, "Failed to fetch URL."); return; }
				if (r->status == 301 || r->status == 302 || r->status == 303
				    || r->status == 307 || r->status == 308) {
					std::string loc = r->get_header_value("Location");
					if (loc.empty()) { err(0, "Failed to fetch URL."); return; }
					next = loc;
					continue;
					}
				break;
				}
			if (r->status != 200) { err(0, "Failed to fetch URL."); return; }
			// An arbitrary third-party URL; nothing about it is bounded except
			// by us. Same reasoning as MAX_PORTRAIT_BYTES.
			if (r->body.size() > MAX_COVER_BYTES) {
				err(0, "Image at URL is too large."); return;
				}
			auto ct = r->get_header_value("Content-Type");
			if (ct.rfind("image/", 0) != 0) {
				err(0, "URL did not return an image."); return;
				}
			bytes = std::move(r->body);
			}
		else {
			err(10, "Required parameter missing: file or url."); return;
			}

		// The filename follows the bytes, not the claim.  Both checks above
		// are claims — an upload's content_type is whatever the client typed
		// and a provider's Content-Type is whatever it felt like — and this
		// used to write everything as cover.jpg regardless.  That is not
		// cosmetic: the scanner indexes only .jpg/.jpeg/.png, so a WebP
		// written as cover.jpg became a cover nothing could decode.
		std::string up_mime = imagescale::sniff_mime(bytes);
		if (up_mime != "image/jpeg" && up_mime != "image/png") {
			err(0, "Unsupported image format; use JPEG or PNG.");
			return;
			}
		const bool is_png = (up_mime == "image/png");

		namespace fs = std::filesystem;
		// **A file-album's cover is the sidecar named after it**, not
		// cover.jpg inside it — there is no inside.  This is the whole point
		// of the endpoint for a loose film: under the rule where a section
		// holding loose films was itself the album, an upload here could only
		// ever set the *section's* cover, so a film TMDB had matched wrongly
		// could not be given the right poster at all. setCoverArt is the only
		// remedy for a bad match, so that gap had no way round it.
		//
		// The names are find_song_cover()'s first two, which is what makes the
		// next scan pick this up.
		std::error_code fec;
		const bool file_album =
			fs::is_regular_file(store_.abs_path(folder_rel), fec);
		auto beside = [&](const char* suffix) {
			fs::path p(folder_rel);
			p.replace_extension("");
			return p.string() + suffix;
			};
		std::string cover_rel = file_album
		    ? beside(is_png ? ".png" : ".jpg")
		    : (fs::path(folder_rel) / (is_png ? "cover.png" : "cover.jpg")).string();
		fs::path cover_abs = fs::path(store_.abs_path(cover_rel));
		if (!store_.path_is_within_root(cover_abs)) {
			std::cout << stamp() << "setCoverArt: refusing path outside every root: "
			          << cover_abs.string() << std::endl;
			err(0, "Refusing to write outside the configured roots.");
			return;
			}
		{
		std::ofstream out(cover_abs, std::ios::binary | std::ios::trunc);
		if (!out) { err(0, "Failed to write cover art to disk."); return; }
		out.write(bytes.data(), (std::streamsize)bytes.size());
		}

		// find_cover() prefers cover.jpg over cover.png, so uploading a PNG
		// beside an existing cover.jpg would leave the old one winning on the
		// next scan and the upload apparently ignored.  Only the names this
		// upload outranks are removed.
		//
		// For a file-album the list is one longer, because find_song_cover()'s
		// order is .jpg, .jpeg, .png: writing the .png has to clear *both*
		// earlier spellings or the stale one keeps winning, which is the same
		// bug one rung further along.  The "-poster.*" forms are never touched
		// — they rank below all three, so they cannot win anyway.
		std::vector<std::string> outranked;
		if (file_album) {
			if (is_png) { outranked.push_back(beside(".jpg"));
			              outranked.push_back(beside(".jpeg")); }
			}
		else
			outranked.push_back(
				(fs::path(folder_rel) / (is_png ? "cover.jpg" : "cover.png")).string());
		for (auto& other_rel : outranked) {
			fs::path other_abs = fs::path(store_.abs_path(other_rel));
			std::error_code rec;
			if (store_.path_is_within_root(other_abs) && fs::exists(other_abs, rec)) {
				fs::remove(other_abs, rec);
				store_.drop_cover_thumbs(other_rel);
				cover_cache_.invalidate(other_rel);
				}
			}

		// Unchanged, and it is what closes the gap above: the manual_covers row
		// is keyed on `folder_rel`, which for a file-album is the media file's
		// own stored path — the very string lookup_video_meta() passes to
		// cover_is_manual() before it fetches a poster.  So a poster chosen
		// here now survives every later scan.
		store_.set_cover_art_path(folder_rel, cover_rel);
		cover_cache_.invalidate(cover_rel);

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// Upload a music archive (zip / tar / tar.gz / tgz) and extract it into
	// the calling user's personal folder under <uploads root>/<username>/.
	// The content-reader form, and that is the security boundary as much as a
	// convenience: httplib hands the handler control *before* reading a byte
	// of body, so auth and the upload permission are checked against the
	// query-string credentials first, and an unauthenticated multi-gigabyte
	// POST costs the server nothing. The archive then streams to a file on
	// disk rather than through req.body — the old form buffered the whole
	// body in memory (and the multipart copy doubled it), which made /upload
	// the cheapest OOM in the server.
	server_.Post("/upload", [this](const httplib::Request& req, httplib::Response& res,
	                               const httplib::ContentReader& content_reader) {
		if (!check_auth(req, res, store_)) return;

		auto json_err = [&](const std::string& msg) {
			nlohmann::json j;
			j["status"]  = "error";
			j["message"] = msg;
			res.status = 400;
			res.set_content(j.dump(), "application/json");
			};

		// Refuse rather than fall back to a library root: an upload landing in
		// a shared library would be scanned as somebody's album.
		if (users_dir_.empty()) {
			json_err("No uploads root is configured on this server.");
			return;
			}

		// Require upload_allowed (or admin); capture the username for the dest path.
		std::string uname;
		{
		auto it = req.params.find("u");
		uname = (it != req.params.end()) ? it->second : std::string{};
		auto ui = store_.get_user(uname);
		if (!ui || (!ui->upload_allowed && !ui->is_admin)) {
			json_err("User is not authorized to upload.");
			return;
			}
		}

		// Typed names, exactly as fetchUrl takes them — the two producers are
		// the same steps around a different source of bytes, and scan_batch()
		// below has always accepted these. Blank means "keep what the archive's
		// own folders and the files' tags say", which is what every upload did
		// before this existed.
		std::string want_artist, want_album;
		{
		auto typed = [&](const char* key, std::string& out) -> bool {
			auto it = req.params.find(key);
			if (it == req.params.end()) return true;
			if (it->second.find_first_not_of(" \t") == std::string::npos) return true;
			// utf8_clean first: invalid UTF-8 reaches folders.path and then
			// dump(), which throws. Then sanitise_component, which is what
			// stops a typed "../.." being a path at all.
			out = sanitise_component(utf8_clean(it->second, 200));
			return !out.empty();
			};
		if (!typed("artist", want_artist)) {
			json_err("The artist name contains nothing usable."); return;
			}
		if (!typed("album", want_album)) {
			json_err("The album name contains nothing usable."); return;
			}
		}

		if (!req.is_multipart_form_data()) {
			json_err("Expected multipart/form-data."); return;
			}

		namespace fs = std::filesystem;

		// Each upload lands in its own UUID subdirectory so that messy zip
		// structures (missing artist/album dirs) are always isolated and the
		// scanner has a stable "artist-level" root to work from. The archive
		// itself streams to a sibling .part file — outside dest, so the
		// extraction can never mistake it for content — and is deleted
		// whatever happens.
		std::string uuid = make_uuid();
		fs::path dest = fs::path(users_dir_) / uname / uuid;
		fs::path part = fs::path(users_dir_) / uname / (uuid + ".upload.part");

		std::ofstream out;
		std::string cur_part;      // name of the multipart part being received
		std::string name;          // the file part's client-side filename
		uint64_t received = 0;
		bool too_big = false, bad_type = false, io_failed = false;

		auto type_ok = [](const std::string& n) {
			return n.ends_with(".zip") || n.ends_with(".tar")
			    || n.ends_with(".tar.gz") || n.ends_with(".tgz");
			};

		bool read_ok = content_reader(
			[&](const httplib::FormData& fd) {
				cur_part = fd.name;
				if (cur_part != "file") return true;   // other parts: ignored
				name = fd.filename;
				// Refusing here aborts the read, so a wrong extension costs
				// the client its upload time, not the server its disk.
				if (!type_ok(name)) { bad_type = true; return false; }
				std::error_code ec;
				fs::create_directories(part.parent_path(), ec);
				out.open(part, std::ios::binary | std::ios::trunc);
				if (!out) { io_failed = true; return false; }
				return true;
				},
			[&](const char* data, size_t n) {
				if (cur_part != "file") return true;
				received += n;
				if (received > MAX_REQUEST_BYTES) { too_big = true; return false; }
				out.write(data, static_cast<std::streamsize>(n));
				if (!out) { io_failed = true; return false; }
				return true;
				});
		if (out.is_open()) out.close();

		auto drop_part = [&]{ std::error_code ec; fs::remove(part, ec); };

		if (bad_type) {
			drop_part();
			json_err("Unsupported file type. Use zip, tar, tar.gz, or tgz.");
			return;
			}
		if (too_big)   { drop_part(); res.status = 413;
		                 json_err("Upload too large."); return; }
		if (io_failed) { drop_part(); res.status = 500;
		                 json_err("Could not store the upload."); return; }
		if (!read_ok)  { drop_part(); json_err("Upload was interrupted."); return; }
		if (name.empty()) { drop_part(); json_err("Missing file part."); return; }

		fs::create_directories(dest);

		std::cout << stamp() << "Upload: extracting " << log_safe(name)
		          << " (" << received << " bytes)"
		          << " for user " << uname
		          << " into " << dest << std::endl;

		int n = extract_archive_to_dir(part, dest);
		drop_part();
		if (n < 0) { json_err("Failed to open archive."); return; }

		std::cout << stamp() << "Upload: extracted " << n << " file(s) to " << dest << std::endl;

		std::string rel_batch = uploads_root_name_ + "/" + uname + "/" + uuid;
		// Normalising and scanning is the same three steps for both producers —
		// an archive here, a URL fetch in fetch_worker() — so it lives in one
		// place. Detached here because this one is on an HTTP thread and the
		// client is holding a request open; the fetch worker calls it directly,
		// being a background thread already.
		std::thread([this, rel_batch, dest, want_artist, want_album]{
			scan_batch(rel_batch, dest, "Unknown Artist", want_artist, want_album);
			}).detach();

		nlohmann::json j;
		j["status"] = "ok";
		j["files"]  = n;
		j["batch"]  = rel_batch;
		res.set_content(j.dump(), "application/json");
		});

	// moveAlbum — put an album somewhere, under a name.
	//
	// Params: id (album folder_id), musicFolderId, folder, album.  Everything
	// but the id is optional and every omitted part means "unchanged".
	//
	// This is one endpoint because it was always one operation.  It replaces
	// renameAlbum and promoteAlbum, which were the same handler written twice
	// — resolve the id, validate, move the directory, relocate_prefix, drop
	// the emptied source, rescan destination-then-source — differing only in
	// how the destination was spelled.  Each also had a gap the other filled:
	// a rename could not cross roots, so a film misfiled under a music root
	// could not reach a categories root without shell access, and a promote
	// could not rename, so the Move dialog could not fix a channel-derived
	// album name in the same step.
	//
	// **The destination rule** is the one place the two behaviours genuinely
	// differed, so it is a rule rather than something implicit in the code:
	//
	//  * musicFolderId given -> "<root>/<folder>/<album>", exactly two levels
	//    under that root.  The old promote, generalised.
	//  * musicFolderId omitted -> "<everything above the artist level,
	//    unchanged>/<folder>/<album>".  The old rename.  For an upload that
	//    preserves "<uploads>/<user>/<uuid>/".
	//
	// music_folder_by_id() already refuses anything that is not a browsable
	// library root, the uploads root included, so promoting *into* uploads
	// stays impossible without a test of its own.
	//
	// Note what promoteAlbum's five-component source check does **not** become
	// here: it is gone, deliberately, because with it an admin cannot move a
	// library album at all — which is the gap being closed.  What it looked
	// like it was defending is defended by the admin requirement and by
	// path_is_within_root() on the *destination*.  deleteUpload's
	// identical-looking check is a different thing and must stay: there the
	// shape *is* the boundary, since without it that endpoint is "delete any
	// folder on the server by guessing integers".
	server_.Get("/rest/moveAlbum.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		auto id_it = req.params.find("id");
		if (id_it == req.params.end()) { err(10, "Missing parameter: id."); return; }
		int folder_id = 0;
		try { folder_id = std::stoi(id_it->second); }
		catch (...) { err(10, "Parameter id must be a number."); return; }

		auto rel_opt = store_.album_folder_path_by_id(folder_id);
		if (!rel_opt) { err(70, "Item not found."); return; }
		const std::string old_rel = *rel_opt;

		namespace fs = std::filesystem;
		std::vector<std::string> parts;
		// const auto&, not auto&: path::iterator's reference type is
		// implementation-defined. libstdc++ hands out a const path&, but libc++
		// returns a path by value, and a non-const lvalue reference cannot bind
		// to that temporary. const& works on both — it binds the reference on
		// libstdc++ and lifetime-extends the temporary on libc++.
		for (const auto& c : fs::path(old_rel)) parts.push_back(c.string());
		if (parts.size() < 2) { err(0, "Could not resolve the library path."); return; }

		// ---- Permission, first cut -------------------------------------
		//
		// The full rule — an upload user may reorganise inside their own
		// batch, naming a destination root is admin's alone — needs the
		// destination parsed and so still runs below.  What cannot wait is
		// the refusal: the shape-specific errors between here and there (is
		// it a file, how deep does it sit) were an oracle a non-admin could
		// walk over ids in other users' uploads.  A foreign upload answers
		// exactly like a missing id; a shared-library item tells a non-admin
		// no before its shape is examined.
		{
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		if (!(ui && ui->is_admin)) {
			if (!item_read_allowed(req, store_, uploads_root_name_, old_rel)) {
				err(70, "Item not found."); return;
				}
			const bool own_upload = !uploads_root_name_.empty()
			                        && parts[0] == uploads_root_name_;
			if (!own_upload) {
				err(50, "Moving outside your own uploads requires admin role.");
				return;
				}
			}
		}

		// **The album may be a single media file.**  A loose file is its own
		// album, so its folder row's path names the file — and every step
		// below that assumes a directory has to be told.  The filesystem is
		// asked rather than the database because what follows is filesystem
		// work: what matters is what is actually there to move.
		std::error_code sec;
		const bool file_album =
			fs::is_regular_file(store_.abs_path(old_rel), sec);

		const std::string old_album = parts.back();
		// The name, as opposed to the on-disk leaf: for a file-album they
		// differ by the extension, which is nobody's idea of part of a title
		// and must not reach an album tag.
		const std::string src_ext =
			file_album ? fs::path(old_album).extension().string() : std::string{};
		const std::string old_album_name =
			file_album ? fs::path(old_album).stem().string() : old_album;
		// The artist level above the album.  A level-1 file-album has none of
		// its own and the root plays the part, which is what parts[0] is here
		// — the same thing ALBUM_ARTIST_ID_SQL reports as its artist.
		const std::string old_folder = parts[parts.size() - 2];

		// ---- Where it lands -------------------------------------------
		std::string new_parent_dir;   // the artist/category directory it goes in
		std::string dest_root_type;   // "artists" or "categories"

		auto mf_it = req.params.find("musicFolderId");
		const bool rerooting = (mf_it != req.params.end() && !mf_it->second.empty());

		std::string new_folder;
		if (req.params.count("folder")) {
			new_folder = sanitise_component(
				utf8_clean(req.params.find("folder")->second, 200));
			if (new_folder.empty()) {
				err(10, "The folder name contains nothing usable."); return;
				}
			}

		if (rerooting) {
			auto mf = store_.music_folder_by_id(to_int(mf_it->second, 0));
			if (!mf) { err(70, "No such library folder."); return; }
			// **folder is required when a root is named**, and that is not an
			// oversight carried over.  It was briefly optional and defaulted to
			// the source's own level-1 name, which for a fetched video is the
			// channel that published it and never a category — a guess that
			// filed documentaries under YouTube channel names.  A caller that
			// does not know where something belongs should be made to decide.
			if (new_folder.empty()) {
				err(10, "Missing parameter: folder."); return;
				}
			new_parent_dir = mf->name + "/" + new_folder;
			dest_root_type = mf->type;
			}
		else {
			// Staying put: everything above the artist level is untouched — the
			// library root, and for an upload the owner and the batch id.
			//
			// A level-1 file-album is the exception: a loose film in a root
			// used as one flat library has no artist directory, the root
			// itself plays that part, and renaming such a film in place is the
			// commonest thing this endpoint gets asked to do.  Its parent is
			// the root, which is a real directory, so there is nothing here
			// that needs one to be invented.
			if (parts.size() < 3 && !file_album) {
				err(0, "This folder is its own artist; move it to a named root, "
				       "or rename it on the server.");
				return;
				}
			if (parts.size() < 3 && !new_folder.empty() && new_folder != parts[0]) {
				err(0, "This item sits directly in a library root; name a root "
				       "with musicFolderId to file it under a folder.");
				return;
				}
			if (new_folder.empty()) new_folder = old_folder;
			if (parts.size() < 3)
				// The root is the parent; there is no level above it to keep.
				new_parent_dir = parts[0];
			else {
				std::string above;
				for (size_t i = 0; i + 2 < parts.size(); ++i)
					above += (i ? "/" : "") + parts[i];
				new_parent_dir = above + "/" + new_folder;
				}
			// The uploads root is not in get_music_folders() by design, so it
			// matches nothing here and falls through to "artists" — which is
			// what an upload's level-1 directory is.
			dest_root_type = "artists";
			for (const auto& f : store_.get_music_folders())
				if (f.name == parts[0]) { dest_root_type = f.type; break; }
			}

		std::string new_album = old_album_name;
		if (req.params.count("album")) {
			new_album = sanitise_component(
				utf8_clean(req.params.find("album")->second, 200));
			if (new_album.empty()) {
				err(10, "The album name contains nothing usable."); return;
				}
			}

		// **The on-disk leaf carries the source's extension back.**
		// sanitise_component() strips nothing but it is handed a *name*, and a
		// file-album renamed to that name alone lands on disk with no
		// extension — at which point is_media_file() stops recognising it and
		// the next scan prunes the film out of the library entirely.  A caller
		// that typed the extension is not made to type it twice.
		std::string new_leaf = new_album;
		if (file_album && !src_ext.empty()
		        && !(new_leaf.size() > src_ext.size()
		             && ends_with_ci(new_leaf, src_ext)))
			new_leaf += src_ext;

		const std::string new_rel = new_parent_dir + "/" + new_leaf;
		// Everything above the album, as it stands now.
		std::string old_parent_dir;
		for (size_t i = 0; i + 1 < parts.size(); ++i)
			old_parent_dir += (i ? "/" : "") + parts[i];

		// ---- Permission ------------------------------------------------
		//
		// Admin, except that an upload user may reorganise their own batch
		// inside itself — the case this feature exists for, since somebody who
		// has just uploaded has to be able to fix the names.  Anything that
		// names a destination root is putting something into the shared
		// library and is admin's alone.
		{
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		const bool is_admin = ui && ui->is_admin;
		const bool own_upload = !uploads_root_name_.empty()
		                        && parts[0] == uploads_root_name_
		                        && parts.size() >= 5
		                        && !uname.empty() && parts[1] == uname;
		if (!is_admin && !(own_upload && !rerooting)) {
			err(50, rerooting
			        ? "Moving into the shared library requires admin role."
			        : "Moving outside your own uploads requires admin role.");
			return;
			}
		}

		if (new_rel == old_rel) {
			// Nothing to do, but answer with the ids so a client need not
			// special-case a no-op edit.
			int aid = 0, pid = 0;
			if (auto f = store_.folder_id_by_path(old_rel))        aid = *f;
			if (auto f = store_.folder_id_by_path(old_parent_dir)) pid = *f;
			res.set_content(use_json
				? subsonic_ok_json([&](nlohmann::json& r) {
					r["movedAlbum"] = {{"id",     std::to_string(aid)},
					                   {"parent", std::to_string(pid)},
					                   {"album",  new_album},
					                   {"artist", new_folder},
					                   {"tagFailures", 0}};
					})
				: subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
					auto* el = doc.NewElement("movedAlbum");
					el->SetAttribute("id",          std::to_string(aid).c_str());
					el->SetAttribute("parent",      std::to_string(pid).c_str());
					el->SetAttribute("album",       new_album.c_str());
					el->SetAttribute("artist",      new_folder.c_str());
					el->SetAttribute("tagFailures", 0);
					root->InsertEndChild(el);
					}),
				use_json ? "application/json" : "application/xml");
			return;
			}

		// ---- The move --------------------------------------------------
		fs::path abs_src    = store_.abs_path(old_rel);
		fs::path abs_target = store_.abs_path(new_rel);
		if (abs_src.empty() || abs_target.empty()) {
			err(0, "Could not resolve the library path."); return;
			}
		// Belt and braces over sanitise_component's traversal guard.
		// path_is_within_root uses weakly_canonical, so it answers for a target
		// that does not exist yet.
		if (!store_.path_is_within_root(abs_target)) {
			std::cout << stamp() << "moveAlbum: refusing target outside every root: "
			          << abs_target << std::endl;
			err(0, "Refusing to write outside the configured roots.");
			return;
			}
		if (fs::exists(abs_target)) {
			// Not "for that artist": the destination may be a category.
			err(0, "Something with that name is already in that folder.");
			return;
			}

		std::error_code ec;
		// Collected before the rename, since they are named after the file
		// where it is now.  A file-album's cover, poster, subtitles, chapter
		// markers and liner notes are separate files beside it, and a move
		// that left them behind would strand every one of them somewhere
		// nothing will ever reconnect them from.
		const std::vector<std::string> sidecars =
			file_album ? store_.sidecars_of(abs_src.string())
			           : std::vector<std::string>{};

		// Creating the destination is what makes "type a name that is not in
		// the list" the way to add an artist or a category, with no separate
		// operation for it.
		fs::create_directories(store_.abs_path(new_parent_dir), ec);

		fs::rename(abs_src, abs_target, ec);
		if (ec) {
			// Cross-device: fall back to recursive copy then remove.
			fs::copy(abs_src, abs_target, fs::copy_options::recursive, ec);
			if (ec) { err(0, ("Failed to move item: " + ec.message()).c_str()); return; }
			fs::remove_all(abs_src, ec);
			}

		std::cout << stamp() << "Move: " << old_rel << " → " << new_rel << std::endl;

		// Carry the DB across with the directory.  Without this the rescan
		// below deletes the old folder and inserts the new one cold, and every
		// star, play count, playlist entry and bookmark — all keyed on the path
		// string — quietly stops matching anything, along with the hand-picked
		// cover and any typed video title, and the derived art caches.
		store_.relocate_prefix(old_rel, new_rel);

		// The sidecars follow, each one its own tiny relocate.  The media
		// file's own move above already carried songs.path, chapters.path and
		// the rest; what these calls are for is the columns that name the
		// *image*: cover_thumbs.source_key, songs.cover_path,
		// albums.cover_path.  A rescan would repair the last two and never the
		// first, so the thumbnails would go on being keyed on a path with no
		// file at it.
		//
		// The suffix is whatever follows the old stem — ".jpg", but equally
		// "-poster.jpg" or ".chapters.txt", which is why it is taken by length
		// rather than by extension().
		if (file_album) {
			const std::string old_stem = fs::path(abs_src).stem().string();
			const fs::path    new_stem =
				abs_target.parent_path() / abs_target.stem();
			for (const auto& s : sidecars) {
				std::string fname = fs::path(s).filename().string();
				if (fname.size() <= old_stem.size()) continue;
				std::string suffix = fname.substr(old_stem.size());
				fs::path    dest   = fs::path(new_stem.string() + suffix);
				std::error_code sc;
				if (fs::exists(dest, sc)) continue;   // never overwrite
				fs::rename(s, dest, sc);
				if (sc) {
					std::cout << stamp() << "Move: could not move sidecar " << s
					          << ": " << sc.message() << std::endl;
					continue;
					}
				store_.relocate_prefix(store_.rel_path(s),
				                        store_.rel_path(dest.string()));
				}
			}

		// ---- Then the tags ---------------------------------------------
		//
		// The opposite order to updateSong, which writes tags first so that a
		// failure cannot be recorded in the DB.  There the DB mirrors the tag;
		// here it mirrors the *directory*, so the move is the authoritative act
		// and a file TagLib will not write is a cosmetic loss rather than a
		// desync.  Failures are counted, not fatal.
		//
		// **The artist tag is only written under an artists root.**  Under a
		// categories root the level is a category — Film, Series — and writing
		// that into an artist tag would stamp "Film" across every file of a
		// promoted documentary.  Neither endpoint this replaces got that right:
		// promote wrote no tags at all, so a promoted film kept its channel
		// name, and rename wrote both unconditionally, which was safe only
		// because it could never reach a categories root.
		// Compared as *names*, so a file-album's extension does not make every
		// rename look like an album change — and never reaches a tag.
		const bool write_album  = (new_album  != old_album_name);
		const bool write_artist = (new_folder != old_folder)
		                          && dest_root_type == "artists";
		int tag_failures = 0;
		auto tag_one = [&](const fs::path& p) {
			try {
				TagLib::FileRef f(p.c_str());
				if (f.isNull() || !f.tag()) return;   // not a taggable file
				if (write_album)
					f.tag()->setAlbum(TagLib::String(new_album, TagLib::String::UTF8));
				// Only overwrite an artist tag that was the old folder's
				// name anyway.  A tag naming somebody else is a real
				// credit — every track of a compilation has one — and this
				// used to flatten the lot to "Various Artists" on a move.
				// Worse, saving bumps the mtime, so the next scan re-read
				// the value it had just destroyed and the library
				// converged on it with nothing left to recover from.
				if (write_artist) {
					std::string cur = f.tag()->artist().to8Bit(true);
					if (cur.empty() || artist_key(cur) == artist_key(old_folder))
						f.tag()->setArtist(
							TagLib::String(new_folder, TagLib::String::UTF8));
					}
				if (!f.save()) {
					++tag_failures;
					std::cout << stamp() << "Move: could not write tags to "
					          << p << std::endl;
					}
				}
			catch (const std::exception& ex) {
				++tag_failures;
				std::cout << stamp() << "Move: exception tagging " << p
				          << ": " << ex.what() << std::endl;
				}
			catch (...) {
				++tag_failures;
				std::cout << stamp() << "Move: unknown exception tagging "
				          << p << std::endl;
				}
			};
		if (write_album || write_artist) {
			// One file or a directory of them.  The walk is not merely
			// unnecessary for a file-album: recursive_directory_iterator on a
			// regular file sets `ec` and iterates zero times, so the rename
			// would report tagFailures 0 having written nothing at all.
			if (file_album)
				tag_one(abs_target);
			else
				for (auto& e : fs::recursive_directory_iterator(abs_target, ec)) {
					if (!e.is_regular_file()) continue;
					tag_one(e.path());
					}
			}

		// An emptied source directory is gone as far as the library is
		// concerned; removing it lets the rescan below prune its folder row.
		// Left in place it is an artist reading "0 albums", which is the shape
		// every stranding here takes.
		//
		// **Only when there was an artist level to empty.**  A level-1
		// file-album — a loose film in a root used as one flat library — leaves
		// parts.size() == 2, and its "parent" is the root itself, which
		// fs::is_empty would happily report on and fs::remove would then delete
		// out from under the server.  (Before a loose file became its own
		// album, the case was a loose-file *section* moved out of a root; the
		// arithmetic and the hazard are the same.)
		fs::path old_parent_abs = store_.abs_path(old_parent_dir);
		bool old_parent_gone = false;
		if (parts.size() >= 3
		        && fs::is_directory(old_parent_abs, ec)
		        && fs::is_empty(old_parent_abs, ec)) {
			fs::remove(old_parent_abs, ec);
			old_parent_gone = true;
			}
		// And the batch directory above it, as deleteUpload does: the <uuid>
		// level never gets a folder row of its own — scan_artist_dir parents an
		// artist directory straight to the root — so there is nothing to
		// rescan for it, only a directory to not leave behind.
		if (old_parent_gone && !uploads_root_name_.empty()
		        && parts[0] == uploads_root_name_ && parts.size() >= 5) {
			fs::path batch_abs = old_parent_abs.parent_path();
			if (fs::is_directory(batch_abs, ec) && fs::is_empty(batch_abs, ec))
				fs::remove(batch_abs, ec);
			}

		// Synchronously, so the response is never ahead of the database. After
		// the relocate above this is a consistency pass rather than a rebuild:
		// the rows already carry the new paths, so the walk re-stamps them and
		// prunes nothing. It is also what re-establishes the album's artist
		// link, which relocate_prefix dropped — so a contended database here
		// would leave the album with no artist until the next scan, and that is
		// worth a log line rather than a 500 on a move that has already
		// happened.
		//
		// DESTINATION FIRST, and it must be two calls rather than one set:
		// scan_dirs takes a std::set, so a single call would scan them in
		// whatever order the names happen to sort in. The relocate moved the
		// album folder's *path* out from under the old parent but left its
		// parent_id pointing at the old parent's row, and folders.parent_id has
		// no ON DELETE CASCADE. Tearing down the source first therefore hits
		// FOREIGN KEY constraint failed, which rolls the whole prune back and
		// leaves the emptied artist behind — with no error anywhere, because
		// the prune catches. Scanning the destination first re-points
		// parent_id, and the source then deletes cleanly.
		//
		// **A level-1 file-album is rescanned by its own path, not its
		// parent's.** Its parent is the bare root name, which scan_dirs()
		// deliberately escalates to a full scan() — minutes of work to settle
		// a rename. Handing it the file path instead lands in the file-album
		// branch of scan_dirs(), which calls scan_root_files() for that root
		// and nothing else. Both ends are checked separately because a move
		// can be level-1 at one end only.
		//
		// The ordering rule is unaffected: for such a move both ends are in
		// the same root and neither re-parents anything, so there is no
		// dangling parent_id for the source pass to trip over.
		auto components = [](const std::string& rel) {
			int n = 0;
			for (const auto& c : fs::path(rel)) { (void)c; ++n; }
			return n;
			};
		const std::string scan_dest = (components(new_rel) < 3) ? new_rel
		                                                        : new_parent_dir;
		const std::string scan_src  = (parts.size() < 3) ? old_rel
		                                                 : old_parent_dir;
		try {
			store_.scan_dirs({scan_dest});
			if (scan_src != scan_dest)
				store_.scan_dirs({scan_src});
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "Move: rescan failed: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Move: rescan failed: unknown exception"
			          << std::endl;
			}

		// The client needs the new ids to navigate to what it just moved. They
		// are usually unchanged — that is the point of relocating rather than
		// letting the rescan rebuild — but a move into a directory that did not
		// exist yet mints one.
		int new_album_id  = 0;
		int new_parent_id = 0;
		if (auto f = store_.folder_id_by_path(new_rel))        new_album_id  = *f;
		if (auto f = store_.folder_id_by_path(new_parent_dir)) new_parent_id = *f;

		std::string body = use_json
			? subsonic_ok_json([&](nlohmann::json& r) {
				r["movedAlbum"]["id"]          = std::to_string(new_album_id);
				r["movedAlbum"]["parent"]      = std::to_string(new_parent_id);
				r["movedAlbum"]["album"]       = new_album;
				r["movedAlbum"]["artist"]      = new_folder;
				r["movedAlbum"]["tagFailures"] = tag_failures;
				})
			: subsonic_ok([&](XMLDocument& doc, XMLElement* root) {
				auto* el = doc.NewElement("movedAlbum");
				el->SetAttribute("id",          std::to_string(new_album_id).c_str());
				el->SetAttribute("parent",      std::to_string(new_parent_id).c_str());
				el->SetAttribute("album",       new_album.c_str());
				el->SetAttribute("artist",      new_folder.c_str());
				el->SetAttribute("tagFailures", tag_failures);
				root->InsertEndChild(el);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// deleteUpload — remove one of the caller's own uploaded albums.
	//
	// Param: id (album folder_id). The way out of a fetch that produced the
	// wrong thing; before this the only route out of the uploads area was
	// promoting into the shared library, or shell access to the server.
	//
	// **The path check below is the security boundary, not a validation
	// nicety.** Without it this is "delete the folder with this id", which is
	// "delete any folder on the server", reachable by any account with upload
	// rights guessing integers. Two things have to hold whoever is asking: the
	// path is exactly five components, and the first is the uploads root.
	//
	// The third — that the second component is the caller — holds for everyone
	// but an admin. An admin may delete anybody's upload, because an admin is
	// who approves them: `personal=*` lets them see everyone's, and being able
	// to see junk without being able to clear it would leave them asking its
	// owner to do it.
	//
	// That widening is also what keeps the clients simple. Both gate the action
	// on "did I reach this through Uploads" and nothing else, which stays
	// correct precisely because a non-admin can only ever reach their own.
	server_.Get("/rest/deleteUpload.view", [this](const httplib::Request& req,
	                                               httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");

		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                         : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};

		if (!check_upload_perm(req, res, store_, use_json)) return;
		const std::string uname = req.get_param_value("u");
		auto ui = store_.get_user(uname);
		const bool is_admin = ui && ui->is_admin;

		auto id_it = req.params.find("id");
		if (id_it == req.params.end()) { err(10, "Missing parameter: id."); return; }

		auto rel_opt = store_.album_folder_path_by_id(to_int(id_it->second, 0));
		if (!rel_opt) { err(70, "Item not found."); return; }
		const std::string& item_rel = *rel_opt;

		namespace fs = std::filesystem;
		fs::path rel_p(item_rel);
		std::vector<std::string> parts;
		// const auto&, not auto&: path::iterator's reference type is
		// implementation-defined. libstdc++ hands out a const path&, but libc++
		// returns a path by value, and a non-const lvalue reference cannot bind
		// to that temporary.
		for (const auto& c : rel_p) parts.push_back(c.string());
		if (parts.size() != 5 || uploads_root_name_.empty()
		        || parts[0] != uploads_root_name_
		        || (parts[1] != uname && !is_admin)) {
			// One message for "not an upload" and "not yours", deliberately: the
			// difference is only useful to somebody probing ids.
			err(0, "Item is not in your uploads.");
			return;
			}
		// The *owner's* name, which is the caller's own except for an admin
		// clearing somebody else's — and it is the owner's directories that get
		// tidied up below, not the caller's.
		const std::string& owner       = parts[1];
		const std::string& batch_uuid  = parts[2];
		const std::string& artist_name = parts[3];

		fs::path abs = store_.abs_path(item_rel);
		// Belt and braces over the component check, the pairing moveAlbum
		// keeps: that establishes the shape, this establishes that the shape
		// resolves to somewhere inside a root. A symlink escaping to /etc
		// prefixes none of them.
		if (!store_.path_is_within_root(abs)) {
			err(0, "Item is not in your uploads."); return;
			}

		std::error_code ec;
		// A file-album's companions go with it.  remove_all() on a directory
		// takes everything inside; on a single media file it takes the file
		// and leaves the cover, poster, subtitles, chapter markers and liner
		// notes sitting in the section, where nothing afterwards will ever
		// remove them and nothing will ever use them again.  Collected before
		// the delete, since they are found by the file's own name.
		std::vector<std::string> sidecars;
		if (fs::is_regular_file(abs, ec)) sidecars = store_.sidecars_of(abs.string());

		fs::remove_all(abs, ec);
		if (ec) { err(0, ("Failed to delete item: " + ec.message()).c_str()); return; }
		for (const auto& s : sidecars) {
			std::error_code sc;
			fs::remove(s, sc);
			store_.forget_prefix(store_.rel_path(s));
			}

		// Before the rescan, and it is the half the rescan will not do: the
		// scanner prunes the music DB and its derived caches but has never
		// touched the client schema, so stars, play counts, playlist entries,
		// the queue and bookmarks would outlive the files. That matters more
		// than it used to — a later batch folded into an earlier one can put a
		// new file at exactly this path, and a surviving star would attach
		// itself to it.
		store_.forget_prefix(item_rel);

		// An emptied artist directory left in place is a folder row reading
		// "0 albums" in the listing, which is the shape every stranding here
		// takes. Removing it makes scan_artist_dir treat it as gone and prune
		// it — the same two steps moveAlbum takes for the same reason.
		std::string artist_rel =
			uploads_root_name_ + "/" + owner + "/" + batch_uuid + "/" + artist_name;
		fs::path artist_abs = store_.abs_path(artist_rel);
		if (fs::is_directory(artist_abs, ec) && fs::is_empty(artist_abs, ec))
			fs::remove(artist_abs, ec);

		// Synchronously, so the response is never ahead of the database. One
		// call covers both outcomes: the artist directory either still stands
		// with fewer albums, or has gone and is pruned.
		store_.scan_dirs({artist_rel});

		// And the batch after it. Nothing to rescan for this one — the <uuid>
		// level never gets a folder row, because scan_artist_dir parents an
		// artist directory straight to the root.
		fs::path batch_abs = artist_abs.parent_path();
		if (fs::is_directory(batch_abs, ec) && fs::is_empty(batch_abs, ec))
			fs::remove(batch_abs, ec);

		// Says who asked as well as what went, because those differ when an
		// admin clears somebody else's and the log is the only record of it.
		std::cout << stamp() << "Delete upload: " << uname << " removed "
		          << item_rel << std::endl;

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// ---- Fetching from a URL ------------------------------------------
	//
	// The second producer for a personal batch, beside /upload's archive. The
	// user pastes a URL, the server matches it against the configured handler
	// table and runs whatever tool that handler names.
	//
	// **A URL matching no handler is refused, and that refusal is the security
	// boundary** — see urlfetch.hh. There is no fallback handler, and adding one
	// would turn any account allowed to upload into a way of making the server
	// issue arbitrary outbound requests from inside the network.

	// getUrlHandlers — what this server can fetch. Any account that may upload
	// can ask; an empty list is what a client keys "do not offer the row" on,
	// which is what makes an install with no yt-dlp simply not show it.
	//
	// The pattern and the argv are deliberately not reported. An argv can carry
	// --cookies, a proxy credential or an API key, and the pattern is operator
	// configuration a client cannot act on anyway.
	server_.Get("/rest/getUrlHandlers.view", [this](const httplib::Request& req,
	                                                 httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_upload_perm(req, res, store_, use_json)) return;

		auto caps = url_fetcher_.capabilities();
		std::string body;
		if (use_json)
			body = subsonic_ok_json([&caps](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& c : caps)
					arr.push_back({{"name",  utf8_clean(c.name, 200)},
					               {"audio", c.audio},
					               {"video", c.video}});
				r["urlHandlers"] = {{"urlHandler", arr}};
				});
		else
			body = subsonic_ok([&caps](XMLDocument& doc, XMLElement* root) {
				auto* list = doc.NewElement("urlHandlers");
				for (const auto& c : caps) {
					auto* e = doc.NewElement("urlHandler");
					e->SetAttribute("name",  c.name.c_str());
					e->SetAttribute("audio", c.audio);
					e->SetAttribute("video", c.video);
					list->InsertEndChild(e);
					}
				root->InsertEndChild(list);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// fetchUrl — queue a fetch. Params: url, mode (audio|video, default audio).
	// Returns at once with a job id; getFetchJobs reports on it.
	server_.Get("/rest/fetchUrl.view", [this](const httplib::Request& req,
	                                           httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                        : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		if (!check_upload_perm(req, res, store_, use_json)) return;

		auto qp = [&](const std::string& k) -> std::string {
			auto it = req.params.find(k);
			return it != req.params.end() ? it->second : "";
			};

		std::string uname = qp("u");
		std::string url   = qp("url");
		std::string mode  = qp("mode");
		bool audio = (mode != "video");
		if (!mode.empty() && mode != "audio" && mode != "video") {
			err(0, "mode must be audio or video."); return;
			}

		if (url.empty())     { err(10, "Required parameter missing."); return; }
		if (users_dir_.empty()) {
			err(0, "No uploads root is configured on this server."); return;
			}
		// Reported separately from "no handler" so the message names the real
		// problem: a pattern is never consulted for a scheme we refuse outright.
		if (!urlfetch_http_url(url)) {
			err(0, "Only http and https URLs can be fetched."); return;
			}
		const UrlHandler* h = url_fetcher_.match(url);
		if (!h) { err(0, "No handler is configured for that URL."); return; }
		if ((audio && h->audio_argv.empty()) || (!audio && h->video_argv.empty())) {
			err(0, audio ? "That handler cannot fetch audio."
			             : "That handler cannot fetch video.");
			return;
			}

		// The two optional names, checked last because they are the only
		// optional thing: everything above answers "can this URL be fetched at
		// all", which is the security boundary, and a request that was going to
		// be refused for the URL should say so rather than complain about a
		// name it would never have used.
		//
		// Blank — absent, or nothing but whitespace — means "keep whatever the
		// handler chose", so a client can send both fields unconditionally. A
		// name that is *not* blank but sanitises away to nothing was typed and
		// is wrong; moveAlbum answers the identical question the same way,
		// and for the same reason: filing it under "Unknown" would hide the
		// mistake. That substitution belongs to reorganise_by_tags, where
		// nobody typed anything.
		//
		// utf8_clean before sanitise_component, not the other way round. These
		// become directory names and from there folders.path, and invalid UTF-8
		// reaching dump() is a bare 500 on an httplib thread; cleaning it can
		// also expose a new trailing space, which sanitise_component must then
		// get the chance to strip. The 200-byte cap keeps the name well under
		// NAME_MAX.
		auto typed = [&](const char* key, std::string& out) -> bool {
			std::string raw = qp(key);
			if (raw.find_first_not_of(" \t") == std::string::npos) return true;
			out = sanitise_component(utf8_clean(raw, 200));
			return !out.empty();
			};
		std::string want_artist, want_album;
		if (!typed("artist", want_artist)) {
			err(10, "The artist name contains nothing usable."); return;
			}
		if (!typed("album", want_album)) {
			err(10, "The album name contains nothing usable."); return;
			}

		FetchJob job;
		job.id      = make_uuid();
		job.batch   = uploads_root_name_ + "/" + uname + "/" + job.id;
		job.user    = uname;
		job.url     = url;
		job.handler = utf8_clean(h->name, 200);
		job.audio   = audio;
		job.artist  = want_artist;
		job.album   = want_album;
		job.started = static_cast<int64_t>(std::time(nullptr));

		{
		std::lock_guard<std::mutex> lk(fetch_mu_);
		// The same URL twice is a double-click, not two wants. Two batches of
		// one video is the outcome without this — the same reason the portrait
		// worker keeps a queued set.
		for (const auto& j : fetch_jobs_)
			if (j.user == uname && j.url == url
			    && (j.state == "queued" || j.state == "running"
			        || j.state == "scanning")) {
				err(0, "That URL is already being fetched.");
				return;
				}
		if (fetch_queue_.size() >= FETCH_QUEUE_MAX) {
			err(0, "Too many fetches are already queued."); return;
			}
		fetch_jobs_.push_back(job);
		fetch_queue_.push_back(job.id);
		}
		fetch_cv_.notify_one();

		std::cout << stamp() << "url fetch: queued " << job.id << " for "
		          << uname << " (" << h->name << ", "
		          << (audio ? "audio" : "video") << "): " << url << std::endl;

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&job](nlohmann::json& r) {
				r["fetchJob"] = {{"id",      job.id},
				                 {"batch",   job.batch},
				                 {"handler", job.handler},
				                 {"mode",    job.audio ? "audio" : "video"},
				                 {"artist",  job.artist},
				                 {"album",   job.album},
				                 {"state",   job.state}};
				});
		else
			body = subsonic_ok([&job](XMLDocument& doc, XMLElement* root) {
				auto* e = doc.NewElement("fetchJob");
				e->SetAttribute("id",      job.id.c_str());
				e->SetAttribute("batch",   job.batch.c_str());
				e->SetAttribute("handler", job.handler.c_str());
				e->SetAttribute("mode",    job.audio ? "audio" : "video");
				e->SetAttribute("artist",  job.artist.c_str());
				e->SetAttribute("album",   job.album.c_str());
				e->SetAttribute("state",   job.state.c_str());
				root->InsertEndChild(e);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// getFetchJobs — the caller's own jobs, newest first. Admins see their own
	// too and not everyone else's: this is a progress display, not an audit log.
	server_.Get("/rest/getFetchJobs.view", [this](const httplib::Request& req,
	                                                httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		if (!check_upload_perm(req, res, store_, use_json)) return;

		std::string uname = req.get_param_value("u");
		std::vector<FetchJob> mine;
		{
		std::lock_guard<std::mutex> lk(fetch_mu_);
		for (auto it = fetch_jobs_.rbegin(); it != fetch_jobs_.rend(); ++it)
			if (it->user == uname) mine.push_back(*it);
		}
		// Scrubbed on the way out rather than on the way in: the stored url is
		// what gets fetched and must stay byte-exact, while what leaves here is
		// serialised — and a percent-decoded query parameter is arbitrary
		// bytes. See utf8_clean.
		//
		// artist and album are absent from this list on purpose. They were
		// cleaned in fetchUrl, because they become directory names there and a
		// directory name has to be valid before it is created, not before it is
		// reported.
		for (auto& j : mine) {
			j.url     = utf8_clean(j.url,     2048);
			j.handler = utf8_clean(j.handler, 200);
			j.error   = utf8_clean(j.error,   400);
			}

		std::string body;
		if (use_json)
			body = subsonic_ok_json([&mine](nlohmann::json& r) {
				nlohmann::json arr = nlohmann::json::array();
				for (const auto& j : mine)
					arr.push_back({{"id",       j.id},
					               {"batch",    j.batch},
					               {"handler",  j.handler},
					               {"mode",     j.audio ? "audio" : "video"},
					               {"artist",   j.artist},
					               {"album",    j.album},
					               {"url",      j.url},
					               {"state",    j.state},
					               {"percent",  j.percent},
					               {"detail",   j.detail},
					               {"error",    j.error},
					               {"files",    j.files},
					               {"started",  j.started},
					               {"finished", j.finished}});
				r["fetchJobs"] = {{"fetchJob", arr}};
				});
		else
			body = subsonic_ok([&mine](XMLDocument& doc, XMLElement* root) {
				auto* list = doc.NewElement("fetchJobs");
				for (const auto& j : mine) {
					auto* e = doc.NewElement("fetchJob");
					e->SetAttribute("id",       j.id.c_str());
					e->SetAttribute("batch",    j.batch.c_str());
					e->SetAttribute("handler",  j.handler.c_str());
					e->SetAttribute("mode",     j.audio ? "audio" : "video");
					e->SetAttribute("artist",   j.artist.c_str());
					e->SetAttribute("album",    j.album.c_str());
					e->SetAttribute("url",      j.url.c_str());
					e->SetAttribute("state",    j.state.c_str());
					e->SetAttribute("percent",  j.percent);
					e->SetAttribute("detail",   j.detail.c_str());
					e->SetAttribute("error",    j.error.c_str());
					e->SetAttribute("files",    j.files);
					e->SetAttribute("started",  (int64_t)j.started);
					e->SetAttribute("finished", (int64_t)j.finished);
					list->InsertEndChild(e);
					}
				root->InsertEndChild(list);
				});
		res.set_content(body, use_json ? "application/json" : "application/xml");
		});

	// cancelFetch — stop a queued or running fetch. Param: id.
	//
	// The ownership check is not a formality: without it any account that may
	// upload could cancel anybody else's fetch by guessing nothing at all, since
	// the id is handed to the client that started it.
	server_.Get("/rest/cancelFetch.view", [this](const httplib::Request& req,
	                                              httplib::Response& res) {
		if (!check_auth(req, res, store_)) return;
		bool use_json = (fmt_of(req) == "json");
		auto err = [&](int code, const char* msg) {
			res.set_content(use_json ? subsonic_error_json(code, msg)
			                        : subsonic_error(code, msg),
			                use_json ? "application/json" : "application/xml");
			};
		if (!check_upload_perm(req, res, store_, use_json)) return;

		std::string uname = req.get_param_value("u");
		std::string id    = req.get_param_value("id");
		if (id.empty()) { err(10, "Required parameter missing."); return; }
		auto ui = store_.get_user(uname);
		bool is_admin = ui && ui->is_admin;

		bool found = false;
		{
		std::lock_guard<std::mutex> lk(fetch_mu_);
		for (auto& j : fetch_jobs_) {
			if (j.id != id) continue;
			if (j.user != uname && !is_admin) break;   // report as not found
			found = true;
			if (j.state == "queued" || j.state == "running") {
				// Marked here rather than by the worker so the state is set
				// before the child dies: the worker sees a non-zero exit and
				// would otherwise call it an error.
				j.state    = "cancelled";
				j.finished = static_cast<int64_t>(std::time(nullptr));
				}
			break;
			}
		}
		if (!found) { err(70, "Item not found."); return; }
		url_fetcher_.cancel(id);
		std::cout << stamp() << "url fetch: cancelled " << id << std::endl;

		res.set_content(use_json ? subsonic_ok_json() : subsonic_ok(),
		                use_json ? "application/json" : "application/xml");
		});

	// Catch-all for endpoints not yet implemented.
	server_.Get("/rest/:endpoint", [](const httplib::Request& req, httplib::Response& res) {
		// log_safe: this is the one unauthenticated route that echoed the
		// percent-decoded path into the log verbatim.
		std::cout << stamp() << "NOT IMPLEMENTED: " << log_safe(req.path) << std::endl;
		res.set_content(subsonic_error(0, "Not implemented."), "application/xml");
		});

	if (!store_.has_users())
		std::cout << stamp()
		          << "WARNING: no users in database. "
		             "Create one with --add-user <name> --password <pass>."
		          << std::endl;

	// Background work (library scan, Cast discovery, folder watching) is NOT
	// started here — listen() starts it once the port is actually held.
	// Starting it in the constructor meant a server that could not bind still
	// spent minutes scanning, and could not exit promptly either: the detached
	// scan holds db_mutex_, so the watcher's join in the destructor blocks
	// behind it.  Nothing should run until we know we can serve.
	no_scan_ = no_scan;
	}

GainDrive::~GainDrive()
	{
	// The fetch worker first: it is the one that can be inside scan_dirs(), and
	// joining it here — in the destructor body — is what guarantees it is not
	// still holding store_ when the members are destroyed.
	fetch_stop_ = true;
	fetch_cv_.notify_all();
	// A fetch is allowed to run for hours, and the worker checks the stop flag
	// only between jobs — so without killing the child, this join is the
	// shutdown. The half-written batch is removed by the worker's own failure
	// path on the way out.
	url_fetcher_.cancel_any();
	if (fetch_thread_.joinable()) fetch_thread_.join();

	portrait_stop_ = true;
	portrait_cv_.notify_all();
	if (portrait_thread_.joinable()) portrait_thread_.join();
	}

// Normalise what a producer wrote into a batch, then scan it.
//
// Shared by /upload and the URL fetcher, which differ only in what put the
// files there. The order matters: tags first, since they are the better answer
// where they exist, then the depth fix for whatever had none.
void GainDrive::scan_batch(const std::string& rel_batch,
                           const std::filesystem::path& dest,
                           const std::string& fallback_artist,
                           const std::string& artist_override,
                           const std::string& album_override)
	{
	try {
		// First, while every file is still exactly where the tool's own -o
		// template put it: this pairs a .info.json with its media by name, and
		// every step below may rename one of them.
		convert_tool_sidecars(dest);
		// Tags first, and never skipped even when both names were typed: this
		// is what derives an album from a file's own metadata, drags a sibling
		// cover along with the audio it belongs to, and prunes what it empties.
		// Renaming before it would let a still-loose file scatter into a
		// tag-named pair *beside* the typed one. It costs one redundant rename.
		reorganise_by_tags(dest);
		// The typed artist doubles as the fallback, so a stray file is filed
		// under it directly rather than under the handler's name and renamed a
		// moment later. Cosmetic — the rename below would fix it either way —
		// but it makes the log read sanely.
		reparent_loose_media(dest, artist_override.empty() ? fallback_artist
		                                                   : artist_override);
		apply_batch_names(dest, artist_override, album_override);

		// One entry per artist directory, so artist names — not the UUID — are
		// what appears as a top-level entry in personal mode. Which batch each
		// one ends up in is the fold's answer, not this batch's: an artist the
		// user already has under some earlier batch is merged into it, so the
		// path to rescan is that one.
		//
		// Serialised because two batches folding at the same moment would each
		// move the other's contents away. /upload detaches this onto an HTTP
		// thread, so two uploads really can arrive together; the fetch worker is
		// single and never races itself.
		std::set<std::string> to_scan;
		{
		std::lock_guard<std::mutex> lock(batch_fold_mu_);
		fold_batch_into_siblings(dest, rel_batch, to_scan);
		}
		if (!to_scan.empty()) store_.scan_dirs(to_scan);
		}
	catch (const std::exception& e) {
		// Same reason as the full scan in listen(): an exception escaping a
		// thread's top-level function calls std::terminate, so a momentary
		// database lock would otherwise be a dead server.
		std::cout << stamp() << "Batch scan aborted: " << e.what() << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "Batch scan aborted: unknown exception"
		          << std::endl;
		}
	}

std::string GainDrive::sanitise_detail(const std::string& line,
                                       const std::filesystem::path& dest,
                                       const std::string& rel_batch) const
	{
	// A tool announces the file it is writing, absolutely — "[download]
	// Destination: /srv/uploads/alice/9c3a…/Artist/Album/Track.opus". Root
	// paths are private to MediaStore and are never surfaced in an API
	// response, so the batch's absolute path becomes its stored form and
	// anything else absolute under the uploads root becomes the root's name.
	std::string s = line;
	auto swap = [&s](const std::string& from, const std::string& to) {
		if (from.empty()) return;
		for (size_t p = s.find(from); p != std::string::npos;
		     p = s.find(from, p + to.size()))
			s.replace(p, from.size(), to);
		};
	swap(dest.string(), rel_batch);
	swap(users_dir_, uploads_root_name_);

	// Belt and braces: a handler names whatever tool an operator chose, and it
	// may print a path from somewhere else entirely. Anything left that still
	// looks absolute is replaced.
	//
	// Matched as a whole token beginning with '/', not merely as a line
	// containing one — "[download] 42% of 5.00MiB at 1.00MiB/s" is the most
	// common line there is, and truncating it at the first slash would throw
	// away the part worth showing.
	std::string out;
	for (size_t i = 0; i < s.size(); ) {
		bool at_start = (i == 0 || std::isspace((unsigned char)s[i - 1]));
		if (at_start && s[i] == '/') {
			while (i < s.size() && !std::isspace((unsigned char)s[i])) i++;
			out += "…";
			}
		else
			out += s[i++];
		}
	return utf8_clean(out, 200);
	}

// One fetch at a time, in submission order.
//
// Serialised deliberately: a fetch is bandwidth- and CPU-heavy (a merge runs
// ffmpeg), and one running job is also what makes the progress reporting
// unambiguous — UrlFetcher tracks a single child, which is what cancel() names.
void GainDrive::fetch_worker()
	{
	namespace fs = std::filesystem;
	try {
		for (;;) {
			std::string id;
			{
			std::unique_lock<std::mutex> lk(fetch_mu_);
			fetch_cv_.wait(lk, [this]{
				return fetch_stop_ || !fetch_queue_.empty(); });
			if (fetch_stop_) return;
			id = fetch_queue_.front();
			fetch_queue_.pop_front();
			}

			// Everything below works on a copy; fetch_jobs_ is the shared
			// record and is only touched under the mutex.
			FetchJob snap;
			{
			std::lock_guard<std::mutex> lk(fetch_mu_);
			auto it = std::find_if(fetch_jobs_.begin(), fetch_jobs_.end(),
			                       [&id](const FetchJob& j){ return j.id == id; });
			// Cancelled before it ever started, which is the common case for a
			// queued job: nothing was created, so there is nothing to remove.
			if (it == fetch_jobs_.end() || it->state != "queued") continue;
			it->state = "running";
			snap      = *it;
			}

			const UrlHandler* h = url_fetcher_.match(snap.url);
			fs::path dest = fs::path(users_dir_) / snap.user / snap.id;

			UrlFetcher::Result r;
			if (!h)
				r.error = "No handler is configured for that URL.";
			else {
				std::error_code ec;
				fs::create_directories(dest, ec);
				r = url_fetcher_.run(*h, snap.audio, snap.url, dest, snap.id,
					[this, &id, &dest, &snap](int pct, const std::string& ln) {
						std::string clean =
							sanitise_detail(ln, dest, snap.batch);
						std::lock_guard<std::mutex> lk(fetch_mu_);
						for (auto& j : fetch_jobs_)
							if (j.id == id) {
								j.percent = pct;
								j.detail  = clean;
								break;
								}
						});
				}

			// A cancel flipped the state while the child was running, and the
			// non-zero exit it caused is not an error worth reporting as one.
			bool cancelled = false;
			{
			std::lock_guard<std::mutex> lk(fetch_mu_);
			for (auto& j : fetch_jobs_)
				if (j.id == id) { cancelled = (j.state == "cancelled"); break; }
			}

			if (cancelled || !r.ok) {
				// Nothing here is worth keeping: a partial download is not
				// playable, and left in place it would be scanned into the
				// library on the next pass over the uploads root.
				std::error_code ec;
				fs::remove_all(dest, ec);
				std::lock_guard<std::mutex> lk(fetch_mu_);
				for (auto& j : fetch_jobs_)
					if (j.id == id) {
						if (!cancelled) {
							j.state = "error";
							// Through sanitise_detail like every progress
							// line: the message names argv.front(), which
							// for a configured handler is an absolute path
							// on this machine.
							j.error = r.error.empty()
							    ? std::string("The fetch failed.")
							    : sanitise_detail(r.error, dest, snap.batch);
							}
						j.finished = static_cast<int64_t>(std::time(nullptr));
						break;
						}
				}
			else {
				{
				std::lock_guard<std::mutex> lk(fetch_mu_);
				for (auto& j : fetch_jobs_)
					if (j.id == id) {
						j.state   = "scanning";
						j.percent = 100;
						j.files   = r.files;
						j.detail  = "Scanning…";
						break;
						}
				}
				// Synchronously, and only then "done" — this is already a
				// background thread, so there is nothing to gain by detaching
				// and everything to gain by "done" meaning the library is
				// actually correct. The client re-renders rather than guessing.
				scan_batch(snap.batch, dest, snap.handler,
				           snap.artist, snap.album);
				std::lock_guard<std::mutex> lk(fetch_mu_);
				for (auto& j : fetch_jobs_)
					if (j.id == id) {
						j.state    = "done";
						j.detail.clear();
						j.finished = static_cast<int64_t>(std::time(nullptr));
						break;
						}
				}

			// Retention, applied per user for the reason given at the constant.
			{
			int64_t cutoff = static_cast<int64_t>(std::time(nullptr))
			               - FETCH_KEEP_S;
			std::lock_guard<std::mutex> lk(fetch_mu_);
			std::map<std::string, size_t> kept;
			for (auto it = fetch_jobs_.rbegin(); it != fetch_jobs_.rend(); ) {
				bool live = it->finished == 0;
				if (!live && (++kept[it->user] > FETCH_KEEP_PER_USER
				              || it->finished < cutoff)) {
					// Erasing through a reverse iterator: base() is one past
					// the element, so step it back to name the element itself.
					it = std::deque<FetchJob>::reverse_iterator(
						fetch_jobs_.erase(std::next(it).base()));
					}
				else
					++it;
				}
			}
			}
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "url fetch worker died: " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp() << "url fetch worker died: unknown exception"
		          << std::endl;
		}
	}

// The artist folders with nothing resolved yet, oldest question first. Called
// once at start and again whenever the queue drains, which is how an artist a
// scan has just added is picked up without the scanner needing to know this
// exists.
void GainDrive::portrait_seed()
	{
	// A 'none' — the providers had nothing — is worth re-asking about after a
	// month; an 'error' says the network failed and is retried at once, which
	// artists_needing_art() handles by not excluding it at all.
	const int64_t month_ago = static_cast<int64_t>(std::time(nullptr))
	    - 30LL * 24 * 3600;
	auto jobs = store_.artists_needing_art(month_ago);

	std::lock_guard<std::mutex> lk(portrait_mu_);
	for (auto& j : jobs) {
		if (portrait_queued_.count(j.path)) continue;
		portrait_queued_.insert(j.path);
		portrait_queue_.push_back(PortraitJob{j.folder_id, j.path, j.name});
		}
	if (!portrait_queue_.empty())
		std::cout << stamp() << "Artist portraits: " << portrait_queue_.size()
		          << " to resolve" << std::endl;
	}

// The start-up seed runs at the same moment the scan thread is detached, so on
// a first scan of an empty library it asks a database with no artists in it,
// finds nothing, and sleeps.  Nothing in the scanner knew this thread existed,
// so the first portrait was fetched up to fifteen minutes after the artists it
// wanted had appeared — on a cold start, always.
//
// This is the notification the scanner owes it.  Deliberately only "look
// again", not a queue: what needs looking up is a database question that
// portrait_seed() already answers, and duplicating that here would be a second
// definition of which artists want art.
void GainDrive::portrait_wake()
	{
	{
	std::lock_guard<std::mutex> lock(portrait_mu_);
	portrait_reseed_ = true;
	}
	portrait_cv_.notify_one();
	}

void GainDrive::portrait_request_front(int folder_id, const std::string& path,
                                        const std::string& name)
	{
	{
	std::lock_guard<std::mutex> lk(portrait_mu_);
	// Already queued: move it to the front rather than adding it twice. What
	// a client is looking at right now should not wait behind the alphabet.
	for (auto it = portrait_queue_.begin(); it != portrait_queue_.end(); ++it) {
		if (it->path == path) {
			PortraitJob j = *it;
			portrait_queue_.erase(it);
			portrait_queue_.push_front(std::move(j));
			portrait_cv_.notify_one();
			return;
			}
		}
	if (portrait_queued_.count(path)) return;   // in flight; it will finish
	portrait_queued_.insert(path);
	portrait_queue_.push_front(PortraitJob{folder_id, path, name});
	}
	portrait_cv_.notify_one();
	}

// See the header. portrait_queued_ covers both halves of "pending": a job is
// inserted when it is queued and erased only after store_artist_art() has run,
// so the in-flight window is inside it too.
bool GainDrive::portrait_pending(const std::string& path)
	{
	std::lock_guard<std::mutex> lk(portrait_mu_);
	return portrait_queued_.count(path) > 0;
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

void GainDrive::portrait_worker()
	{
	// The whole body is guarded. This thread parses four providers' JSON and
	// touches the database, and an exception escaping it would take the server
	// with it rather than one portrait.
	try {
		portrait_seed();

		while (!portrait_stop_) {
			PortraitJob job;
			{
			std::unique_lock<std::mutex> lk(portrait_mu_);
			if (portrait_queue_.empty()) {
				// Nothing to do: sleep, then look again. That is how an artist
				// added by a scan since the last pass is found, and the timer
				// is the fallback for a scan nothing told us about — the folder
				// watcher's, an upload's, a URL fetch's.
				//
				// portrait_reseed_ is in the predicate because the queue is
				// still empty when a scan finishes: waking on
				// !portrait_queue_.empty() alone would re-evaluate to false and
				// go straight back to sleep for the rest of the fifteen
				// minutes, which is precisely the wait being removed.
				portrait_cv_.wait_for(lk, std::chrono::minutes(15),
					[this] { return portrait_stop_ || portrait_reseed_
					              || !portrait_queue_.empty(); });
				if (portrait_stop_) break;
				if (portrait_queue_.empty()) {
					// Cleared here, where it is acted on, and not on every
					// wake: a scan finishing while this thread was waking for
					// a queued request would otherwise clear the flag without
					// ever re-seeding, and the artists that scan added would
					// wait for the timer after all.
					portrait_reseed_ = false;
					lk.unlock();
					portrait_seed();
					continue;
					}
				}
			job = portrait_queue_.front();
			portrait_queue_.pop_front();
			}

			if (portrait_stop_) break;

			MediaStore::ArtistArtRow row;
			// A categories section is called "Film" or "Series"; asking
			// MusicBrainz about that is exactly the mistake is_category_folder
			// exists to prevent. The seed query already excludes them, but a
			// demand request comes straight from an id in a URL.
			if (store_.is_category_folder(job.folder_id)) {
				row.status = "none";
				}
			else {
				// Always the providers and never artist_info_cache — see the
				// note on resolve_artist_info(). This call is also what fills
				// that cache, so it resolves the biography getArtistInfo2
				// serves as much as it resolves the picture.
				bool provider_error = false;
				auto info = resolve_artist_info(job.folder_id, job.name, store_,
				                                &provider_error);
				if (provider_error && info.image_url.empty()) {
					// A provider did not answer — a 503 from MusicBrainz is the
					// usual one, since it rate-limits hard. We have learnt
					// nothing about this artist, so record that rather than a
					// verdict: 'error' is retried, 'none' is not touched for a
					// month. Getting this wrong made one rate-limited moment
					// look exactly like "nobody has a picture of them".
					row.status = "error";
					}
				else if (info.image_url.empty()) {
					// Every provider was asked and none had one. Recorded, or
					// every pass would ask again.
					row.status = "none";
					}
				else {
					row = portrait_fetch(info.image_url);
					}
				}

			store_.store_artist_art(job.path, job.name, row);
			cover_cache_.invalidate(job.path);
			{
			std::lock_guard<std::mutex> lk(portrait_mu_);
			portrait_queued_.erase(job.path);
			}

			if (row.status == "ok")
				std::cout << stamp() << "Artist portrait: " << job.name << " "
				          << row.width << "x" << row.height << ", "
				          << row.bytes.size() << " bytes" << std::endl;
			else
				// Which of the two it is matters — "error" will be asked
				// again, "none" will not for a month — so say which, rather
				// than leaving the difference to be inferred from behaviour a
				// month later.
				std::cout << stamp() << "Artist portrait: " << job.name
				          << ": " << row.status
				          << (row.status == "error"
				                  ? " (a provider did not answer; will retry)"
				                  : " (no provider has one)") << std::endl;

			// The gap between artists, and it is never skipped.
			//
			// MusicBrainz allows one request a second per address and answers
			// 503 when that is exceeded. The chain makes two MusicBrainz
			// requests per artist with a one-second sleep between them, so
			// this wait is what keeps the sustained rate under the limit —
			// and being rate-limited is not a harmless slowdown here, because
			// a 503 is indistinguishable from "this artist has no picture"
			// unless the code is careful, and one full-speed pass over a
			// library would earn a great many of them.
			//
			// An earlier version skipped the wait whenever the queue was not
			// empty, meaning to let a user who was waiting jump ahead. But a
			// seeded backlog leaves the queue permanently non-empty, so the
			// pacing never applied at all during precisely the pass that
			// needed it. A demand request already gets what it needs by going
			// to the front of the queue; it does not also need to outrun the
			// rate limit.
			{
			std::unique_lock<std::mutex> lk(portrait_mu_);
			portrait_cv_.wait_for(lk, PORTRAIT_GAP,
				[this] { return portrait_stop_.load(); });
			}
			}
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "Artist portrait worker stopped: " << e.what()
		          << std::endl;
		}
	catch (...) {
		std::cout << stamp()
		          << "Artist portrait worker stopped: unknown exception"
		          << std::endl;
		}
	}

void GainDrive::cast_teardown()
	{
	cast_manager_.stop();
	last_cast_song_id_.clear();
	last_cast_offset_ = 0.0f;
	set_cast_stream({});
		{
		// Releasing the in-use count is what lets the transcode cache prune a
		// soundtrack nobody is listening to any more.
		std::lock_guard<std::mutex> lk(cast_warm_mu_);
		cast_warm_entry_.reset();
		}
		{
		std::lock_guard<std::mutex> lk(cast_owner_mu_);
		cast_owner_user_.clear();
		cast_owner_controller_.clear();
		}
	// After the owner is cleared, not before: the bump is the signal a
	// displaced castEvents connection watches for, and it must not see a
	// session that is half torn down.
	++cast_session_gen_;
	}

bool GainDrive::cast_owned_by(const httplib::Request& req)
	{
	std::string controller = req.get_param_value("castController");
	if (controller.empty()) return false;
	std::lock_guard<std::mutex> lk(cast_owner_mu_);
	return !cast_owner_controller_.empty()
	    && cast_owner_controller_ == controller
	    && cast_owner_user_       == req.get_param_value("u");
	}

void GainDrive::cast_claim(const std::string& user,
                           const std::string& controller)
	{
		{
		std::lock_guard<std::mutex> lk(cast_owner_mu_);
		cast_owner_user_       = user;
		cast_owner_controller_ = controller;
		}
	++cast_session_gen_;
	}

bool GainDrive::cast_video_refused(const std::string& device_id,
                                   const std::string& codec_pair)
	{
	if (device_id.empty() || codec_pair.empty()) return false;
	std::string list = store_.get_setting(CAST_NOVIDEO_KEY + device_id);
	// Wrapped in commas both sides so "h264/aac" cannot match inside
	// "h264/aac_latm".
	return ("," + list + ",").find("," + codec_pair + ",") != std::string::npos;
	}

void GainDrive::cast_note_video_refused(const std::string& device_id,
                                        const std::string& codec_pair)
	{
	if (device_id.empty() || codec_pair.empty()) return;
	if (cast_video_refused(device_id, codec_pair)) return;
	std::string list = store_.get_setting(CAST_NOVIDEO_KEY + device_id);
	if (!list.empty()) list += ",";
	list += codec_pair;
	store_.set_setting(CAST_NOVIDEO_KEY + device_id, list);
	std::cout << stamp() << "Cast: device " << device_id << " refused "
	          << codec_pair << "; sending the soundtrack for it from now on"
	          << std::endl;
	}

CastManager::LoadRequest
GainDrive::soundtrack_load(const std::string& base,
                           const MediaStore::SongInfo& song, int song_id,
                           float offset, bool is_fallback,
                           CastStreamInfo& desc)
	{
	CastManager::LoadRequest lr;
	desc = CastStreamInfo{};

	// The same answer cast_load_song() reached when it decided this was worth
	// doing at all — one definition, in codecs.hh, for the reason stated there.
	const std::string fmt = cast_soundtrack_format(song.audio_codec);
	if (fmt.empty()) return lr;

	// No captions collected, which is not an omission: there is no picture to
	// put them on, and an empty list is what leaves the minted token unable to
	// fetch one at all.
	const std::string token = cast_manager_.mint_token(song_id, {});
	if (token.empty()) return lr;

	const std::string sid_s = std::to_string(song_id);
	// Naming an audio format for a video *is* the request for its soundtrack —
	// audio_only_request() in codecs.hh — so this one parameter is the whole of
	// it on the server side.
	lr.url  = base + "stream.view?id=" + sid_s + "&castToken=" + token
	        + "&format=" + fmt;
	lr.mime = std::string(codec_to_mime(fmt));
	// Native seek: the URL serves the whole file and the LOAD says where to
	// begin, so the receiver's clock is absolute.
	lr.current_time = offset;
	lr.duration     = song.duration;

	desc.audio_only     = true;
	// No picture reaches the receiver on this route, by construction.  A client
	// keeps its own copy of the picture on this, never on audio_only.
	desc.receiver_video = false;
	desc.mime           = lr.mime;
	desc.suffix         = fmt;

	Streamer::SongInfo si = streamer_song(song, store_.abs_path(song.path));
	// Resolved here rather than inside the lambda so the figures reported to
	// the client are the ones the warm actually produces.  It is pure
	// negotiation against the source — no I/O — so hoisting it costs nothing
	// and two copies of it could disagree.
	auto plan = Streamer::plan_transcode(si, fmt, 0, 0);
	// A copy plan is exactly the one whose bitrate is 0.
	const bool copying = plan.bitrate == 0;
	// The same two words the video ladder uses, and they mean the same thing
	// here: "remux" is -c:a copy of the track the file already holds, "encode"
	// is a decode and a lossless re-encode.  It is the answer to "why does this
	// sound worse on the television", which is the whole reason these fields
	// are reported rather than re-derived by the client.
	desc.tier = copying ? "remux" : "encode";
	// 0 for anything that is not a fixed-rate encode, which is two cases: a
	// copy, and a *lossless* encode, where plan.bitrate is only the non-zero
	// marker meaning "encode" and describes nothing about the bytes.
	desc.bitrate = (plan.target && plan.target->lossy) ? plan.bitrate : 0;

	// Captured now: by the time prepare runs, the session's device is still the
	// same one, but reading it on that thread would be a second answer to a
	// question already settled here.
	const std::string dev  = cast_manager_.get_device_id();
	const std::string pair = song.video_codec + "/" + song.audio_codec;

	// The soundtrack of a film is a transcode of a two-hour AC3 track, and
	// serve() answers it out of the transcode cache — which materialises the
	// whole file before the first byte.  A receiver drops a session after about
	// a minute with no data on the HTTP body, so that wait has to happen before
	// it is told anything at all.  This runs on CastManager's load worker,
	// which already treats a newer load_gen_ as a cancellation.
	//
	// The entry is kept alive afterwards: it is an RAII in-use count and
	// prune() skips in-use keys, so releasing it here would let the file be
	// evicted between the warm and the receiver's first GET.
	lr.prepare = [this, si, sid_s, plan, copying, is_fallback, desc, dev,
	              pair]() {
		// Before the warm, not after: a client asking castSession during the
		// minute ffmpeg takes should already be told what it is waiting for.
		// This hook running at all is the proof that the first attempt was
		// refused — CastManager only reaches a fallback down that path — which
		// is why both of these live here and nowhere else.
		if (is_fallback) {
			cast_note_video_refused(dev, pair);
			set_cast_stream(desc);
			}
		auto entry = Streamer::cache_entry(si, transcode_cache_, plan);
		if (!entry) {
			// Logged because a cast that dies about a minute in is this line.
			// Whether it is fatal depends on which plan failed, and the
			// asymmetry is the point: an *encode* that could not be cached may
			// still play as a pipe, so the LOAD goes out.  A *copy* that could
			// not be cached means the argv itself failed, and serve() would run
			// the identical argv down the piped path and fail identically — so
			// pointing the receiver at that URL buys a dead session with
			// nothing to explain it.
			std::cout << stamp()
			          << "Cast: no cache entry for soundtrack of song="
			          << sid_s
			          << (copying ? ", copy failed; not loading"
			                      : ", streaming unwarmed") << std::endl;
			return !copying;
			}
		std::cout << stamp() << "Cast: soundtrack warmed song=" << sid_s
		          << " " << (entry->hit() ? "hit" : "built")
		          << " bytes=" << entry->size() << std::endl;
		std::lock_guard<std::mutex> lk(cast_warm_mu_);
		cast_warm_entry_ = std::move(entry);
		return true;
		};
	return lr;
	}

GainDrive::CastStreamInfo
GainDrive::cast_load_song(const httplib::Request& req,
                          const MediaStore::SongInfo& song,
                          int song_id, float offset, int track_id)
	{
	// Everything the receiver fetches hangs off this, and every one of them
	// carries the cast token rather than the account's credentials: a
	// television is not a place to leave a password, and the token is already
	// what stream.view accepts.
	//
	// The configured public_url wins over the request's own Host header, and
	// on a public deployment it must: Host and X-Forwarded-Proto are client
	// input, so believing them let any cast-role account point the receiver
	// at an origin of its choosing — which hands that origin the cast token
	// in the query string, and the token is accepted by stream.view with no
	// account attached. On a LAN with no public_url configured the header is
	// the only source there is, and the peers who could abuse it are the
	// household.
	std::string base;
	if (!public_url_.empty())
		base = public_url_ + "/rest/";
	else {
		std::string host  = req.get_header_value("Host");
		if (host.empty()) host = "localhost";
		std::string proto = req.get_header_value("X-Forwarded-Proto");
		if (proto.empty()) proto = "http";
		base = proto + "://" + host + "/rest/";
		}
	const std::string sid_s = std::to_string(song_id);

	// A receiver that cannot display a picture is sent the film's soundtrack
	// rather than a video container it will drop the picture out of.  The
	// decision is made here, in the one place that has both the song and the
	// device, and reported to the client rather than re-derived there — the
	// same rule the three callers of the tier predicate follow.
	//
	// `video_out()` reports true for a device that announced no capabilities,
	// which is every configured one: refusing the picture on a guess is worse
	// than the guess.
	//
	// The third term is not belt and braces.  audio_only_request() is what
	// serve() will actually apply, so this has to reach the same answer or the
	// LOAD announces audio/mpeg while the video ladder serves MP4, and a
	// receiver refuses media whose type does not match what arrives.  A
	// *silent* video is the case that separates the two: it has nothing to
	// extract, so audio_only_request() keeps it on the video ladder.
	//
	// What "cannot display a picture" means is not quite the `ca` record on its
	// own any more.  A per-device preference, held server-side under the cast
	// device's id so every client agrees about a device, can override it in
	// either direction, and the two directions are deliberately not
	// symmetrical:
	//
	//  * `send` only ever *raises* a device that announced no screen, and only
	//    for a file the Direct tier sends untouched.  A WiiM plays a video
	//    file's sound perfectly well whatever the Cast documentation says, and
	//    the raw file byte-ranged off disk costs nothing at all — no ffmpeg,
	//    no wait before the LOAD.  It stops at the Direct tier because the
	//    other two are worse than extracting the sound: a remux reads and
	//    writes the whole film through the cache, and a re-encode re-encodes a
	//    picture nobody can see.  Never applied to a device that *does*
	//    announce a screen, or it would demote a remuxable film to audio.
	//  * `sound` closes the other half of the gap.  A configured device has
	//    capabilities == -1 and so reads as capable, which means a screenless
	//    device named in the config could not be told it has no screen.
	const std::string video_pref =
		store_.get_setting("cast_video:" + cast_manager_.get_device_id());
	bool shows_video = cast_manager_.device_video_out();
	const std::string codec_pair = song.video_codec + "/" + song.audio_codec;
	if (video_pref == "sound")
		shows_video = false;
	else if (video_pref == "send" && !shows_video)
		// The tier says the file can go out untouched; the second test says
		// this device has not already proved it cannot decode it.  The Direct
		// tier is built from *browser* predicates, and a Cast receiver's codec
		// support is narrower — an AV1/Opus MP4 passes it and no amplifier
		// plays it — so the tier alone is a hopeful answer rather than a
		// reliable one.  Being refused once is what makes it reliable.
		shows_video = cast_tier_for(song.codec, song.video_codec,
		                            song.audio_codec) == CastTier::Direct
		           && !cast_video_refused(cast_manager_.get_device_id(),
		                                  codec_pair);

	// Empty when there is nothing to extract — a silent film, or one the
	// scanner could not probe.  Such a video stays on the video ladder, which
	// is what keeps this predicate agreeing with the one Streamer::serve()
	// applies: a LOAD announcing audio/flac while MP4 goes out is media a
	// receiver refuses on the type alone.
	const std::string cast_fmt = cast_soundtrack_format(song.audio_codec);

	const bool audio_only = song.is_video && !shows_video && !cast_fmt.empty();

	// Two shapes, and they share only the description they produce.  The
	// soundtrack is built by soundtrack_load(), which the fallback below builds
	// again — that is why it is a function rather than a branch.
	CastManager::LoadRequest lr;
	CastStreamInfo stream;

	if (audio_only) {
		lr = soundtrack_load(base, song, song_id, offset, false, stream);
		if (lr.url.empty()) {
			std::cout << stamp() << "Cast: no soundtrack load for song=" << sid_s
			          << "; refusing to load" << std::endl;
			// Nothing was loaded, so nothing is being sent: an empty description.
			// The client hides a row it has no value for, which is the right
			// showing for a load that did not happen.
			return {};
			}
		}
	else {
		// The caption list is resolved before the token is minted, because the
		// token is scoped to this song *and* to the caption ids this LOAD is
		// about to declare — getCaptions will accept it for those and nothing
		// else.
		MediaStore::VideoStreams streams;
		std::vector<int> caption_ids;
		if (song.is_video) {
			streams = store_.get_video_streams(song_id);
			for (const auto& c : streams.captions)
				caption_ids.push_back(c.index);
			}

		// Minted per LOAD rather than per session. Everything the receiver
		// fetches carries this rather than the account's credentials — a
		// television is not a place to leave a password — so what it is worth is
		// what a leak costs: one song, for as long as this LOAD is current.
		const std::string token = cast_manager_.mint_token(song_id, caption_ids);
		if (token.empty()) {
			std::cout << stamp() << "Cast: no stream token; refusing to load"
			          << std::endl;
			return {};
			}
		const std::string tok = "&castToken=" + token;

		lr.url  = base + "stream.view?id=" + sid_s + tok;
		lr.mime = std::string(cast_mime_for(song.codec, song.video_codec,
		                                    song.audio_codec));
		// Native seek: the URL serves the whole file and the LOAD says where to
		// begin, so the receiver's clock is absolute.  That is also why the
		// caption cues need no shifting here, unlike the browser's own transcoded
		// seek — see videoShiftCues() in web/app.js for the case where they do.
		lr.current_time = offset;
		lr.duration     = song.duration;

		stream.mime = lr.mime;
		// Whether the receiver will actually *show* the film, which is a
		// different question from whether it was sent one.  Under `send` a
		// screenless device gets the whole video and displays none of it, so
		// !audio_only would tell a client to drop its own picture exactly where
		// it is most wanted — the bug that made this a separate field.
		// device_video_out() is the raw announcement here on purpose: the
		// preference decides what to send, and no preference gives a receiver a
		// screen.
		stream.receiver_video = song.is_video
		                     && cast_manager_.device_video_out();
		CastTier t  = cast_tier_for(song.codec, song.video_codec,
		                            song.audio_codec);
		stream.tier = std::string(cast_tier_name(t));
		// A remux and a re-encode both arrive as MP4; only the direct tier sends
		// the file as it stands, so only there is the file's own bitrate the one
		// going over the wire.  That is the fact a client cannot get from
		// anywhere else: the transcoded* fields describe the account ceiling,
		// which stream.view exempts for a cast token, so they describe a
		// conversion that is not happening.
		//
		// 0 on the other two tiers rather than the source's figure.  A -c copy is
		// close enough to it that quoting it would be nearly right, and nearly
		// right is the worst thing a diagnostic can be; a re-encode is not
		// fixed-rate at all.
		stream.suffix  = t == CastTier::Direct ? song.codec : "mp4";
		stream.bitrate = t == CastTier::Direct ? song.bitrate : 0;

		if (song.is_video) {
			int  n = 0;
			for (const auto& c : streams.captions) {
				++n;
				lr.caption_ids.push_back(c.index);
				lr.tracks.push_back({
					{"trackId",          n},
					{"type",             "TEXT"},
					{"subtype",          "SUBTITLES"},
					{"trackContentId",   base + "getCaptions.view?id=" + sid_s
					                     + "&captionId=" + std::to_string(c.index)
					                     + tok},
					{"trackContentType", "text/vtt"},
					// Required for a subtitle track, and one without it can be
					// dropped by the receiver with no diagnostic anywhere.  A
					// sidecar file has no language to report, so it gets the
					// ISO 639-2 code that means exactly that.
					{"language",         c.language.empty() ? "und" : c.language},
					{"name",             c.title.empty() ? "Subtitles" : c.title}
					});
				}
			if (track_id > 0 && track_id <= n)
				lr.active_track_ids.push_back(track_id);
			}

		// What to do when the receiver refuses this outright.
		//
		// Only for the one case where the answer is unambiguous: the `send`
		// preference has put a picture on a device that announced no screen, so
		// the picture was never going to be seen there and the browser is very
		// likely holding its own copy already.  Dropping to the soundtrack costs
		// nothing anyone was looking at.
		//
		// Deliberately not offered for a device that *did* announce a screen.
		// There the soundtrack is not obviously wanted, no client is holding the
		// picture to fall back to, and the panel would go on saying "Playing on
		// your TV" over sound alone.  That case is a known gap: an AV1 film cast
		// to a receiver that cannot decode it still fails, and the tier ladder
		// has nothing better to offer it — a remux is -c copy, so it would send
		// the identical codecs a second time.
		if (song.is_video && !cast_manager_.device_video_out()) {
			CastStreamInfo fb;
			auto fb_req = soundtrack_load(base, song, song_id, offset, true, fb);
			if (!fb_req.url.empty())
				lr.fallback = std::make_shared<CastManager::LoadRequest>(
				                  std::move(fb_req));
			}
		}

	// One line rather than reading it out of the LOAD dump below, which for a
	// film with several tracks is long enough to scroll past. Says what a
	// "subtitles do not appear", "no picture" or "why is this being
	// re-encoded" report needs first: whether the server thinks this is a
	// video at all, whether it decided to send only the sound, which tier the
	// fetch will land on and what that means the receiver gets, how many
	// tracks it offered, and which one it asked for.  It is also the line to
	// compare a client's info panel against, since the panel is drawn from
	// exactly these values rather than from a second derivation.
	std::cout << stamp() << "Cast: load song=" << sid_s
	          << " video=" << (song.is_video ? "yes" : "no")
	          << " audio_only=" << (audio_only ? "yes" : "no")
	          << (video_pref.empty() ? "" : " pref=" + video_pref)
	          << " screen=" << (stream.receiver_video ? "yes" : "no")
	          << (lr.fallback ? " fallback=soundtrack" : "")
	          << " mime=" << lr.mime
	          << " tier=" << stream.tier
	          << " sent=" << stream.suffix
	          << (stream.bitrate > 0 ? "@" + std::to_string(stream.bitrate) : "")
	          << " tracks=" << lr.tracks.size()
	          << " active=" << (lr.active_track_ids.empty()
	                            ? 0 : lr.active_track_ids.front())
	          << std::endl;

	last_cast_song_id_ = sid_s;
	last_cast_offset_  = 0.0f;
	set_cast_stream(stream);
	cast_manager_.load(lr);
	return stream;
	}

// A configured cast device is never confirmed by anything: mDNS does not
// announce it, and CastManager::start() neither connects nor fails, so a typo
// in --cast-device or the config file stays completely silent until someone
// tries to cast and gets a session that never begins. One probe each at startup
// turns that into a line in the log.
//
// Detached, because a device that is merely switched off costs the connect
// timeout and nothing should wait for that.
void GainDrive::probe_cast_devices_background()
	{
	auto devices = cast_manager_.cached_devices();
	std::vector<CastManager::CastDevice> manual;
	for (auto& d : devices)
		if (d.manual) manual.push_back(d);
	if (manual.empty()) return;

	std::thread([this, manual]{
		try {
			for (const auto& d : manual) {
				auto result = cast_manager_.probe(d);
				std::cout << stamp() << "Cast: configured device '" << d.name
				          << "' (" << d.address << ":" << d.port << ") "
				          << CastManager::probe_text(result) << std::endl;
				}
			}
		// An exception escaping a detached thread is std::terminate, and a
		// probe is not worth a dead server.
		catch (const std::exception& e) {
			std::cout << stamp() << "Cast device probe failed: " << e.what()
			          << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Cast device probe failed: unknown exception"
			          << std::endl;
			}
		}).detach();
	}

// A statically linked binary resolves names through whatever its libc provides:
// musl does it itself and works, a static glibc cannot do it at all. Either way
// the failure is invisible — the library serves fine and only the MusicBrainz,
// Wikidata, Discogs and cover-art-by-URL fetches quietly come back empty. One
// lookup at startup turns that into a line in the log.
static void check_dns_background()
	{
	std::thread([]{
		try {
			addrinfo hints{};
			hints.ai_family   = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			int rc = getaddrinfo("www.gaindrive.org", nullptr, &hints, &res);
			if (rc == 0) {
				freeaddrinfo(res);
				return;
				}
			std::cout << stamp() << "Warning: cannot resolve www.gaindrive.org ("
			          << gai_strerror(rc) << "). Artist info, lyrics and cover "
			             "art fetched from the web will not work. Harmless if "
			             "this server is deliberately offline." << std::endl;
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "DNS check failed: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "DNS check failed: unknown exception"
			          << std::endl;
			}
		}).detach();
	}

bool GainDrive::listen(const std::string& host, int port)
	{
	// Bind first and announce afterwards.  The message used to print before
	// the attempt, so a start that never acquired the port still looked like
	// a healthy one in the log.
	if (!server_.bind_to_port(host, port)) {
		std::cerr << stamp() << "Error: cannot bind to " << host << ":" << port
		          << " - is another gaindrive already running?" << std::endl;
		return false;
		}
	std::cout << stamp() << "Listening on " << host << ":" << port << std::endl;

	// Only now that the port is ours: a server that cannot serve should do no
	// work at all, and should be able to exit at once.
	// An exception escaping a thread's top-level function calls
	// std::terminate, so an unguarded scan turns a momentary database lock
	// into a dead server.  A stale library until the next scan is a far better
	// outcome, and the folder watcher will retry on the next filesystem event.
	if (!no_scan_)
		std::thread([this]{
			try { store_.scan(); }
			catch (const std::exception& e) {
				std::cout << stamp() << "Scan aborted: " << e.what()
				          << std::endl;
				}
			catch (...) {
				std::cout << stamp() << "Scan aborted: unknown exception"
				          << std::endl;
				}
			// Outside the try, and on every path: an aborted scan still added
			// whatever it got through, and those artists want portraits as
			// much as any others.  Safe before the worker below has started —
			// it sets a flag the worker's first wait tests, so an early wake
			// costs one extra seed rather than being lost.
			portrait_wake();
			}).detach();
	cast_manager_.discover_background();
	probe_cast_devices_background();
	check_dns_background();
	watcher_.start();
	// Joined in the destructor rather than detached: it holds references to
	// store_ and cover_cache_, so it must not outlive them.
	portrait_thread_ = std::thread([this]{ portrait_worker(); });
	// Same reasoning, and more sharply: this one calls scan_dirs().
	if (url_fetcher_.configured())
		fetch_thread_ = std::thread([this]{ fetch_worker(); });

	if (!server_.listen_after_bind()) {
		std::cerr << stamp() << "Error: server loop exited unexpectedly."
		          << std::endl;
		return false;
		}
	return true;
	}

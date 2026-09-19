#include "authz.hh"
#include "subsonic.hh"
#include "netaddr.hh"
#include "textutil.hh"
#include "stamp.hh"

#include <chrono>
#include <cstring>
#include <iostream>
#include <map>
#include <mutex>

#include <arpa/inet.h>

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

// The bucket an address's failures count against. IPv6 keys on the /64
// prefix rather than the full address: a routed allocation hands a caller
// that entire prefix, so exact keys would give a rotating attacker its free
// tries back on every guess, plus an endless supply of fresh keys with which
// to fill the capped map and sweep everyone else's counters. IPv4 stays
// exact, since one NAT'd household sharing a bucket is the throttle working
// as sized. Only the key is truncated; log lines keep the address as it
// arrived.
std::string throttle_bucket(const std::string& addr)
	{
	// No colon is IPv4. A dot alongside colons is a v4-mapped IPv6 address,
	// which names an IPv4 client and is keyed like one.
	if (addr.find(':') == std::string::npos
	    || addr.find('.') != std::string::npos) return addr;
	struct in6_addr v6;
	const std::string bare = addr.substr(0, addr.find('%'));
	if (inet_pton(AF_INET6, bare.c_str(), &v6) != 1) return addr;
	std::memset(v6.s6_addr + 8, 0, 8);
	char buf[INET6_ADDRSTRLEN];
	if (!inet_ntop(AF_INET6, &v6, buf, sizeof buf)) return addr;
	return std::string(buf) + "/64";
	}

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

std::size_t throttle_entries()
	{
	std::lock_guard<std::mutex> lock(throttle_mu);
	return throttle.size();
	}

// Extracts u/p/t/s params and validates auth. Writes error into res on failure.
bool check_auth(const httplib::Request& req, httplib::Response& res,
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
	const std::string addr         = client_addr(req);
	const std::string throttle_key = throttle_bucket(addr);
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
		std::cout << stamp(addr) << "auth failed for user "
		          << log_safe(u, 64) << std::endl;
		err(40, "Wrong username or password.");
		return false;
		}

	throttle_record_success(throttle_key);
	return true;
	}
// Returns true if the authenticated user has cast permission.
bool check_cast_perm(const httplib::Request& req, httplib::Response& res,
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

bool check_upload_perm(const httplib::Request& req,
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
bool item_write_allowed(const httplib::Request& req, MediaStore& store,
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

bool check_item_write_perm(const httplib::Request& req,
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
// here rather than in each query keeps the twelve ChildEntry queries untouched,
// which is the same trade getVideos made for is_video.
bool item_read_allowed(const httplib::Request& req, MediaStore& store,
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

bool check_item_read_perm(const httplib::Request& req,
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


bool personal_scope(const httplib::Request& req, httplib::Response& res,
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

#include "gaindrive.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "textutil.hh"
#include "untrusted.hh"
#include "batchtools.hh"
#include "urlfetch.hh"

#include <algorithm>
#include <filesystem>
#include <set>
#include <vector>

#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

// How many fetches may be waiting at once. One worker runs the queue, so this
// is a bound on how far behind a user can get the server, not on throughput.
static constexpr size_t FETCH_QUEUE_MAX = 20;

// How many finished jobs are kept per user, and for how long. Per user rather
// than server-wide: a shared cap lets one busy account evict another's results
// before that person's browser has polled for them.
static constexpr size_t FETCH_KEEP_PER_USER = 10;
static constexpr int64_t FETCH_KEEP_S       = 15 * 60;


void GainDrive::routes_fetch()
	{
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
		// Nothing moves a directory under this batch after the fold, so the
		// hold has done its job. Released *before* the scan rather than by the
		// producer afterwards, because scan_dirs() skips a held batch and this
		// is the scan that batch exists for.
		{
		std::error_code ec;
		std::filesystem::remove(batch_marker(dest), ec);
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

			// Declared out here so it outlives the fetch and covers scan_batch()
			// below, but only taken once there is a batch to hold: an unmatched
			// URL never creates the directory.
			std::optional<BatchHold> hold;

			UrlFetcher::Result r;
			if (!h)
				r.error = "No handler is configured for that URL.";
			else {
				std::error_code ec;
				hold.emplace(dest);
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

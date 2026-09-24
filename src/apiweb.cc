#include "gaindrive.hh"
#include "countingpool.hh"
#include "subsonic.hh"
#include "authz.hh"
#include "stamp.hh"
#include "textutil.hh"
#include "netaddr.hh"
#include "embedded_web.hh"

#include <functional>
#include <string_view>
#include <netinet/tcp.h>
#include <sys/socket.h>

#include <iostream>

#include <tinyxml2.h>
#include <nlohmann/json.hpp>

using namespace tinyxml2;

void GainDrive::routes_web()
	{
	// Socket options, including the deliberate SO_REUSEADDR-not-SO_REUSEPORT
	// choice that stops a second gaindrive silently sharing this port, are set
	// further down alongside the TCP keepalive settings - set_socket_options()
	// takes a single callback, so they have to live together.

	// A cache miss now blocks its request thread for the whole transcode
	// (seconds, not milliseconds), so the default pool of 8 is too small to
	// absorb a client that pins an album and fans out downloads.  The ffmpeg
	// count is bounded separately by --transcode-jobs; this only bounds waiting.
	// The queue behind the pool is bounded (third argument): unbounded, a
	// slowloris that pinned all 32 workers would go on accepting connections
	// into an ever-growing deque, each holding a file descriptor, until
	// EMFILE. Refusing the 129th queued request instead sheds load at the
	// edge, which is the recoverable failure. max_n stays 0 - a fixed pool.
	server_.new_task_queue = [this]{
		auto* q = new CountingPool(32, 0, 128);
		http_pool_.store(q);
		return q;
		};

	// httplib's payload cap defaults to SIZE_MAX. Since 0.54 the body of a
	// routed request is read only after the route has matched and the
	// pre-request handler has approved it, so the cap no longer has to be
	// small to protect unauthenticated callers - the pre-request handler
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
	// content reader to disk under its own cap - it never buffers it - and
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


	// Normalise /rest/foo → /rest/foo.view so clients that omit the suffix still work.
	// In debug mode also strip Accept-Encoding: httplib swaps compressed bytes into
	// res.body before firing the logger, making it unreadable (cpp-httplib#1656).
	//
	// Also the one place CORS is answered - and it is answered only for the
	// endpoints a Cast receiver fetches for itself. **A Chromecast needs this
	// to show a subtitle.** The receiver fetches a side-loaded WebVTT track by
	// XHR, and declaring any track at all puts its media element into
	// anonymous cross-origin mode - so the *film* needs the header as much as
	// the captions do, on its 206 responses as much as its 200s.
	//
	// It used to be `*` on every endpoint, on the reasoning that credentials
	// ride in query parameters so there is no ambient authority for a hostile
	// page to borrow. That was true and still incomplete: the Subsonic
	// envelope is HTTP 200 for success and failure alike, so only a
	// CORS-readable body distinguishes a right password from a wrong one -
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
	// response - so the fallback quietly undid that rule for every unexpected
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
	// diagnosing a client - and is why it has to redact. Credentials ride in
	// the query string on every single request, so without this the journal is
	// a second complete plaintext credential store, kept for longer than the
	// database and usually readable by more people. `p` is the password
	// itself, `password` is a *new* one on its way through changePassword,
	// createUser or updateUser, `t`/`s` are the token pair, which is
	// replayable for as long as the password behind it lives, and `castToken`
	// is a bearer credential in its own right.
	//
	// Values are also stripped of CR and LF: they are attacker-supplied and
	// were written raw, so a parameter could forge whole log lines - which
	// matters more once something is reading this log to decide who to block.
	server_.set_logger([this](const httplib::Request& req, const httplib::Response& res) {
		static const std::set<std::string> secret_params = {
			"p", "password", "t", "s", "castToken",
			// Server-side API keys arrive as ordinary GET parameters on
			// saveServerSettings, and getServerSettings goes to some lengths
			// never to read them back - a log line that printed them would
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
		// sets SO_REUSEPORT on Linux - see the note above.
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

	// Web client - serve embedded static files.
	//
	// A track link opened on an Android device is answered with a chooser
	// page - an intent:// anchor aimed at the app, and a browser link -
	// instead of the SPA.  web=1 is the loop-breaker: the chooser's own
	// browser link and the intent's fallback URL both carry it, or an
	// Android browser would be handed the chooser again for ever.  The page
	// composes its anchors from location.href itself, so nothing from the
	// query string is spliced into HTML here, and the public scheme and
	// host - which behind a proxy this process does not know - come free.
	//
	// Every embedded asset is revalidated rather than cached blindly, which is
	// the same bargain getCoverArt strikes and for a sharper reason.
	//
	// These files had no cache headers at all - no Cache-Control, no ETag, no
	// Last-Modified - which does not mean "do not cache", it means the browser
	// picks a lifetime by heuristic and nothing can correct it. **The SPA talks
	// to the API it shipped with**, so a heuristically cached app.js run against
	// an upgraded server is a client out of step with its server, with nothing
	// anywhere saying so: exactly the confusion Errors.kt's "the server may be
	// too old" message exists to name, arriving from the other direction. The
	// person who upgraded the binary has no reason to suspect their browser.
	//
	// `no-cache` is not `no-store`: the copy is kept and revalidated, so the
	// steady-state cost is a conditional request answered 304, not a download.
	//
	// The validator is the version *and* a hash of the bytes. The version alone
	// would be wrong in the case it matters most - every build between releases
	// carries the same VERSION, so changed bytes would keep their old validator
	// during development. std::hash is enough for a cache validator: this is not
	// a signature, and a collision costs one stale load, which is what the
	// version half is there to bound anyway.
	auto etag_of = [](std::string_view body) {
		// Computed per call rather than memoised: it is one pass over a few
		// hundred kilobytes on a request that is already writing that much to a
		// socket, and a static would have to be keyed by asset to be correct.
		return "\"gd-" + std::string(GAINDRIVE_VERSION) + "-"
		     + std::to_string(std::hash<std::string_view>{}(body)) + "\"";
		};
	// True when the response has been completed as a 304, so the caller returns
	// without writing a body. Headers that describe the *resource* rather than
	// the payload - the CSP, X-Frame-Options - are already set by then, which is
	// what a conformant 304 wants.
	auto revalidated = [etag_of](const httplib::Request& req, httplib::Response& res,
	                             std::string_view body) {
		std::string etag = etag_of(body);
		res.set_header("Cache-Control", "no-cache");
		res.set_header("ETag", etag);
		if (req.get_header_value("If-None-Match") == etag) {
			res.status = 304;
			return true;
			}
		return false;
		};
	auto static_asset = [revalidated](std::string_view body, std::string_view mime) {
		return [revalidated, body, mime](const httplib::Request& req,
		                                 httplib::Response& res) {
			if (revalidated(req, res, body)) return;
			res.set_content(body.data(), body.size(), mime.data());
			};
		};

	// The two HTML pages carry a Content-Security-Policy; assets and API
	// responses need none, since a policy governs only the document that a
	// browser renders. frame-ancestors 'none' is the load-bearing directive:
	// without it the Settings pane - delete user, move album - can be framed
	// and overlaid by any origin, and one click on the overlay is a click on
	// this UI. The SPA keeps its one inline script in theme.js precisely so
	// script-src can be 'self' with no carve-out; img-src is the single
	// exception to "nothing external", and it is one the cover-art dialog
	// needs: it previews an image URL a person has typed, so the host is not
	// knowable in advance and no list of them would do. An image is all the
	// carve-out buys - script-src, connect-src and the rest stay 'self', so
	// nothing reached this way can execute or be read back - and data: is
	// there for the same dialog's preview of a file picked off the device.
	// http: matters only when gaindrive itself is served over plain http,
	// where setCoverArt would accept such a URL and the preview should not
	// disagree with it; over https the browser blocks it as mixed content
	// whatever the policy says. link.html is deliberately self-contained (it
	// must survive with no other asset loading), so its policy allows its own
	// inline script and style and nothing else. X-Frame-Options is the same
	// rule for browsers that predate frame-ancestors.
	auto index_page = [revalidated](const httplib::Request& req,
	                                httplib::Response& res) {
		res.set_header("X-Frame-Options", "DENY");
		if (req.has_param("track") && !req.has_param("web")
		    && req.get_header_value("User-Agent").find("Android")
		       != std::string::npos) {
			res.set_header("Content-Security-Policy",
			               "default-src 'none'; script-src 'unsafe-inline'; "
			               "style-src 'unsafe-inline'; base-uri 'none'; "
			               "form-action 'none'; frame-ancestors 'none'");
			if (revalidated(req, res, embedded::link_html)) return;
			res.set_content(embedded::link_html.data(), embedded::link_html.size(),
			                embedded::link_html_mime.data());
			}
		else {
			res.set_header("Content-Security-Policy",
			               "default-src 'none'; script-src 'self'; "
			               "style-src 'self' 'unsafe-inline'; "
			               "img-src 'self' data: https: http:; "
			               "media-src 'self'; connect-src 'self'; "
			               "font-src 'self'; base-uri 'none'; "
			               "form-action 'self'; frame-ancestors 'none'");
			if (revalidated(req, res, embedded::index_html)) return;
			res.set_content(embedded::index_html.data(), embedded::index_html.size(),
			                embedded::index_html_mime.data());
			}
		};
	server_.Get("/",           index_page);
	server_.Get("/index.html", index_page);
	// The old Subsonic server landed on /index.view after login; keep
	// existing bookmarks working.
	server_.Get("/index.view", [](const httplib::Request&, httplib::Response& res)
		{
		res.set_redirect("/", 301);
		});
	server_.Get("/style.css",
	            static_asset(embedded::style_css, embedded::style_css_mime));
	server_.Get("/app.js",
	            static_asset(embedded::app_js, embedded::app_js_mime));
	server_.Get("/theme.js",
	            static_asset(embedded::theme_js, embedded::theme_js_mime));
	server_.Get("/favicon.svg",
	            static_asset(embedded::favicon_svg, embedded::favicon_svg_mime));
	// Half a megabyte that only changes when the binary does, so it is worth
	// telling the browser not to ask again.
	//
	// The one asset deliberately left out of the revalidation above, and
	// `immutable` is why: it tells the browser never to consult a validator, so
	// an ETag here would be decoration. That is the right trade for a font that
	// has never changed - but it does mean a *replaced* font would be served
	// stale for a year. Fixing that properly means versioning the URL, and this
	// URL lives in style.css's @font-face, so it would need substituting at
	// build time. Worth knowing before anyone swaps the font.
	server_.Get("/material-symbols-rounded.woff2",
	            [](const httplib::Request&, httplib::Response& res) {
		res.set_header("Cache-Control", "public, max-age=31536000, immutable");
		res.set_content(embedded::material_symbols_woff2.data(),
		                embedded::material_symbols_woff2.size(),
		                embedded::material_symbols_woff2_mime.data());
		});
	}

#include "apiclient.hh"
#include "jsonread.hh"
#include "md5.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <httplib.h>
#include <gio/gio.h>
#include <openssl/rand.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace {
	std::string query(const Session& s, const Params& params, const std::string& cast_token="")
		{
		Params all;
		if(cast_token.empty())
			all = {{"u", s.user}, {"t", s.token}, {"s", s.salt}};
		else
			all = {{"castToken", cast_token}};
		all.insert(all.end(), {{"v", "1.16.1"}, {"c", "gaindrive-linux"}, {"f", "json"}});
		all.insert(all.end(), params.begin(), params.end());
		std::string q;
		for(const auto& [k, v]: all) {
			if(!q.empty()) q += "&";
			q += k + "=" + httplib::encode_query_component(v);
			}
		return q;
		}

	// httplib wants the scheme and host apart from the path; the path part
	// matters when a reverse proxy mounts the server under a prefix.
	void split_url(const std::string& url, std::string& origin, std::string& path)
		{
		auto scheme_end = url.find("://");
		auto slash = url.find('/', scheme_end==std::string::npos ? 0 : scheme_end+3);
		origin = url.substr(0, slash);
		path   = slash==std::string::npos ? "/" : url.substr(slash);
		}

	// A Subsonic answer, `body`, into `r`, logging a refusal under `endpoint`.
	void read_answer(const Session& s, const std::string& endpoint, const std::string& body, ApiResult& r)
		{
		auto j = nlohmann::json::parse(body, nullptr, false);
		if(j.is_discarded() || !j.contains("subsonic-response") || !j["subsonic-response"].is_object()) {
			r.error = "The server's answer was not a Subsonic response.";
			r.foreign = true;
			// The start of the body tells an HTML error page from a non-JSON
			// answer to an endpoint the server lacks.
			std::string start = body.substr(0, 120);
			for(auto& c: start)
				if(c=='\n' || c=='\r') c = ' ';
			std::cout << stamp() << s.base_url << " " << endpoint << ": " << r.error
			          << " Body starts: " << start << std::endl;
			return;
			}
		r.body = std::move(j["subsonic-response"]);
		if(jstr(r.body, "status")!="ok") {
			const auto& e = jsub(r.body, "error");
			r.code  = jint(e, "code");
			r.error = jstr(e, "message");
			if(r.error.empty()) r.error = "The server refused the request.";
			std::cout << stamp() << s.base_url << " " << endpoint << ": error " << r.code << ": " << r.error << std::endl;
			return;
			}
		r.ok = true;
		}

	// One kept-alive connection per server and worker thread, so a screenful
	// of covers does not open a connection, and a TLS session, each. httplib
	// reconnects by itself when the server has closed one meanwhile; after a
	// failure the slot is emptied, for a clean start next time.
	std::unique_ptr<httplib::Client>& client_for(const std::string& origin)
		{
		thread_local std::map<std::string, std::unique_ptr<httplib::Client>> clients;
		auto& cli = clients[origin];
		if(!cli) {
			cli = std::make_unique<httplib::Client>(origin);
			cli->set_connection_timeout(5);
			cli->set_read_timeout(20);
			cli->set_follow_location(true);
			cli->set_keep_alive(true);
			}
		return cli;
		}
}

ApiClient::ApiClient(WorkPool& pool, Session session)
	: pool_(pool), session_(std::make_shared<const Session>(std::move(session)))
	{
	}

const Session& ApiClient::session() const
	{
	return *session_;
	}

std::string ApiClient::url(const std::string& endpoint, const Params& params) const
	{
	return session_->base_url + "/rest/" + endpoint + ".view?" + query(*session_, params);
	}

std::string ApiClient::cast_url(const std::string& endpoint, const Params& params,
                                const std::string& token, const char *ext) const
	{
	return session_->base_url + "/rest/" + endpoint + ext + "?" + query(*session_, params, token);
	}

void ApiClient::call(const std::string& endpoint, Params params,
                     std::function<void(const ApiResult&)> done)
	{
	// The session by shared_ptr: a logout may drop this client while the
	// request is still running.
	pool_.submit([s=session_, endpoint, params=std::move(params), done=std::move(done)]() {
		auto r = get_json(*s, endpoint, params);
		on_ui([r=std::move(r), done]() { done(r); });
		});
	}

void ApiClient::text(const std::string& endpoint, Params params,
                     std::function<void(bool, const std::string&)> done)
	{
	fetch(url(endpoint, params), std::move(done));
	}

void ApiClient::fetch(const std::string& u, std::function<void(bool, const std::string&)> done)
	{
	pool_.submit([u, done=std::move(done)]() {
		std::string body, error;
		bool ok = get_bytes(u, body, error);
		// Not the URL, which may carry the account's token.
		if(!ok) std::cout << stamp() << "fetch: " << error << std::endl;
		on_ui([ok, body=std::move(body), done]() { done(ok, body); });
		});
	}

void ApiClient::upload(const std::string& path, Params params,
                       std::function<void(int)> progress,
                       std::shared_ptr<std::atomic<bool>> cancel,
                       std::function<void(const ApiResult&)> done)
	{
	pool_.submit([s=session_, path, params=std::move(params), progress=std::move(progress),
	              cancel, done=std::move(done)]() {
		ApiResult r;
		r.server = s->server_key;
		auto finish = [&r, &done]() {
			if(!r.ok) std::cout << stamp() << "upload: " << r.error << std::endl;
			on_ui([r=std::move(r), done]() { done(r); });
			};
		std::error_code ec;
		auto size = std::filesystem::file_size(path, ec);
		std::ifstream file(path, std::ios::binary);
		if(ec || !file) {
			r.error = "Cannot read " + path;
			finish();
			return;
			}
		// The multipart body by hand, around the file, so that its length is
		// known: httplib's own file provider sends it chunked, which a
		// reverse proxy may buffer whole before passing any of it on, and the
		// progress bar would then measure the proxy.
		std::string name = std::filesystem::path(path).filename().string();
		for(auto& c: name)
			if(c=='"' || c=='\r' || c=='\n') c = '_';
		std::string boundary = "gaindrive-" + random_hex(12);
		std::string head = "--" + boundary + "\r\n"
		                   "Content-Disposition: form-data; name=\"file\"; filename=\"" + name + "\"\r\n"
		                   "Content-Type: application/octet-stream\r\n\r\n";
		std::string tail = "\r\n--" + boundary + "--\r\n";
		size_t total = head.size() + size + tail.size();
		auto provider = [&](size_t offset, size_t length, httplib::DataSink& sink) {
			std::string chunk;
			if(offset<head.size())
				chunk = head.substr(offset, length);
			else if(offset<head.size()+size) {
				chunk.resize(std::min<size_t>(length, std::min<size_t>(head.size()+size-offset, 1<<16)));
				file.seekg(std::streamoff(offset-head.size()));
				if(!file.read(chunk.data(), std::streamsize(chunk.size()))) return false;
				}
			else
				chunk = tail.substr(offset-head.size()-size, length);
			return sink.write(chunk.data(), chunk.size());
			};
		int last = -1;
		auto sent = [&](size_t current, size_t all) {
			if(*cancel) return false;
			int pct = all ? int(current*100/all) : 0;
			if(pct!=last) {
				last = pct;
				on_ui([progress, pct]() { progress(pct); });
				}
			return true;
			};
		std::string origin, prefix;
		split_url(s->base_url, origin, prefix);
		if(prefix=="/") prefix.clear();
		try {
			httplib::Client cli(origin);
			cli.set_connection_timeout(5);
			cli.set_write_timeout(60);
			// The answer comes only once the server has extracted the archive.
			cli.set_read_timeout(600);
			httplib::Headers headers = {{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION}};
			auto res = cli.Post(prefix + "/upload?" + query(*s, params), headers, total, provider,
			                    "multipart/form-data; boundary=" + boundary, sent);
			if(!res) {
				r.error = *cancel ? "Upload cancelled." : httplib::to_string(res.error());
				finish();
				return;
				}
			auto j = nlohmann::json::parse(res->body, nullptr, false);
			if(jstr(j, "status")=="ok") {
				r.ok   = true;
				r.body = std::move(j);
				}
			else {
				r.error = jstr(j, "message");
				// A proxy's own error page, or nothing at all.
				if(r.error.empty()) r.error = "Unexpected response (HTTP " + std::to_string(res->status) + ").";
				}
			}
		catch(const std::exception& ex) {
			r.error = ex.what();
			}
		finish();
		});
	}

void ApiClient::post_form(const std::string& endpoint, Params params, const std::string& path,
                          std::function<void(const ApiResult&)> done)
	{
	pool_.submit([s=session_, endpoint, params=std::move(params), path, done=std::move(done)]() {
		ApiResult r;
		r.server = s->server_key;
		auto finish = [&r, &done]() {
			on_ui([r=std::move(r), done]() { done(r); });
			};
		std::string body, type = "application/x-www-form-urlencoded";
		if(!path.empty()) {
			std::ifstream file(path, std::ios::binary);
			std::ostringstream data;
			if(file) data << file.rdbuf();
			if(!file || data.str().empty()) {
				r.error = "Cannot read " + path;
				std::cout << stamp() << s->base_url << " " << endpoint << ": " << r.error << std::endl;
				finish();
				return;
				}
			// Typed by name and content: the server takes only an image/ part,
			// and says so only after the whole body has gone up.
			auto guess = data.str().substr(0, 4096);
			gchar *ct   = g_content_type_guess(path.c_str(), reinterpret_cast<const guchar*>(guess.data()),
			                                   guess.size(), nullptr);
			gchar *mime = ct ? g_content_type_get_mime_type(ct) : nullptr;
			std::string part_type = mime ? mime : "application/octet-stream";
			g_free(mime);
			g_free(ct);
			std::string name = std::filesystem::path(path).filename().string();
			for(auto& c: name)
				if(c=='"' || c=='\r' || c=='\n') c = '_';
			std::string boundary = "gaindrive-" + random_hex(12);
			body = "--" + boundary + "\r\n"
			       "Content-Disposition: form-data; name=\"file\"; filename=\"" + name + "\"\r\n"
			       "Content-Type: " + part_type + "\r\n\r\n" + data.str() + "\r\n--" + boundary + "--\r\n";
			type = "multipart/form-data; boundary=" + boundary;
			}
		std::string origin, prefix;
		split_url(s->base_url, origin, prefix);
		if(prefix=="/") prefix.clear();
		try {
			httplib::Client cli(origin);
			cli.set_connection_timeout(5);
			cli.set_write_timeout(60);
			// A url= is fetched by the server before it answers.
			cli.set_read_timeout(60);
			httplib::Headers headers = {{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION}};
			auto res = cli.Post(prefix + "/rest/" + endpoint + ".view?" + query(*s, params), headers, body, type);
			if(!res)
				r.error = httplib::to_string(res.error());
			else if(res->status!=200)
				r.error = "Server returned HTTP " + std::to_string(res->status);
			else
				read_answer(*s, endpoint, res->body, r);
			}
		catch(const std::exception& ex) {
			r.error = ex.what();
			}
		if(!r.ok && r.code==0) std::cout << stamp() << s->base_url << " " << endpoint << ": " << r.error << std::endl;
		finish();
		});
	}

bool ApiClient::get_bytes(const std::string& url, std::string& body, std::string& error,
                          int read_timeout)
	{
	auto f = get_if_changed(url, "", read_timeout);
	if(f.status!=200) {
		error = std::move(f.error);
		return false;
		}
	body = std::move(f.body);
	return true;
	}

ApiClient::Fetch ApiClient::get_if_changed(const std::string& url, const std::string& etag,
                                           int read_timeout)
	{
	Fetch f;
	std::string origin, path;
	split_url(url, origin, path);
	try {
		auto& cli = client_for(origin);
		// Per request: the client is kept alive and shared by every call.
		cli->set_read_timeout(read_timeout);
		httplib::Headers headers = {{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION}};
		if(!etag.empty()) headers.emplace("If-None-Match", etag);
		auto res = cli->Get(path, headers);
		// A kept-alive socket the server closed just as we reused it passes
		// httplib's liveness check and then fails on the write or the read.
		// The request never reached the server, so one retry on a fresh
		// connection is safe for a GET.
		if(!res && (res.error()==httplib::Error::Read || res.error()==httplib::Error::Write)) {
			std::cout << stamp() << "retrying after stale connection: "
			          << httplib::to_string(res.error()) << std::endl;
			res = cli->Get(path, headers);
			}
		if(!res) {
			f.error = httplib::to_string(res.error());
			cli.reset();
			return f;
			}
		f.status = res->status;
		f.etag   = res->get_header_value("ETag");
		if(f.status==200)
			f.body = std::move(res->body);
		else if(f.status!=304) {
			f.error = "Server returned HTTP " + std::to_string(f.status);
			auto cc = res->get_header_value("Cache-Control");
			auto at = cc.find("max-age=");
			if(at!=std::string::npos)
				f.miss_max_age = std::atoi(cc.c_str()+at+8);
			}
		}
	// httplib throws on a URL it cannot parse, which a typed server address
	// can easily be.
	catch(const std::exception& ex) {
		f.error = ex.what();
		}
	return f;
	}

std::string ApiClient::sent_of(const std::string& url)
	{
	std::string origin, path, sent;
	split_url(url, origin, path);
	try {
		httplib::Client cli(origin);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(20);
		cli.set_follow_location(true);
		httplib::Headers headers = {{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION},
		                            {"Range", "bytes=0-0"}};
		// Refusing the body ends the request: the answer is in the headers.
		cli.Get(path, headers, [&sent](const httplib::Response& r) {
			sent = r.get_header_value("X-Gaindrive-Sent");
			return false;
			}, [](const char *, size_t) { return false; });
		}
	catch(const std::exception& ex) {
		std::cout << stamp() << "sent_of: " << ex.what() << std::endl;
		}
	return sent;
	}

ApiResult ApiClient::get_json(const Session& s, const std::string& endpoint, const Params& params,
                              int read_timeout)
	{
	ApiResult r;
	r.server = s.server_key;
	std::string url = s.base_url + "/rest/" + endpoint + ".view?" + query(s, params);
	std::string body;
	if(!get_bytes(url, body, r.error, read_timeout)) {
		std::cout << stamp() << s.base_url << " " << endpoint << ": " << r.error << std::endl;
		return r;
		}
	read_answer(s, endpoint, body, r);
	return r;
	}

void ApiClient::login(WorkPool& pool, const std::string& url, const std::string& user,
                      const std::string& password,
                      std::function<void(const ApiResult&, const Session&)> done)
	{
	Session s;
	s.base_url = normalise_server_url(url);
	s.user     = user;
	// Eight bytes, sixteen hex characters: above the six the server requires.
	s.salt     = random_hex(8);
	s.token    = md5_hex(password + s.salt);

	pool.submit([s, done=std::move(done)]() {
		auto r = get_json(s, "ping", {});
		// A ping alone proves little: some Subsonic servers answer it with ok
		// whatever the credentials. getUser does check them, and its roles
		// will be needed later anyway.
		if(r.ok)
			r = get_json(s, "getUser", {{"username", s.user}});
		on_ui([r=std::move(r), s, done]() { done(r, s); });
		});
	}

std::string normalise_server_url(std::string url)
	{
	while(!url.empty() && (url.back()==' ' || url.back()=='/'))
		url.pop_back();
	while(!url.empty() && url.front()==' ')
		url.erase(0, 1);
	if(url.size()>=5 && url.compare(url.size()-5, 5, "/rest")==0)
		url.erase(url.size()-5);
	if(url.find("://")==std::string::npos)
		url = "https://" + url;
	return url;
	}

std::string random_hex(int nbytes)
	{
	std::vector<unsigned char> buf(nbytes);
	if(RAND_bytes(buf.data(), nbytes)!=1)
		throw std::runtime_error("RAND_bytes failed");
	static const char *hex = "0123456789abcdef";
	std::string out;
	for(auto b: buf) {
		out += hex[b>>4];
		out += hex[b&15];
		}
	return out;
	}

void call_each(const ServerCalls& calls, const std::string& endpoint,
               std::function<void(const std::vector<ApiResult>&)> done)
	{
	if(calls.empty()) {
		done({});
		return;
		}
	struct Gather {
		std::vector<ApiResult> results;
		size_t                 left;
	};
	auto g = std::make_shared<Gather>(Gather{std::vector<ApiResult>(calls.size()), calls.size()});
	for(size_t i=0; i<calls.size(); ++i)
		calls[i].first->call(endpoint, calls[i].second, [g, i, done](const ApiResult& r) {
			g->results[i] = r;
			if(--g->left==0) done(g->results);
			});
	}

void call_each(const std::vector<std::shared_ptr<ApiClient>>& apis, const std::string& endpoint,
               const Params& params, std::function<void(const std::vector<ApiResult>&)> done)
	{
	ServerCalls calls;
	for(const auto& api: apis)
		calls.push_back({api, params});
	call_each(calls, endpoint, std::move(done));
	}

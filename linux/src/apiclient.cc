#include "apiclient.hh"
#include "jsonread.hh"
#include "md5.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <httplib.h>
#include <openssl/rand.h>

#include <cstdlib>
#include <iostream>
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
                                const std::string& token) const
	{
	return session_->base_url + "/rest/" + endpoint + ".view?" + query(*session_, params, token);
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
	std::string u = url(endpoint, params);
	pool_.submit([u, done=std::move(done)]() {
		std::string body, error;
		bool ok = get_bytes(u, body, error);
		on_ui([ok, body=std::move(body), done]() { done(ok, body); });
		});
	}

bool ApiClient::get_bytes(const std::string& url, std::string& body, std::string& error)
	{
	auto f = get_if_changed(url, "");
	if(f.status!=200) {
		error = std::move(f.error);
		return false;
		}
	body = std::move(f.body);
	return true;
	}

ApiClient::Fetch ApiClient::get_if_changed(const std::string& url, const std::string& etag)
	{
	Fetch f;
	std::string origin, path;
	split_url(url, origin, path);
	try {
		httplib::Client cli(origin);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(20);
		cli.set_follow_location(true);
		httplib::Headers headers = {{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION}};
		if(!etag.empty()) headers.emplace("If-None-Match", etag);
		auto res = cli.Get(path, headers);
		if(!res) {
			f.error = httplib::to_string(res.error());
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

bool ApiClient::probe_stream(const std::string& url, std::string& content_type,
                             std::string& error)
	{
	std::string origin, path;
	split_url(url, origin, path);
	try {
		httplib::Client cli(origin);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(300);
		cli.set_follow_location(true);
		cli.set_default_headers({{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION},
		                         {"Range", "bytes=0-0"}});
		auto res = cli.Get(path);
		if(!res) {
			error = httplib::to_string(res.error());
			return false;
			}
		if(res->status!=200 && res->status!=206) {
			error = "Server returned HTTP " + std::to_string(res->status);
			return false;
			}
		content_type = res->get_header_value("Content-Type");
		// An error document is JSON; audio never is.
		if(content_type.find("json")!=std::string::npos || content_type.find("xml")!=std::string::npos) {
			error = "Server sent " + content_type + " instead of audio";
			return false;
			}
		return true;
		}
	catch(const std::exception& ex) {
		error = ex.what();
		return false;
		}
	}

ApiResult ApiClient::get_json(const Session& s, const std::string& endpoint, const Params& params)
	{
	ApiResult r;
	std::string url = s.base_url + "/rest/" + endpoint + ".view?" + query(s, params);
	std::string body;
	if(!get_bytes(url, body, r.error)) {
		std::cout << stamp() << endpoint << ": " << r.error << std::endl;
		return r;
		}
	auto j = nlohmann::json::parse(body, nullptr, false);
	if(j.is_discarded() || !j.contains("subsonic-response") || !j["subsonic-response"].is_object()) {
		r.error = "The server's answer was not a Subsonic response.";
		std::cout << stamp() << endpoint << ": " << r.error << std::endl;
		return r;
		}
	r.body = std::move(j["subsonic-response"]);
	if(jstr(r.body, "status")!="ok") {
		const auto& e = jsub(r.body, "error");
		r.code  = jint(e, "code");
		r.error = jstr(e, "message");
		if(r.error.empty()) r.error = "The server refused the request.";
		std::cout << stamp() << endpoint << ": error " << r.code << ": " << r.error << std::endl;
		return r;
		}
	r.ok = true;
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

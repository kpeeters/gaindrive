#include "apiclient.hh"
#include "jsonread.hh"
#include "md5.hh"
#include "stamp.hh"
#include "uithread.hh"
#include "workpool.hh"

#include <httplib.h>
#include <openssl/rand.h>

#include <iostream>
#include <stdexcept>

namespace {
	std::string query(const Session& s, const Params& params)
		{
		Params all = {
			{"u", s.user}, {"t", s.token}, {"s", s.salt},
			{"v", "1.16.1"}, {"c", "gaindrive-linux"}, {"f", "json"}
		};
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

bool ApiClient::get_bytes(const std::string& url, std::string& body, std::string& error)
	{
	std::string origin, path;
	split_url(url, origin, path);
	try {
		httplib::Client cli(origin);
		cli.set_connection_timeout(5);
		cli.set_read_timeout(20);
		cli.set_follow_location(true);
		cli.set_default_headers({{"User-Agent", "gaindrive-player/" GAINDRIVE_VERSION}});
		auto res = cli.Get(path);
		if(!res) {
			error = httplib::to_string(res.error());
			return false;
			}
		if(res->status!=200) {
			error = "Server returned HTTP " + std::to_string(res->status);
			return false;
			}
		body = std::move(res->body);
		return true;
		}
	// httplib throws on a URL it cannot parse, which a typed server address
	// can easily be.
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

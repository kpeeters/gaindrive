// Chromecast device discovery (mDNS) and playback control (Cast v2 over TLS).
//
// Cast protocol wire format is hand-encoded protobuf — the CastMessage schema
// has only six fields, so we avoid a full protobuf library dependency.

#define MDNS_IMPLEMENTATION
#include <mdns.h>

#include "castmanager.hh"
#include "stamp.hh"

#include <cstdio>
#include <cstring>
#include <iostream>
#include <map>
#include <chrono>
#include <thread>

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <ifaddrs.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

static const char* NS_CONN      = "urn:x-cast:com.google.cast.tp.connection";
static const char* NS_RECV      = "urn:x-cast:com.google.cast.receiver";
static const char* NS_MEDIA     = "urn:x-cast:com.google.cast.media";
static const char* NS_HEARTBEAT = "urn:x-cast:com.google.cast.tp.heartbeat";

// ---- mDNS discovery -----------------------------------------------

// Find the interface index of the first non-loopback, up, IPv6-capable interface.
// Needed because sending to ff02::fb (link-local multicast) requires a scope.
static unsigned int find_ipv6_if()
	{
	struct ifaddrs* iflist;
	if (getifaddrs(&iflist) < 0) return 0;
	unsigned int idx = 0;
	for (struct ifaddrs* ifa = iflist; ifa && !idx; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr) continue;
		if (ifa->ifa_addr->sa_family != AF_INET6) continue;
		if (ifa->ifa_flags & IFF_LOOPBACK) continue;
		if (!(ifa->ifa_flags & IFF_UP)) continue;
		idx = if_nametoindex(ifa->ifa_name);
		}
	freeifaddrs(iflist);
	return idx;
	}

// State accumulated across mDNS response packets within one discover() call.
struct DiscState {
	std::map<std::string, CastManager::CastDevice> devs;   // service instance → device
	std::map<std::string, std::string>              hosts;  // hostname → IPv4 address
	std::map<std::string, std::string>              hosts6; // hostname → IPv6 address (with scope)
	std::map<std::string, std::string>              srvs;   // service instance → SRV host
	};

static int mdns_cb(int, const struct sockaddr* from, size_t,
                    mdns_entry_type_t, uint16_t,
                    uint16_t rtype, uint16_t, uint32_t,
                    const void* data, size_t size,
                    size_t name_off, size_t,
                    size_t rec_off, size_t rec_len,
                    void* user_data)
	{
	auto* st = reinterpret_cast<DiscState*>(user_data);
	char buf1[256], buf2[256];

	size_t off = name_off;
	mdns_string_t rname = mdns_string_extract(data, size, &off, buf1, sizeof(buf1));
	std::string key(rname.str, rname.length);

	// Extract sender IP — used as address fallback and for AAAA scope IDs.
	std::string src_ip4, src_ip6;
	if (from) {
		if (from->sa_family == AF_INET) {
			char ip[INET_ADDRSTRLEN];
			inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in*>(from)->sin_addr,
			          ip, sizeof(ip));
			src_ip4 = ip;
			}
		else if (from->sa_family == AF_INET6) {
			const auto* s6 = reinterpret_cast<const struct sockaddr_in6*>(from);
			char ip[INET6_ADDRSTRLEN];
			inet_ntop(AF_INET6, &s6->sin6_addr, ip, sizeof(ip));
			src_ip6 = ip;
			if (s6->sin6_scope_id) {
				char iface[IF_NAMESIZE] = {};
				if (if_indextoname(s6->sin6_scope_id, iface))
					src_ip6 += std::string("%") + iface;
				}
			}
		}
	// Prefer IPv4 as device address; fall back to IPv6.
	std::string src_ip = src_ip4.empty() ? src_ip6 : src_ip4;

	if (rtype == MDNS_RECORDTYPE_PTR) {
		std::cout << stamp() << "Cast mDNS: PTR  from=" << src_ip
		          << " name=" << key << std::endl;
		}
	else if (rtype == MDNS_RECORDTYPE_TXT) {
		mdns_record_txt_t txt[32];
		size_t n = mdns_record_parse_txt(data, size, rec_off, rec_len, txt, 32);
		auto& dev = st->devs[key];
		if (!src_ip.empty() && dev.address.empty())
			dev.address = src_ip;
		for (size_t i = 0; i < n; i++) {
			std::string k(txt[i].key.str,   txt[i].key.length);
			std::string v(txt[i].value.str, txt[i].value.length);
			if (k == "id") dev.id   = v;
			if (k == "fn") dev.name = v;
			}
		std::cout << stamp() << "Cast mDNS: TXT  from=" << src_ip
		          << " name=" << key
		          << " id=" << dev.id
		          << " fn=" << dev.name << std::endl;
		}
	else if (rtype == MDNS_RECORDTYPE_SRV) {
		mdns_record_srv_t srv = mdns_record_parse_srv(
			data, size, rec_off, rec_len, buf2, sizeof(buf2));
		auto& dev = st->devs[key];
		dev.port = srv.port;
		if (!src_ip.empty() && dev.address.empty())
			dev.address = src_ip;
		st->srvs[key] = std::string(srv.name.str, srv.name.length);
		std::cout << stamp() << "Cast mDNS: SRV  from=" << src_ip
		          << " name=" << key
		          << " target=" << st->srvs[key]
		          << " port=" << srv.port << std::endl;
		}
	else if (rtype == MDNS_RECORDTYPE_A) {
		struct sockaddr_in a4 = {};
		mdns_record_parse_a(data, size, rec_off, rec_len, &a4);
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip));
		st->hosts[key] = ip;
		std::cout << stamp() << "Cast mDNS: A    from=" << src_ip
		          << " name=" << key << " addr=" << ip << std::endl;
		}
	else if (rtype == MDNS_RECORDTYPE_AAAA) {
		struct sockaddr_in6 a6 = {};
		mdns_record_parse_aaaa(data, size, rec_off, rec_len, &a6);
		char ip[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &a6.sin6_addr, ip, sizeof(ip));
		// Attach the scope ID from the sender for link-local addresses.
		std::string addr6 = ip;
		size_t scope = src_ip6.find('%');
		if (scope != std::string::npos)
			addr6 += src_ip6.substr(scope);
		st->hosts6[key] = addr6;
		std::cout << stamp() << "Cast mDNS: AAAA from=" << src_ip
		          << " name=" << key << " addr=" << addr6 << std::endl;
		}
	else {
		std::cout << stamp() << "Cast mDNS: type=" << rtype
		          << " from=" << src_ip << " name=" << key << std::endl;
		}

	return 0;
	}

std::vector<CastManager::CastDevice> CastManager::discover(int timeout_ms)
	{
	DiscState state;

	struct sockaddr_in saddr4 = {};
	saddr4.sin_family      = AF_INET;
	saddr4.sin_addr.s_addr = INADDR_ANY;
	saddr4.sin_port        = htons(MDNS_PORT);

	int sock4 = mdns_socket_open_ipv4(&saddr4);
	if (sock4 < 0)
		std::cout << stamp() << "Cast: failed to open IPv4 mDNS socket (errno "
		          << errno << ")" << std::endl;

	// IPv6 is required on many modern networks where devices only respond via
	// ff02::fb multicast.
	int sock6 = mdns_socket_open_ipv6(nullptr);
	if (sock6 < 0) {
		std::cout << stamp() << "Cast: failed to open IPv6 mDNS socket (errno "
		          << errno << ")" << std::endl;
		}
	else {
		// Sending to ff02::fb (link-local multicast) requires the kernel to know
		// which interface to use. Set IPV6_MULTICAST_IF and re-join with that
		// interface so both send and receive work correctly.
		unsigned int ifidx = find_ipv6_if();
		if (ifidx == 0) {
			std::cout << stamp() << "Cast: no suitable IPv6 interface found" << std::endl;
			}
		else {
			setsockopt(sock6, IPPROTO_IPV6, IPV6_MULTICAST_IF,
			           &ifidx, sizeof(ifidx));
			struct ipv6_mreq req = {};
			req.ipv6mr_multiaddr.s6_addr[0]  = 0xFF;
			req.ipv6mr_multiaddr.s6_addr[1]  = 0x02;
			req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
			req.ipv6mr_interface = ifidx;
			setsockopt(sock6, IPPROTO_IPV6, IPV6_JOIN_GROUP,
			           &req, sizeof(req));
			std::cout << stamp() << "Cast: IPv6 multicast on interface index "
			          << ifidx << std::endl;
			}
		}

	if (sock4 < 0 && sock6 < 0)
		return {};

	std::cout << stamp() << "Cast: querying for " << timeout_ms << " ms"
	          << " (IPv4=" << (sock4 >= 0 ? "ok" : "fail")
	          << " IPv6=" << (sock6 >= 0 ? "ok" : "fail") << ")" << std::endl;

	std::vector<uint8_t> buf(4096);
	static const char svc[] = "_googlecast._tcp.local.";

	// mdns_query_send returns int; storing in uint16_t would corrupt -1 → 65535,
	// which would then cause mdns_query_recv to reject all ID-0 mDNS responses.
	if (sock4 >= 0) {
		int r = mdns_query_send(sock4, MDNS_RECORDTYPE_PTR,
		                        svc, strlen(svc), buf.data(), buf.size(), 0);
		std::cout << stamp() << "Cast: IPv4 query "
		          << (r < 0 ? "failed" : "sent") << std::endl;
		}
	if (sock6 >= 0) {
		int r = mdns_query_send(sock6, MDNS_RECORDTYPE_PTR,
		                        svc, strlen(svc), buf.data(), buf.size(), 0);
		std::cout << stamp() << "Cast: IPv6 query "
		          << (r < 0 ? "failed" : "sent") << std::endl;
		}

	auto deadline = std::chrono::steady_clock::now()
	              + std::chrono::milliseconds(timeout_ms);

	while (std::chrono::steady_clock::now() < deadline) {
		auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		              deadline - std::chrono::steady_clock::now()).count();
		if (us <= 0) break;
		// Cast to the real member types rather than long: on Darwin tv_usec is
		// suseconds_t (int), so a long here narrows inside a braced initialiser,
		// which is ill-formed and rejected outright rather than warned about.
		struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = 0;
		if (sock4 >= 0) { FD_SET(sock4, &fds); maxfd = std::max(maxfd, sock4); }
		if (sock6 >= 0) { FD_SET(sock6, &fds); maxfd = std::max(maxfd, sock6); }
		int r = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
		if (r < 0 && errno == EINTR) continue;
		if (r <= 0) break;
		// Pass 0 as only_query_id: mDNS responses always carry ID 0 per RFC 6762.
		if (sock4 >= 0 && FD_ISSET(sock4, &fds))
			mdns_query_recv(sock4, buf.data(), buf.size(), mdns_cb, &state, 0);
		if (sock6 >= 0 && FD_ISSET(sock6, &fds))
			mdns_query_recv(sock6, buf.data(), buf.size(), mdns_cb, &state, 0);
		}

	if (sock4 >= 0) mdns_socket_close(sock4);
	if (sock6 >= 0) mdns_socket_close(sock6);

	// Resolve each SRV target hostname → IPv4 A record, then IPv6 AAAA as fallback.
	for (auto& [inst, dev] : state.devs) {
		if (!dev.address.empty()) continue;
		auto ti = state.srvs.find(inst);
		if (ti == state.srvs.end()) continue;
		auto hi = state.hosts.find(ti->second);
		if (hi != state.hosts.end()) { dev.address = hi->second; continue; }
		auto hi6 = state.hosts6.find(ti->second);
		if (hi6 != state.hosts6.end()) dev.address = hi6->second;
		}

	std::cout << stamp() << "Cast: discovery done — "
	          << state.devs.size() << " instance(s) seen" << std::endl;
	for (auto& [inst, dev] : state.devs)
		std::cout << stamp() << "  inst=" << inst
		          << " id=" << dev.id
		          << " fn=" << dev.name
		          << " addr=" << dev.address
		          << " port=" << dev.port << std::endl;

	std::vector<CastDevice> result;
	for (auto& [inst, dev] : state.devs)
		if (!dev.id.empty() && !dev.address.empty())
			result.push_back(dev);
	return result;
	}

void CastManager::discover_background(int timeout_ms)
	{
	std::thread([this, timeout_ms]{
		auto devs = discover(timeout_ms);
		std::lock_guard<std::mutex> lk(cache_mutex_);
		devices_cache_ = std::move(devs);
		}).detach();
	}

std::vector<CastManager::CastDevice> CastManager::cached_devices() const
	{
	std::lock_guard<std::mutex> lk(cache_mutex_);
	return devices_cache_;
	}

// ---- Protobuf helpers (CastMessage encoding / decoding) -----------
//
// CastMessage schema (cast_channel.proto):
//   field 1 varint  — protocol_version (0 = CASTV2_1_0)
//   field 2 string  — source_id
//   field 3 string  — destination_id
//   field 4 string  — namespace
//   field 5 varint  — payload_type (0 = STRING)
//   field 6 string  — payload_utf8

static void pb_varint(std::vector<uint8_t>& out, uint64_t v)
	{
	while (v > 127) { out.push_back((v & 0x7f) | 0x80); v >>= 7; }
	out.push_back((uint8_t)v);
	}

static void pb_varint_field(std::vector<uint8_t>& out, int f, uint64_t v)
	{
	pb_varint(out, ((uint64_t)f << 3) | 0);
	pb_varint(out, v);
	}

static void pb_string_field(std::vector<uint8_t>& out, int f, const std::string& s)
	{
	pb_varint(out, ((uint64_t)f << 3) | 2);
	pb_varint(out, s.size());
	out.insert(out.end(), s.begin(), s.end());
	}

static uint64_t pb_varint_decode(const uint8_t* d, size_t sz, size_t& pos)
	{
	uint64_t r = 0; int sh = 0;
	while (pos < sz) {
		uint8_t b = d[pos++];
		r |= (uint64_t)(b & 0x7f) << sh;
		sh += 7;
		if (!(b & 0x80)) break;
		}
	return r;
	}

// Extract the payload_utf8 field (field 6) from a raw CastMessage.
static std::string pb_payload(const std::vector<uint8_t>& msg)
	{
	size_t pos = 0;
	while (pos < msg.size()) {
		uint64_t tag = pb_varint_decode(msg.data(), msg.size(), pos);
		int field = (int)(tag >> 3), wire = (int)(tag & 7);
		if (wire == 2) {
			uint64_t len = pb_varint_decode(msg.data(), msg.size(), pos);
			if (field == 6 && pos + len <= msg.size())
				return std::string(msg.begin() + pos, msg.begin() + pos + len);
			pos += (size_t)len;
			}
		else if (wire == 0) { pb_varint_decode(msg.data(), msg.size(), pos); }
		else break;
		}
	return {};
	}

// ---- TLS connection (RAII) ----------------------------------------

struct Tls {
	SSL_CTX* ctx  = nullptr;
	SSL*     ssl  = nullptr;
	int      sock = -1;

	~Tls()
		{
		if (ssl)    { SSL_shutdown(ssl); SSL_free(ssl); }
		if (ctx)    SSL_CTX_free(ctx);
		if (sock >= 0) ::close(sock);
		}
	Tls() = default;
	Tls(const Tls&) = delete;
	Tls& operator=(const Tls&) = delete;
	};

static bool tls_connect(Tls& t, const std::string& addr, int port)
	{
	t.ctx = SSL_CTX_new(TLS_client_method());
	// Chromecasts use self-signed certificates; we verify the device via the
	// Cast token instead.
	SSL_CTX_set_verify(t.ctx, SSL_VERIFY_NONE, nullptr);

	struct timeval tv = {5, 0};  // 5-second timeout per operation

	bool is_ipv6 = addr.find(':') != std::string::npos;
	if (is_ipv6) {
		t.sock = socket(AF_INET6, SOCK_STREAM, 0);
		if (t.sock < 0) return false;
		setsockopt(t.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(t.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		struct sockaddr_in6 sa = {};
		sa.sin6_family = AF_INET6;
		sa.sin6_port   = htons(port);
		// Strip %iface scope suffix for inet_pton, then apply scope_id.
		std::string bare = addr;
		size_t scope = addr.find('%');
		if (scope != std::string::npos) {
			bare = addr.substr(0, scope);
			sa.sin6_scope_id = if_nametoindex(addr.substr(scope + 1).c_str());
			}
		if (inet_pton(AF_INET6, bare.c_str(), &sa.sin6_addr) != 1) return false;
		if (::connect(t.sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) return false;
		}
	else {
		t.sock = socket(AF_INET, SOCK_STREAM, 0);
		if (t.sock < 0) return false;
		setsockopt(t.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
		setsockopt(t.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

		struct sockaddr_in sa = {};
		sa.sin_family = AF_INET;
		sa.sin_port   = htons(port);
		if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) return false;
		if (::connect(t.sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) return false;
		}

	t.ssl = SSL_new(t.ctx);
	SSL_set_fd(t.ssl, t.sock);
	return SSL_connect(t.ssl) == 1;
	}

// ---- Cast message send / receive ----------------------------------

static bool cast_send(SSL* ssl, const std::string& ns,
                       const std::string& src, const std::string& dst,
                       const nlohmann::json& payload)
	{
	std::string js = payload.dump();
	std::vector<uint8_t> msg;
	pb_varint_field(msg, 1, 0);   // protocol_version = CASTV2_1_0
	pb_string_field(msg, 2, src);
	pb_string_field(msg, 3, dst);
	pb_string_field(msg, 4, ns);
	pb_varint_field(msg, 5, 0);   // payload_type = STRING
	pb_string_field(msg, 6, js);

	uint32_t len = (uint32_t)msg.size();
	uint8_t fr[4] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16),
	                  (uint8_t)(len >>  8), (uint8_t)len };
	return SSL_write(ssl, fr, 4) == 4
	    && SSL_write(ssl, msg.data(), (int)msg.size()) == (int)msg.size();
	}

static nlohmann::json cast_recv(SSL* ssl)
	{
	uint8_t fr[4]; size_t got = 0;
	while (got < 4) {
		int r = SSL_read(ssl, fr + got, (int)(4 - got));
		if (r <= 0) return nullptr;
		got += (size_t)r;
		}
	uint32_t len = ((uint32_t)fr[0] << 24) | ((uint32_t)fr[1] << 16)
	             | ((uint32_t)fr[2] <<  8) |  (uint32_t)fr[3];
	if (len == 0 || len > (1u << 20)) return nullptr;
	std::vector<uint8_t> msg(len);
	got = 0;
	while (got < len) {
		int r = SSL_read(ssl, msg.data() + got, (int)(len - got));
		if (r <= 0) return nullptr;
		got += (size_t)r;
		}
	std::string payload = pb_payload(msg);
	if (payload.empty()) return nullptr;
	return nlohmann::json::parse(payload, nullptr, false);
	}

// Poll until we see a RECEIVER_STATUS that includes a running application.
// Returns the application's transportId, or empty string on timeout/error.
static std::string wait_transport(SSL* ssl)
	{
	for (int i = 0; i < 20; i++) {
		auto m = cast_recv(ssl);
		if (m.is_null()) break;
		if (m.value("type", "") == "RECEIVER_STATUS") {
			auto& apps = m["status"]["applications"];
			if (apps.is_array() && !apps.empty())
				return apps[0].value("transportId", "");
			}
		}
	return {};
	}

// ---- CastManager public methods -----------------------------------

bool CastManager::start(const CastDevice& dev)
	{
	device_ = dev;
	active_ = true;

	// Generate a 128-bit random token. The Chromecast includes this when it
	// fetches the stream from us, so we can skip normal credential auth.
	uint8_t bytes[16];
	RAND_bytes(bytes, sizeof(bytes));
	char hex[33];
	for (int i = 0; i < 16; i++) snprintf(hex + 2*i, 3, "%02x", bytes[i]);
	token_ = hex;

	// Start the background status polling thread.
	poll_active_ = true;
	std::thread([this]{ poll_loop(); }).detach();

	std::cout << stamp() << "Cast: enabled → " << dev.name
	          << " (" << dev.address << ":" << dev.port << ")" << std::endl;
	return true;
	}

void CastManager::load(const std::string& url, const std::string& mime,
                        float current_time, double duration)
	{
	// Signal any running content-provider thread to stop immediately.
	int gen = ++load_gen_;
	std::cout << stamp() << "Cast: load gen=" << gen << " url=" << url
	          << " currentTime=" << current_time << std::endl;

	// Reset playback status for the new track and arm the auto-retry
	// watcher.  The retry covers the receiver-side bug where a LOAD that
	// interrupts an active session produces an IDLE/ERROR new session;
	// see the comment in castmanager.hh next to retry_pending_.
	// last_load_old_msid_ remembers the session being replaced so that
	// stale status pushes for it (e.g. a PLAYING position update arriving
	// as the receiver's response to poll_loop's GET_STATUS that fired
	// just before LOAD reached the receiver) don't fool the retry watcher
	// into thinking the new LOAD succeeded.
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	last_load_old_msid_ = status_.media_session_id;
	status_ = CastStatus{};
	status_.duration    = static_cast<float>(duration);
	last_load_url_      = url;
	last_load_mime_     = mime;
	last_load_time_     = current_time;
	last_load_duration_ = duration;
	retry_pending_      = true;
	}

	// All TLS/blocking work happens in a detached thread so that the
	// castLoad.view handler (and the httplib thread it runs on) returns
	// immediately.  load_gen_ acts as a cancellation token: if a newer
	// load() fires before this worker reaches a blocking call, the worker
	// detects the stale generation and exits without doing any work.
	std::thread([this, url, mime, gen, current_time, duration]{
		load_worker(url, mime, gen, current_time, duration);
		}).detach();
	}

void CastManager::load_worker(std::string url, std::string mime, int gen,
                               float current_time, double duration)
	{
	std::string src = "sender-0";

	std::string tid;
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	tid = transport_id_;
	}

	if (!tid.empty()) {
		// Fast path: app already running — LOAD into the existing session.
		if (load_gen_.load() != gen) return;
		Tls t;
		if (!tls_connect(t, device_.address, device_.port)) {
			std::cout << stamp() << "Cast: connect failed" << std::endl;
			// Treat as stale transport; fall through to LAUNCH.
			{
			std::lock_guard<std::mutex> lk(tid_mutex_);
			transport_id_.clear();
			}
			}
		else {
			if (load_gen_.load() != gen) return;
			cast_send(t.ssl, NS_CONN,  src, "receiver-0", {{"type", "CONNECT"}});
			cast_send(t.ssl, NS_CONN,  src, tid,           {{"type", "CONNECT"}});
			nlohmann::json msg = {
				{"type",      "LOAD"},
				{"requestId", 2},
				{"autoplay",  true},
				{"media", {
					{"contentId",   url},
					{"contentType", mime},
					{"streamType",  "BUFFERED"},
					{"duration",    duration}
					}}
				};
			// currentTime tells the receiver to start playback at this offset
			// within the loaded media — the receiver handles the seek itself
			// using the file's XING/seek tables.  This avoids server-side
			// transcoding and gives the receiver real duration metadata.
			if (current_time > 0.0f)
				msg["currentTime"] = current_time;
			std::cout << stamp() << "Cast: LOAD " << msg.dump() << std::endl;
			cast_send(t.ssl, NS_MEDIA, src, tid, msg);
			// poll_loop() receives the MEDIA_STATUS response and updates status_.
			// No need to wait here — that would block an httplib thread.
			std::cout << stamp() << "Cast: → " << url << std::endl;
			return;
			}
		}

	// Slow path: launch the Default Media Receiver app first.
	if (load_gen_.load() != gen) return;
	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		std::cout << stamp() << "Cast: connect failed ("
		          << device_.address << ":" << device_.port << ")" << std::endl;
		return;
		}

	cast_send(t.ssl, NS_CONN, src, "receiver-0", {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_RECV, src, "receiver-0",
	          {{"type", "LAUNCH"}, {"appId", "CC1AD845"}, {"requestId", 1}});

	tid = wait_transport(t.ssl);
	if (tid.empty()) {
		std::cout << stamp() << "Cast: no transportId from receiver" << std::endl;
		return;
		}

	// Only store the new transport_id if this is still the current load.
	if (load_gen_.load() != gen) return;
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	transport_id_ = tid;
	}

	cast_send(t.ssl, NS_CONN,  src, tid, {{"type", "CONNECT"}});
	{
	nlohmann::json msg = {
		{"type",      "LOAD"},
		{"requestId", 2},
		{"autoplay",  true},
		{"media", {
			{"contentId",   url},
			{"contentType", mime},
			{"streamType",  "BUFFERED"},
			{"duration",    duration}
			}}
		};
	if (current_time > 0.0f)
		msg["currentTime"] = current_time;
	std::cout << stamp() << "Cast: LOAD " << msg.dump() << std::endl;
	cast_send(t.ssl, NS_MEDIA, src, tid, msg);
	}

	std::cout << stamp() << "Cast: → " << url << std::endl;
	}

void CastManager::update_status(const nlohmann::json& msg)
	{
	auto& list = msg["status"];
	if (!list.is_array() || list.empty()) return;
	auto& s = list[0];
	CastStatus cs;
	cs.player_state     = s.value("playerState",    "IDLE");
	cs.current_time     = s.value("currentTime",     0.0f);
	cs.media_session_id = s.value("mediaSessionId",  0);
	cs.idle_reason      = s.value("idleReason",     "");
	if (s.contains("media") && s["media"].contains("duration")
	                        && s["media"]["duration"].is_number())
		cs.duration = s["media"]["duration"].get<float>();

	bool        do_retry = false;
	std::string retry_url, retry_mime;
	float       retry_time = 0.0f;
	double      retry_dur  = 0.0;
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	if (cs.player_state != "IDLE")
		last_known_time_ = cs.current_time;
	if (cs.duration == 0.0f)
		cs.duration = status_.duration;
	status_ = cs;

	// Auto-retry decision: only consider pushes for the NEW media session
	// (msid != last_load_old_msid_).  Otherwise a stale PLAYING update for
	// the old session — typically poll_loop's GET_STATUS reply that
	// crosses our LOAD on the wire — would clear the flag prematurely and
	// the real IDLE/ERROR for the new msid would arrive too late to
	// trigger a retry.  IDLE/INTERRUPTED also carries the old msid, so it
	// is skipped here as well; that's fine — the next push (the new msid
	// going to PLAYING or ERROR) is the one we actually need.
	if (retry_pending_ && cs.media_session_id != last_load_old_msid_) {
		if (cs.player_state == "IDLE" && cs.idle_reason == "ERROR") {
			retry_pending_ = false;
			do_retry       = true;
			retry_url      = last_load_url_;
			retry_mime     = last_load_mime_;
			retry_time     = last_load_time_;
			retry_dur      = last_load_duration_;
			}
		else if (cs.player_state == "PLAYING"
		      || cs.player_state == "BUFFERING"
		      || cs.player_state == "LOADING") {
			retry_pending_ = false;
			}
		}
	}
	status_cv_.notify_all();  // wake any SSE handlers waiting for the next push

	if (do_retry) {
		// Spawn a detached load_worker with the same gen we're already on.
		// load_worker will self-abort if a fresh user-driven load() has
		// bumped load_gen_ in the meantime, so an unwanted retry can never
		// race with a newer LOAD the user just clicked.
		int gen = load_gen_.load();
		std::cout << stamp() << "Cast: auto-retry LOAD (gen=" << gen
		          << ") — receiver went IDLE/ERROR after first attempt"
		          << std::endl;
		std::thread([this, retry_url, retry_mime, gen,
		             retry_time, retry_dur]{
			if (load_gen_.load() != gen) return;
			load_worker(retry_url, retry_mime, gen, retry_time, retry_dur);
			}).detach();
		}
	}

float CastManager::last_known_time() const
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return last_known_time_;
	}

CastManager::CastStatus CastManager::get_status() const
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return status_;
	}

CastManager::CastStatus CastManager::wait_status(int timeout_ms)
	{
	std::unique_lock<std::mutex> lk(status_mutex_);
	status_cv_.wait_for(lk, std::chrono::milliseconds(timeout_ms));
	return status_;
	}

void CastManager::send_media_cmd(const nlohmann::json& payload)
	{
	std::string tid;
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	tid = transport_id_;
	}
	if (tid.empty()) return;
	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		std::cout << stamp() << "Cast: connect failed for media command" << std::endl;
		return;
		}
	std::string src = "sender-0";
	cast_send(t.ssl, NS_CONN,  src, "receiver-0", {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_CONN,  src, tid,           {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_MEDIA, src, tid,            payload);
	}

void CastManager::cast_pause()
	{
	int msid = get_status().media_session_id;
	send_media_cmd({{"type", "PAUSE"}, {"requestId", 10}, {"mediaSessionId", msid}});
	std::cout << stamp() << "Cast: PAUSE sent" << std::endl;
	}

void CastManager::cast_play()
	{
	int msid = get_status().media_session_id;
	send_media_cmd({{"type", "PLAY"},  {"requestId", 11}, {"mediaSessionId", msid}});
	std::cout << stamp() << "Cast: PLAY sent" << std::endl;
	}

void CastManager::cast_seek(float seconds)
	{
	int msid = get_status().media_session_id;
	send_media_cmd({{"type", "SEEK"}, {"requestId", 12},
	                {"mediaSessionId", msid}, {"currentTime", seconds}});
	std::cout << stamp() << "Cast: SEEK → " << seconds << "s" << std::endl;
	}

void CastManager::poll_loop()
	{
	std::string connected_tid;  // transport_id_ we are currently subscribed to

	while (poll_active_) {
		std::string tid;
		{
		std::lock_guard<std::mutex> lk(tid_mutex_);
		tid = transport_id_;
		}

		if (tid.empty()) {
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
			continue;
			}

		Tls t;
		if (!tls_connect(t, device_.address, device_.port)) {
			std::cout << stamp() << "Cast: poll_loop connect failed, retrying" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			continue;
			}

		// Short receive timeout so we wake regularly to check poll_active_ and
		// whether load() has set a new transport_id_, and to send GET_STATUS.
		struct timeval tv = {1, 0};
		setsockopt(t.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

		std::string src = "sender-0";
		cast_send(t.ssl, NS_CONN, src, "receiver-0", {{"type", "CONNECT"}});
		cast_send(t.ssl, NS_CONN, src, tid,           {{"type", "CONNECT"}});
		// Ask for an immediate snapshot; subsequent updates arrive as pushes.
		cast_send(t.ssl, NS_MEDIA, src, tid,
		          {{"type", "GET_STATUS"}, {"requestId", 100}});
		connected_tid = tid;

		std::cout << stamp() << "Cast: poll_loop connected to transport " << tid << std::endl;

		while (poll_active_) {
			// If load() has started a new track, reconnect to the new transport.
			{
			std::lock_guard<std::mutex> lk(tid_mutex_);
			if (transport_id_ != connected_tid) break;
			}

			auto m = cast_recv(t.ssl);
			if (m.is_null()) {
				// EAGAIN means SO_RCVTIMEO fired with no data — loop to re-check
				// poll_active_ and transport_id_ before blocking again.
				if (errno == EAGAIN || errno == EWOULDBLOCK) {
					// Chromecast only pushes MEDIA_STATUS on state changes, not
					// during continuous playback — poll for position explicitly.
					cast_send(t.ssl, NS_MEDIA, src, tid,
					          {{"type", "GET_STATUS"}, {"requestId", 101}});
					continue;
					}
				break;  // real error or closed connection — reconnect
				}

			std::string type = m.value("type", "");
			if (type == "PING") {
				cast_send(t.ssl, NS_HEARTBEAT, src, "receiver-0",
				          {{"type", "PONG"}});
				}
			else if (type == "MEDIA_STATUS") {
				auto& list = m["status"];
				if (list.is_array() && !list.empty()) {
					auto& s = list[0];
					std::string idle_reason = s.value("idleReason", "");
					std::cout << stamp() << "Cast rx MEDIA_STATUS"
					          << " state="    << s.value("playerState", "?")
					          << " t="        << s.value("currentTime",  0.0f)
					          << " dur="      << (s.contains("media") && s["media"]["duration"].is_number()
					                              ? s["media"]["duration"].get<float>() : 0.0f)
					          << " msid="     << s.value("mediaSessionId", 0)
					          << (idle_reason.empty() ? std::string{}
					                                  : " idleReason=" + idle_reason)
					          << std::endl;
					// Dump the full payload for every push — fields like autoplay,
					// loadingItemId, extendedStatus, supportedMediaCommands and
					// preloadedItemId reveal what the receiver thinks it's doing
					// and are essential for diagnosing stuck-IDLE sessions.
					std::cout << stamp() << "Cast rx MEDIA_STATUS payload: "
					          << m.dump() << std::endl;
					}
				update_status(m);
				}
			else if (type == "ERROR") {
				std::cout << stamp() << "Cast rx ERROR: " << m.dump() << std::endl;
				}
			else if (type == "RECEIVER_STATUS") {
				// Dump the full payload so we can see whether the Default
				// Media Receiver app is still running and what transportId
				// it's advertising — STOP can trigger an idle-app teardown
				// that invalidates our cached transport_id.
				std::cout << stamp() << "Cast rx RECEIVER_STATUS: "
				          << m.dump() << std::endl;
				}
			else if (type != "PONG") {
				std::cout << stamp() << "Cast rx " << type << std::endl;
				}
			}
		}
	}

void CastManager::stop()
	{
	poll_active_ = false;
	active_ = false;
	token_.clear();
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	transport_id_.clear();
	}

	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	status_ = CastStatus{};
	}
	// Wake any SSE handlers blocked in wait_status() so they can detect
	// that cast is no longer active and close their response stream.
	status_cv_.notify_all();

	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		std::cout << stamp() << "Cast: connect failed for stop" << std::endl;
		return;
		}

	std::string src = "sender-0", dst = "receiver-0";
	cast_send(t.ssl, NS_CONN, src, dst, {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_RECV, src, dst, {{"type", "GET_STATUS"}, {"requestId", 1}});

	std::string session_id;
	for (int i = 0; i < 5 && session_id.empty(); i++) {
		auto m = cast_recv(t.ssl);
		if (m.is_null()) break;
		if (m.value("type", "") == "RECEIVER_STATUS") {
			auto& apps = m["status"]["applications"];
			if (apps.is_array() && !apps.empty())
				session_id = apps[0].value("sessionId", "");
			}
		}

	if (!session_id.empty())
		cast_send(t.ssl, NS_RECV, src, dst,
		          {{"type", "STOP"}, {"requestId", 2}, {"sessionId", session_id}});

	std::cout << stamp() << "Cast: stopped" << std::endl;
	}

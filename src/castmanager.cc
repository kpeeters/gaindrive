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

#include <sys/socket.h>
#include <sys/select.h>
#include <sys/time.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>

#include <openssl/ssl.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

static const char* NS_CONN  = "urn:x-cast:com.google.cast.tp.connection";
static const char* NS_RECV  = "urn:x-cast:com.google.cast.receiver";
static const char* NS_MEDIA = "urn:x-cast:com.google.cast.media";

// ---- mDNS discovery -----------------------------------------------

// State accumulated across mDNS response packets within one discover() call.
struct DiscState {
	std::map<std::string, CastManager::CastDevice> devs;   // service instance → device
	std::map<std::string, std::string>              hosts;  // hostname → IP address
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

	// Use the sender's IP directly — more reliable than chasing SRV → A record names.
	std::string src_ip;
	if (from && from->sa_family == AF_INET) {
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &reinterpret_cast<const struct sockaddr_in*>(from)->sin_addr,
		          ip, sizeof(ip));
		src_ip = ip;
		}

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
	else {
		std::cout << stamp() << "Cast mDNS: type=" << rtype
		          << " from=" << src_ip << " name=" << key << std::endl;
		}

	return 0;
	}

std::vector<CastManager::CastDevice> CastManager::discover(int timeout_ms)
	{
	DiscState state;

	struct sockaddr_in saddr = {};
	saddr.sin_family      = AF_INET;
	saddr.sin_addr.s_addr = INADDR_ANY;
	saddr.sin_port        = htons(MDNS_PORT);

	int sock = mdns_socket_open_ipv4(&saddr);
	if (sock < 0) {
		std::cout << stamp() << "Cast: failed to open mDNS socket (errno "
		          << errno << ")" << std::endl;
		return {};
		}
	std::cout << stamp() << "Cast: mDNS socket open, querying for "
	          << timeout_ms << " ms" << std::endl;

	std::vector<uint8_t> buf(4096);
	static const char svc[] = "_googlecast._tcp.local.";
	// mdns_query_send returns int; storing in uint16_t would corrupt -1 → 65535,
	// which would then cause mdns_query_recv to reject all ID-0 mDNS responses.
	int send_result = mdns_query_send(sock, MDNS_RECORDTYPE_PTR,
	                                   svc, strlen(svc),
	                                   buf.data(), buf.size(), 0);
	if (send_result < 0)
		std::cout << stamp() << "Cast: mDNS query send failed (errno "
		          << errno << ")" << std::endl;
	else
		std::cout << stamp() << "Cast: mDNS query sent" << std::endl;

	auto deadline = std::chrono::steady_clock::now()
	              + std::chrono::milliseconds(timeout_ms);

	while (std::chrono::steady_clock::now() < deadline) {
		auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		              deadline - std::chrono::steady_clock::now()).count();
		if (us <= 0) break;
		struct timeval tv = { (long)(us / 1000000), (long)(us % 1000000) };
		fd_set fds;
		FD_ZERO(&fds);
		FD_SET(sock, &fds);
		int r = select(sock + 1, &fds, nullptr, nullptr, &tv);
		if (r < 0 && errno == EINTR) continue;  // retry on signal interrupt
		if (r <= 0) break;
		// Pass 0 as only_query_id: mDNS responses always carry ID 0 per RFC 6762,
		// so filtering by our sent query ID would reject all valid responses.
		mdns_query_recv(sock, buf.data(), buf.size(), mdns_cb, &state, 0);
		}

	mdns_socket_close(sock);

	// Resolve each SRV target hostname to an IP address via collected A records.
	for (auto& [inst, dev] : state.devs) {
		if (!dev.address.empty()) continue;
		auto ti = state.srvs.find(inst);
		if (ti == state.srvs.end()) continue;
		auto hi = state.hosts.find(ti->second);
		if (hi != state.hosts.end()) dev.address = hi->second;
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

	t.sock = socket(AF_INET, SOCK_STREAM, 0);
	if (t.sock < 0) return false;

	struct timeval tv = {5, 0};  // 5-second timeout per operation
	setsockopt(t.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(t.sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

	struct sockaddr_in sa = {};
	sa.sin_family = AF_INET;
	sa.sin_port   = htons(port);
	if (inet_pton(AF_INET, addr.c_str(), &sa.sin_addr) != 1) return false;
	if (::connect(t.sock, (struct sockaddr*)&sa, sizeof(sa)) != 0) return false;

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

	std::cout << stamp() << "Cast: enabled → " << dev.name
	          << " (" << dev.address << ":" << dev.port << ")" << std::endl;
	return true;
	}

void CastManager::load(const std::string& url, const std::string& mime)
	{
	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		std::cout << stamp() << "Cast: connect failed ("
		          << device_.address << ":" << device_.port << ")" << std::endl;
		return;
		}

	std::string src = "sender-0", dst = "receiver-0";
	cast_send(t.ssl, NS_CONN, src, dst, {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_RECV, src, dst,
	          {{"type", "LAUNCH"}, {"appId", "CC1AD845"}, {"requestId", 1}});

	std::string tid = wait_transport(t.ssl);
	if (tid.empty()) {
		std::cout << stamp() << "Cast: no transportId from receiver" << std::endl;
		return;
		}

	cast_send(t.ssl, NS_CONN,  src, tid, {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_MEDIA, src, tid, {
		{"type",      "LOAD"},
		{"requestId", 2},
		{"media", {
			{"contentId",   url},
			{"contentType", mime},
			{"streamType",  "BUFFERED"}
			}}
		});

	// Connection closes here; the Chromecast fetches and plays independently.
	std::cout << stamp() << "Cast: → " << url << std::endl;
	}

void CastManager::stop()
	{
	active_ = false;
	token_.clear();

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

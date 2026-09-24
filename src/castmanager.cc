// Chromecast device discovery (mDNS) and playback control (Cast v2 over TLS).
//
// Cast protocol wire format is hand-encoded protobuf - the CastMessage schema
// has only six fields, so we avoid a full protobuf library dependency.

#define MDNS_IMPLEMENTATION
#include <mdns.h>

#include "castmanager.hh"
#include "stamp.hh"
#include "jsonread.hh"

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <iostream>
#include <sstream>
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

#include <algorithm>

#include <openssl/ssl.h>
#include <openssl/rand.h>

#include <nlohmann/json.hpp>

static const char* NS_CONN      = "urn:x-cast:com.google.cast.tp.connection";
static const char* NS_RECV      = "urn:x-cast:com.google.cast.receiver";
static const char* NS_MEDIA     = "urn:x-cast:com.google.cast.media";
static const char* NS_HEARTBEAT = "urn:x-cast:com.google.cast.tp.heartbeat";

// The Default Media Receiver: the only app we ever launch, and the only one
// whose transport a LOAD may be sent to.
static const char* MEDIA_APP_ID = "CC1AD845";

// ---- The deadlines around a LOAD ----------------------------------
//
// Each waits for something specific, and each was once shorter than the thing
// it waits for - which is the whole of the "casting a film does not start it
// on the first play" fault, fixed on the Android side first.
//
// Waiting longer costs nothing here: every wait ends the instant the message
// it wants arrives, nobody is blocked on the thread doing it, and a newer load
// supersedes an old one through load_gen_. These are backstops against a
// receiver that will never answer, not pacing.

// How long to wait for a RECEIVER_STATUS naming our app *before* launching it.
//
// Not about the launch at all: it decides whether an already running receiver
// is joined or torn down, since a LAUNCH against a running app recreates it.
static constexpr int STATUS_WAIT_MS = 4000;

// How long to wait for the receiver app after LAUNCH.
//
// **This is a television changing HDMI input and cold-starting a web app, not
// a network round trip** - measured at 10-20 s on the reference device. What
// this replaced was bounded by the socket's own 5 s timeout, so the set was
// still starting up when the LOAD was abandoned; the receiver then finished
// launching, published its transport, and sat on the Chromecast backdrop with
// a live control channel and nothing loaded. A second attempt always worked,
// because it found the transport already cached.
static constexpr int LAUNCH_WAIT_MS = 45000;

// How long a LOAD that *was* sent is given before it is sent once more.
//
// See load_worker(): the receiver that has published its transport but is not
// yet consuming the media namespace drops the LOAD on the floor, and there is
// no ack to wait on - a send reports only that the bytes left this machine.
static constexpr int LOAD_ACK_WAIT_MS = 8000;

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

// One local interface carrying an address of the family we want to query on.
struct IfaceV4 { std::string name; struct in_addr addr; };
struct IfaceV6 { std::string name; unsigned int  idx; };

// Interfaces worth sending a multicast query from. `want` restricts to one by
// name. IFF_MULTICAST matters: a point-to-point or tunnel interface that
// cannot carry multicast would only ever produce a socket that hears nothing.
static std::vector<IfaceV4> list_ifaces4(const std::string& want)
	{
	std::vector<IfaceV4> out;
	struct ifaddrs* iflist;
	if (getifaddrs(&iflist) < 0) return out;
	for (struct ifaddrs* ifa = iflist; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr) continue;
		if (ifa->ifa_addr->sa_family != AF_INET) continue;
		if (ifa->ifa_flags & IFF_LOOPBACK) continue;
		if (!(ifa->ifa_flags & IFF_UP)) continue;
		if (!(ifa->ifa_flags & IFF_MULTICAST)) continue;
		if (!want.empty() && want != ifa->ifa_name) continue;
		out.push_back({ifa->ifa_name,
		               reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr});
		}
	freeifaddrs(iflist);
	return out;
	}

static std::vector<IfaceV6> list_ifaces6(const std::string& want)
	{
	std::vector<IfaceV6> out;
	struct ifaddrs* iflist;
	if (getifaddrs(&iflist) < 0) return out;
	for (struct ifaddrs* ifa = iflist; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr) continue;
		if (ifa->ifa_addr->sa_family != AF_INET6) continue;
		if (ifa->ifa_flags & IFF_LOOPBACK) continue;
		if (!(ifa->ifa_flags & IFF_UP)) continue;
		if (!(ifa->ifa_flags & IFF_MULTICAST)) continue;
		if (!want.empty() && want != ifa->ifa_name) continue;
		unsigned int idx = if_nametoindex(ifa->ifa_name);
		if (!idx) continue;
		// An interface with several IPv6 addresses appears once per address;
		// the group join is by interface index, so one socket covers them all.
		bool dup = false;
		for (auto& e : out) if (e.idx == idx) dup = true;
		if (!dup) out.push_back({ifa->ifa_name, idx});
		}
	freeifaddrs(iflist);
	return out;
	}

// Open an IPv6 mDNS socket pointed at one interface. mdns_socket_setup_ipv6()
// joins ff02::fb with ipv6mr_interface 0, leaving the kernel to choose; but
// sending to a link-local multicast address needs an explicit scope, so set
// IPV6_MULTICAST_IF and re-join with the same index so send and receive agree.
static int open_ipv6_on(unsigned int ifidx)
	{
	int s = mdns_socket_open_ipv6(nullptr);
	if (s < 0) return -1;
	if (ifidx) {
		setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &ifidx, sizeof(ifidx));
		struct ipv6_mreq req = {};
		req.ipv6mr_multiaddr.s6_addr[0]  = 0xFF;
		req.ipv6mr_multiaddr.s6_addr[1]  = 0x02;
		req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
		req.ipv6mr_interface = ifidx;
		setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &req, sizeof(req));
		}
	return s;
	}

// A socket opened for one discovery pass. `iface` is the interface it *sends*
// on - a per-interface IPv4 socket still binds INADDR_ANY:5353 (mdns.h rewrites
// sin_addr after setting IP_MULTICAST_IF and the group membership), so every
// such socket also hears every interface's replies. The duplicates are free:
// DiscState is keyed on the service instance name.
struct DiscSocket {
	int         fd = -1;
	bool        v6 = false;
	std::string iface;
	};

// The service we query for and the service a record must belong to before it
// can become a device.  One constant for both, so the two cannot drift.
static const char kCastSvc[] = "_googlecast._tcp.local.";

// ASCII case fold.  DNS names are case-insensitive (RFC 4343) while a
// std::map key is not, so one device answering with two spellings of its
// instance name would otherwise be two devices.  ASCII-only is not a
// limitation - it is exactly what RFC 4343 specifies, and it leaves the UTF-8
// in a friendly name alone.
static std::string lc(std::string s)
	{
	for (char& c : s) c = (char)std::tolower(static_cast<unsigned char>(c));
	return s;
	}

// Does this (already case-folded) record owner name belong to a Cast service
// instance?  The dot-less form is accepted too: mdns_string_extract appends a
// dot after every label, but drops it if the name overruns its 256-byte
// buffer, and a device lost to a truncated name would be very hard to see.
static bool is_cast_instance(const std::string& name)
	{
	auto ends_with = [&](const std::string& suffix) {
		return name.size() > suffix.size() &&
		       name.compare(name.size() - suffix.size(),
		                    suffix.size(), suffix) == 0;
		};
	const std::string dotted = std::string(".") + kCastSvc;
	return ends_with(dotted) || ends_with(dotted.substr(0, dotted.size() - 1));
	}

// The instance label of a Cast service instance name - everything before
// "._googlecast._tcp.local.".  Used as the device's name when it sends no
// `fn`, which is what the Android client does (CastDiscovery.kt: `name =
// friendly ?: serviceName`), so a discovered device is never labelled by its
// IP address.  Takes the raw name so the device's own casing survives.
static std::string instance_label(const std::string& raw)
	{
	size_t dot = lc(raw).find(std::string(".") + kCastSvc);
	return dot == std::string::npos ? raw : raw.substr(0, dot);
	}

// State accumulated across mDNS response packets within one discover() call.
//
// Every key here is case-folded, and all four must be: the address join in
// discover() looks a `srvs` *value* up in `hosts` by *key*, so normalising one
// side alone would leave devices with no address at all.
struct DiscState {
	std::map<std::string, CastManager::CastDevice> devs;   // service instance → device
	std::map<std::string, std::string>              hosts;  // hostname → IPv4 address
	std::map<std::string, std::string>              hosts6; // hostname → IPv6 address (with scope)
	std::map<std::string, std::string>              srvs;   // service instance → SRV host
	// Case-folded instance name → the name as the device spelled it, so a
	// device that sent no `fn` can still be labelled in its own casing.
	std::map<std::string, std::string>              raws;
	std::string                                     iface;  // socket currently being drained
	bool                                            verbose = true;
	bool                                            cast_only = true;
	size_t                                          ignored = 0;  // records not _googlecast
	};

static int mdns_cb(int, const struct sockaddr* from, size_t,
                    mdns_entry_type_t, uint16_t,
                    uint16_t rtype, uint16_t, uint32_t ttl,
                    const void* data, size_t size,
                    size_t name_off, size_t,
                    size_t rec_off, size_t rec_len,
                    void* user_data)
	{
	auto* st = reinterpret_cast<DiscState*>(user_data);
	char buf1[256], buf2[256];

	size_t off = name_off;
	mdns_string_t rname = mdns_string_extract(data, size, &off, buf1, sizeof(buf1));
	std::string raw(rname.str, rname.length);
	std::string key = lc(raw);

	// Extract sender IP - used as address fallback and for AAAA scope IDs.
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

	// Every branch below logs one line, and a quiet pass wants none of them.
	std::ostringstream log;
	auto say = [&]{
		if (!st->verbose) return;
		std::cout << stamp() << "Cast mDNS: " << log.str()
		          << (st->iface.empty() ? "" : " via=" + st->iface) << std::endl;
		};

	// A record that is not part of a Cast service instance must not become a
	// device.  The socket is joined to the multicast group and mdns_query_recv
	// hands us every record of every packet on the wire, so without this any
	// service announcing a TXT `id` is assembled into a nameless "Chromecast"
	// - HomeKit's _hap._tcp being the one that shows up on every LAN.
	//
	// The reason is logged rather than the record dropped silently: these
	// per-record lines are how a device that is not appearing gets diagnosed,
	// and "N instance(s) seen" is about to become a far smaller number.
	bool assemble = !st->cast_only || is_cast_instance(key);
	// RFC 6762 §10.1: a TTL of zero is a goodbye, announcing that the device
	// is leaving.  Building a device out of one invents a phantom.
	bool goodbye  = ttl == 0;

	if (rtype == MDNS_RECORDTYPE_PTR) {
		log << "PTR  from=" << src_ip << " name=" << raw;
		say();
		}
	else if (rtype == MDNS_RECORDTYPE_TXT) {
		mdns_record_txt_t txt[32];
		size_t n = mdns_record_parse_txt(data, size, rec_off, rec_len, txt, 32);
		std::string id, fn, md, ca;
		for (size_t i = 0; i < n; i++) {
			std::string k(txt[i].key.str,   txt[i].key.length);
			std::string v(txt[i].value.str, txt[i].value.length);
			if (k == "id") id = v;
			if (k == "fn") fn = v;
			if (k == "md") md = v;
			if (k == "ca") ca = v;
			}
		// `ca` is a decimal bitmask; bit 0 is video_out.  Parsed by hand
		// rather than with std::stoi, which *throws* on a receiver that
		// announces something unexpected - this runs on the discovery
		// thread, where that is a dead server rather than an odd device.
		int caps = -1;
		if (!ca.empty()
		    && ca.find_first_not_of("0123456789") == std::string::npos) {
			try { caps = std::stoi(ca); } catch (...) { caps = -1; }
			}
		log << "TXT  from=" << src_ip
		    << " name=" << raw
		    << " id=" << id
		    << " fn=" << fn
		    << " md=" << md
		    << " ca=" << (ca.empty() ? "-" : ca);
		if (!assemble)    { st->ignored++; log << " (ignored: not _googlecast)"; }
		else if (goodbye) { log << " (ignored: goodbye)"; }
		else {
			auto& dev = st->devs[key];
			st->raws[key] = raw;
			if (!src_ip.empty() && dev.address.empty())
				dev.address = src_ip;
			if (!id.empty()) dev.id    = id;
			if (!fn.empty()) dev.name  = fn;
			if (!md.empty()) dev.model = md;
			// Field-wise like the rest: one spelling of an instance may carry
			// the record and another not, and -1 means "not announced" rather
			// than "announced as nothing".
			if (caps >= 0)   dev.capabilities = caps;
			}
		say();
		}
	else if (rtype == MDNS_RECORDTYPE_SRV) {
		mdns_record_srv_t srv = mdns_record_parse_srv(
			data, size, rec_off, rec_len, buf2, sizeof(buf2));
		std::string target(srv.name.str, srv.name.length);
		log << "SRV  from=" << src_ip
		    << " name=" << raw
		    << " target=" << target
		    << " port=" << srv.port;
		if (!assemble)    { st->ignored++; log << " (ignored: not _googlecast)"; }
		else if (goodbye) { log << " (ignored: goodbye)"; }
		else {
			auto& dev = st->devs[key];
			st->raws[key] = raw;
			dev.port = srv.port;
			if (!src_ip.empty() && dev.address.empty())
				dev.address = src_ip;
			st->srvs[key] = lc(target);
			}
		say();
		}
	else if (rtype == MDNS_RECORDTYPE_A) {
		struct sockaddr_in a4 = {};
		mdns_record_parse_a(data, size, rec_off, rec_len, &a4);
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip));
		st->hosts[key] = ip;
		log << "A    from=" << src_ip << " name=" << raw << " addr=" << ip;
		say();
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
		log << "AAAA from=" << src_ip << " name=" << raw << " addr=" << addr6;
		say();
		}
	else {
		log << "type=" << rtype << " from=" << src_ip << " name=" << raw;
		say();
		}

	return 0;
	}

std::vector<CastManager::CastDevice> CastManager::discover(const DiscoverOpts& opts)
	{
	DiscState state;
	state.verbose   = opts.verbose;
	state.cast_only = opts.cast_service_only;

	std::vector<DiscSocket> socks;

	if (opts.ipv4) {
		if (opts.per_interface) {
			// One socket per local address, which is mdns.h's intended
			// multi-NIC pattern: passing a real address makes it set
			// IP_MULTICAST_IF and join 224.0.0.251 on *that* interface,
			// where the INADDR_ANY case below joins only on whichever one
			// the kernel picks for the group.
			for (auto& e : list_ifaces4(opts.iface)) {
				struct sockaddr_in sa = {};
				sa.sin_family = AF_INET;
				sa.sin_addr   = e.addr;
				sa.sin_port   = htons(MDNS_PORT);
				int s = mdns_socket_open_ipv4(&sa);
				if (s < 0) {
					std::cout << stamp() << "Cast: IPv4 mDNS socket on " << e.name
					          << " failed (errno " << errno << ")" << std::endl;
					continue;
					}
				socks.push_back({s, false, e.name});
				}
			if (socks.empty())
				std::cout << stamp() << "Cast: no usable IPv4 interface"
				          << (opts.iface.empty() ? "" : " matching " + opts.iface)
				          << std::endl;
			}
		else {
			struct sockaddr_in sa = {};
			sa.sin_family      = AF_INET;
			sa.sin_addr.s_addr = INADDR_ANY;
			sa.sin_port        = htons(MDNS_PORT);
			int s = mdns_socket_open_ipv4(&sa);
			if (s < 0)
				std::cout << stamp() << "Cast: failed to open IPv4 mDNS socket (errno "
				          << errno << ")" << std::endl;
			else
				socks.push_back({s, false, ""});
			}
		}

	// IPv6 is required on many modern networks where devices only respond via
	// ff02::fb multicast.
	if (opts.ipv6) {
		if (opts.per_interface) {
			for (auto& e : list_ifaces6(opts.iface)) {
				int s = open_ipv6_on(e.idx);
				if (s < 0) {
					std::cout << stamp() << "Cast: IPv6 mDNS socket on " << e.name
					          << " failed (errno " << errno << ")" << std::endl;
					continue;
					}
				socks.push_back({s, true, e.name});
				}
			}
		else {
			unsigned int ifidx = find_ipv6_if();
			if (ifidx == 0)
				std::cout << stamp() << "Cast: no suitable IPv6 interface found" << std::endl;
			int s = open_ipv6_on(ifidx);
			if (s < 0) {
				std::cout << stamp() << "Cast: failed to open IPv6 mDNS socket (errno "
				          << errno << ")" << std::endl;
				}
			else {
				char nm[IF_NAMESIZE] = {};
				socks.push_back({s, true, ifidx && if_indextoname(ifidx, nm) ? nm : ""});
				}
			}
		}

	if (socks.empty())
		return {};

	std::cout << stamp() << "Cast: querying for " << opts.timeout_ms << " ms, "
	          << opts.queries << " query/queries on " << socks.size()
	          << " socket(s):";
	for (auto& s : socks)
		std::cout << " " << (s.v6 ? "IPv6" : "IPv4")
		          << (s.iface.empty() ? "" : "/" + s.iface);
	std::cout << std::endl;

	std::vector<uint8_t> sendbuf(2048);
	// RFC 6762 §17 caps an mDNS message at 9000 bytes. recvfrom truncates a
	// larger datagram silently, and mdns_query_recv then abandons the whole
	// packet at the first section that fails to parse - losing every device
	// described in it. Sizing to the protocol maximum removes the failure
	// mode rather than trying to detect it.
	std::vector<uint8_t> recvbuf(9000);

	auto now       = std::chrono::steady_clock::now();
	auto deadline  = now + std::chrono::milliseconds(opts.timeout_ms);
	auto next_send = now;
	int  sent      = 0;
	int  gap_ms    = opts.query_gap_ms;

	while (std::chrono::steady_clock::now() < deadline) {
		auto t = std::chrono::steady_clock::now();

		if (sent < opts.queries && t >= next_send) {
			for (auto& s : socks) {
				// mdns_query_send returns int; storing in uint16_t would corrupt
				// -1 → 65535, which would then cause mdns_query_recv to reject
				// all ID-0 mDNS responses.
				int r = mdns_query_send(s.fd, MDNS_RECORDTYPE_PTR,
				                        kCastSvc, strlen(kCastSvc),
				                        sendbuf.data(), sendbuf.size(), 0);
				std::cout << stamp() << "Cast: " << (s.v6 ? "IPv6" : "IPv4")
				          << (s.iface.empty() ? "" : "/" + s.iface)
				          << " query " << (sent + 1) << "/" << opts.queries
				          << " " << (r < 0 ? "failed" : "sent") << std::endl;
				}
			sent++;
			// RFC 6762 §5.2: a repeated query doubles its interval.
			next_send = t + std::chrono::milliseconds(gap_ms);
			gap_ms *= 2;
			}

		// Wake for the next send as well as for the deadline, or a repeat
		// would be deferred behind however long the network stays quiet.
		auto wake = deadline;
		if (sent < opts.queries && next_send < wake) wake = next_send;

		auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		              wake - std::chrono::steady_clock::now()).count();
		if (us < 0) us = 0;
		// Cast to the real member types rather than long: on Darwin tv_usec is
		// suseconds_t (int), so a long here narrows inside a braced initialiser,
		// which is ill-formed and rejected outright rather than warned about.
		struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };
		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = 0;
		for (auto& s : socks) { FD_SET(s.fd, &fds); maxfd = std::max(maxfd, s.fd); }
		int r = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
		if (r < 0) {
			if (errno == EINTR) continue;
			std::cout << stamp() << "Cast: select failed (errno " << errno
			          << ")" << std::endl;
			break;
			}
		// A quiet interval is not the end of the pass - with queries > 1 there
		// is another round still to send. This is why the old `break` here had
		// to go; it would have made every repeat unreachable.
		if (r == 0) continue;
		for (auto& s : socks) {
			if (!FD_ISSET(s.fd, &fds)) continue;
			state.iface = s.iface;
			// Pass 0 as only_query_id: mDNS responses always carry ID 0 per
			// RFC 6762.
			mdns_query_recv(s.fd, recvbuf.data(), recvbuf.size(),
			                mdns_cb, &state, 0);
			}
		}

	for (auto& s : socks) mdns_socket_close(s.fd);

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

	std::cout << stamp() << "Cast: discovery done - "
	          << state.devs.size() << " instance(s) seen, "
	          << state.ignored << " record(s) ignored" << std::endl;
	for (auto& [inst, dev] : state.devs)
		std::cout << stamp() << "  inst=" << inst
		          << " id=" << dev.id
		          << " fn=" << dev.name
		          << " md=" << dev.model
		          << " ca=" << (dev.capabilities < 0
		                        ? std::string("-")
		                        : std::to_string(dev.capabilities))
		          << " video=" << (dev.video_out() ? "yes" : "no")
		          << " addr=" << dev.address
		          << " port=" << dev.port << std::endl;

	// An instance with no address is not contactable whatever else it sent, so
	// that half of the filter is unconditional. The id comes from a TXT record,
	// and require_id=false is how we find out whether devices are arriving but
	// being dropped for want of one.
	// Each survivor carries its instance label alongside, as the name to fall
	// back to further down.
	std::vector<std::pair<CastDevice, std::string>> kept;
	for (auto& [inst, dev] : state.devs)
		if ((!opts.require_id || !dev.id.empty()) && !dev.address.empty()) {
			auto ri = state.raws.find(inst);
			kept.emplace_back(dev, instance_label(
				ri == state.raws.end() ? inst : ri->second));
			}

	// One row per physical device.
	//
	// The Cast id is the device's own identity, so it is the key; the address
	// is only a fallback for a pass run with require_id off. The port is in
	// both, because a Cast multizone group publishes its own instance at the
	// leader's *address* on a dynamic port, and collapsing the two would lose
	// the group.
	//
	// This runs after the filter above, not before: the filter is what
	// guarantees a non-empty address, without which two addressless entries
	// would meet under the same fallback key and merge two real devices.
	std::vector<CastDevice>  result;
	std::vector<std::string> labels;
	std::unordered_map<std::string, size_t> index;
	for (auto& [dev, label] : kept) {
		std::string k = (dev.id.empty() ? "addr:" + dev.address : "id:" + lc(dev.id))
		                + ":" + std::to_string(dev.port);
		auto it = index.find(k);
		if (it == index.end()) {
			index[k] = result.size();
			result.push_back(dev);
			labels.push_back(label);
			continue;
			}
		// Merge rather than pick: one spelling may carry the only `fn` and
		// another the only `md`, and dropping either loses something the
		// device did send.
		CastDevice& into = result[it->second];
		if (into.name.empty())  into.name  = dev.name;
		if (into.model.empty()) into.model = dev.model;
		if (into.capabilities < 0) into.capabilities = dev.capabilities;
		if (into.address.empty()) into.address = dev.address;
		// Prefer IPv4, as the address fallbacks above already do - an IPv6
		// link-local carries a scope suffix that nothing else here matches on.
		else if (into.address.find(':') != std::string::npos &&
		         dev.address.find(':') == std::string::npos)
			into.address = dev.address;
		std::cout << stamp() << "Cast: merged duplicate " << k
		          << " (" << into.name << ")" << std::endl;
		}

	// A device that sent no `fn` anywhere is named by its instance label rather
	// than left nameless, which is what the Android client does - the
	// alternative is a row a client can only label with an IP address. Last,
	// so that a real `fn` from any of the merged duplicates wins over it.
	for (size_t i = 0; i < result.size(); i++)
		if (result[i].name.empty())
			result[i].name = labels[i];

	return result;
	}

std::vector<CastManager::CastDevice> CastManager::discover(int timeout_ms)
	{
	DiscoverOpts opts;
	opts.timeout_ms = timeout_ms;
	return discover(opts);
	}

void CastManager::discover_background(int timeout_ms)
	{
	std::thread([this, timeout_ms]{
		auto devs = discover(timeout_ms);
		std::lock_guard<std::mutex> lk(cache_mutex_);
		devices_cache_ = std::move(devs);
		}).detach();
	}

std::string CastManager::manual_id(const std::string& address, int port)
	{
	return "manual:" + address + ":" + std::to_string(port);
	}

void CastManager::set_manual_devices(std::vector<CastDevice> devices)
	{
	std::lock_guard<std::mutex> lk(manual_mutex_);
	manual_devices_ = std::move(devices);
	}

std::vector<CastManager::CastDevice> CastManager::cached_devices() const
	{
	std::vector<CastDevice> result;
		{
		std::lock_guard<std::mutex> lk(cache_mutex_);
		result = devices_cache_;
		}

	std::lock_guard<std::mutex> lk(manual_mutex_);
	for (const auto& m : manual_devices_) {
		// Deduplicated on the address and port, the only fields the two kinds
		// have in common - a configured device has no Cast id to match on.
		// The port is part of it because a Cast group lives at its leader's
		// address on a different one, and on the address alone a group in
		// range would suppress a configured entry for the leader itself.
		bool found = false;
		for (const auto& d : result)
			if (d.address == m.address && d.port == m.port) { found = true; break; }
		if (!found) result.push_back(m);
		}
	return result;
	}

// ---- Protobuf helpers (CastMessage encoding / decoding) -----------
//
// CastMessage schema (cast_channel.proto):
//   field 1 varint  - protocol_version (0 = CASTV2_1_0)
//   field 2 string  - source_id
//   field 3 string  - destination_id
//   field 4 string  - namespace
//   field 5 varint  - payload_type (0 = STRING)
//   field 6 string  - payload_utf8

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

// Why a receive produced no message, for a caller that has to tell "nothing
// yet" from "there will never be anything".
//
// The distinction only became load-bearing with cast_wait_for(): a loop that
// waits up to forty-five seconds must keep waiting through a socket timeout,
// must keep waiting through a frame it cannot read as JSON, and must *not*
// keep waiting on a closed connection - which would spin for the rest of the
// deadline, since a clean EOF sets no errno to notice it by.
enum class Recv { Message, Timeout, Closed, Unusable };

static nlohmann::json cast_recv(SSL* ssl, Recv* why = nullptr)
	{
	auto fail = [&](Recv r) { if (why) *why = r; return nlohmann::json(nullptr); };
	// A read that ended for no reason the socket reports is a closed one, not a
	// timeout: SSL_read answers 0 on a clean shutdown and leaves errno alone.
	auto read_failed = [] {
		return (errno == EAGAIN || errno == EWOULDBLOCK) ? Recv::Timeout
		                                                 : Recv::Closed;
		};
	if (why) *why = Recv::Message;

	uint8_t fr[4]; size_t got = 0;
	while (got < 4) {
		errno = 0;
		int r = SSL_read(ssl, fr + got, (int)(4 - got));
		if (r <= 0) return fail(read_failed());
		got += (size_t)r;
		}
	uint32_t len = ((uint32_t)fr[0] << 24) | ((uint32_t)fr[1] << 16)
	             | ((uint32_t)fr[2] <<  8) |  (uint32_t)fr[3];
	// A length we cannot believe leaves the stream unresynchronisable, so the
	// connection is finished rather than merely this message.
	if (len == 0 || len > (1u << 20)) return fail(Recv::Closed);
	std::vector<uint8_t> msg(len);
	got = 0;
	while (got < len) {
		errno = 0;
		int r = SSL_read(ssl, msg.data() + got, (int)(len - got));
		if (r <= 0) return fail(read_failed());
		got += (size_t)r;
		}
	std::string payload = pb_payload(msg);
	// A whole message that says nothing we can read - a binary payload, say.
	// The framing is intact, so the next one may well be fine.
	if (payload.empty()) return fail(Recv::Unusable);
	// Null for anything unusable, because that is what every caller already
	// tests. A failed parse yields a *discarded* value, and `is_null()` is
	// false for one - so returning it directly would send a body we could not
	// read past the "did we receive a message" check and into a value() call
	// that throws, on the poll thread, where an exception is std::terminate.
	auto j = nlohmann::json::parse(payload, nullptr, false);
	if (!j.is_object()) return fail(Recv::Unusable);
	return j;
	}

// A deadline this many milliseconds from now.
static std::chrono::steady_clock::time_point in_ms(int ms)
	{
	return std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
	}

// Receive until a message the caller wants arrives, or the deadline passes.
//
// Three things it does that the bare cast_recv() loops it replaces did not,
// and each of them was a fault in at least one of those loops:
//
//  * **It answers PING.** The platform drops a sender that stops answering its
//    heartbeat, so any wait of more than a few seconds has to PONG or the
//    connection dies underneath it. That is what makes a 45 s wait for a
//    launching television possible at all.
//  * **The deadline is wall clock**, not a count of reads. cast_recv() returns
//    null when the socket's own SO_RCVTIMEO fires, and reading that as failure
//    is what capped every one of these waits at five seconds however long they
//    asked for. Only Recv::Closed ends the wait - see there for why that has to
//    be cast_recv()'s answer rather than this loop's guess at errno.
//  * **It can be cancelled**, so a load the user has already replaced does not
//    sit here for the rest of the deadline.
//
// The receive timeout is lowered to 1 s for the duration, which is the
// granularity the deadline and the cancellation are then checked at; these are
// all throwaway connections, so nothing else cares.
static nlohmann::json cast_wait_for(
		Tls& t, const std::string& src,
		std::chrono::steady_clock::time_point deadline,
		const std::function<bool(const nlohmann::json&)>& want,
		const std::function<bool()>& cancelled = {})
	{
	struct timeval tv = {1, 0};
	setsockopt(t.sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

	while (std::chrono::steady_clock::now() < deadline) {
		if (cancelled && cancelled()) return nullptr;
		Recv why = Recv::Message;
		auto m = cast_recv(t.ssl, &why);
		if (m.is_null()) {
			if (why == Recv::Closed) return nullptr;
			continue;   // nothing yet, or nothing we can read
			}
		if (jstr(m, "type") == "PING") {
			cast_send(t.ssl, NS_HEARTBEAT, src, "receiver-0", {{"type", "PONG"}});
			continue;
			}
		if (want(m)) return m;
		}
	return nullptr;
	}

// The entry for one app in a RECEIVER_STATUS, matched on appId, or a null.
//
// **Never applications[0]**, which is a fix rather than a refinement: an idle
// television runs its own ambient app (E8C28D3C, Backdrop), which publishes a
// transportId like any other and ignores the media namespace - so a LOAD sent
// there produces no MEDIA_STATUS, no fetch, no error and nothing in any log.
static const nlohmann::json& running_app(const nlohmann::json& msg,
                                          const char* app_id)
	{
	static const nlohmann::json none;
	const auto& apps = jsub(jsub(msg, "status"), "applications");
	if (!apps.is_array()) return none;
	for (const auto& a : apps)
		if (jstr(a, "appId") == app_id) return a;
	return none;
	}

// Whether this RECEIVER_STATUS says what the receiver is running at all.
//
// The distinction running_app() cannot express: it yields nothing both for
// "our app is not running" and for "this status was not about applications".
// A volume-change push is the second - it carries `volume` and no applications
// array - and reading it as the first discards a transport that is still
// perfectly good, which costs the *next* load the whole GET_STATUS-and-LAUNCH
// path this file's deadlines exist for.
static bool lists_applications(const nlohmann::json& msg)
	{
	return jsub(jsub(msg, "status"), "applications").is_array();
	}

// ---- CastManager public methods -----------------------------------

const char* CastManager::probe_text(Probe result)
	{
	switch (result) {
		case Probe::ANSWERED:    return "answered";
		case Probe::SILENT:      return "connected but did not speak Cast";
		case Probe::UNREACHABLE: return "unreachable";
		}
	return "unknown";
	}

CastManager::Probe CastManager::probe(const CastDevice& dev, int timeout_ms)
	{
	Tls t;
	if (!tls_connect(t, dev.address, dev.port))
		return Probe::UNREACHABLE;

	const std::string src = "sender-0";
	// The virtual connection to the platform has to exist before anything else
	// is accepted; this is the same opening pair poll_loop() sends.
	if (!cast_send(t.ssl, NS_CONN, src, "receiver-0", {{"type", "CONNECT"}}))
		return Probe::UNREACHABLE;
	cast_send(t.ssl, NS_RECV, src, "receiver-0",
	          {{"type", "GET_STATUS"}, {"requestId", 1}});

	// Through cast_wait_for(), which is what makes timeout_ms mean what it
	// says: this loop used to give up on the first read that returned nothing,
	// so it could never wait past the socket's own 5 s however long it was
	// asked for. Answering PING is that helper's job too.
	auto m = cast_wait_for(t, src,
	                       std::chrono::steady_clock::now()
	                       + std::chrono::milliseconds(timeout_ms),
	                       [](const nlohmann::json& j) {
	                           return jstr(j, "type") == "RECEIVER_STATUS";
	                           });
	if (!m.is_null()) {
		std::cout << stamp() << "Cast: probe " << dev.address << ":" << dev.port
		          << " answered: " << m.dump() << std::endl;
		return Probe::ANSWERED;
		}

	return Probe::SILENT;
	}

// How long a minted stream token stays usable. Generous, because it has to
// outlast the film it was minted for, and a seek mints a fresh one anyway -
// this is a backstop for a session someone walked away from, not a timeout the
// receiver will ever notice.
static constexpr auto CAST_TOKEN_TTL = std::chrono::hours(12);

std::string CastManager::mint_token(int song_id,
                                     const std::vector<int>& caption_ids)
	{
	// 128 bits from the CSPRNG. **The return value is checked**: RAND_bytes
	// answers 0 on failure, and ignoring that leaves `bytes` holding whatever
	// was on the stack - a token an attacker may well be able to predict, for
	// a credential that skips authentication entirely.
	uint8_t bytes[16];
	if (RAND_bytes(bytes, sizeof(bytes)) != 1) {
		std::cout << stamp() << "Cast: RAND_bytes failed; refusing to mint a "
		          << "stream token" << std::endl;
		std::lock_guard<std::mutex> lk(token_mutex_);
		token_.clear();
		return {};
		}
	char hex[33];
	for (int i = 0; i < 16; i++) snprintf(hex + 2*i, 3, "%02x", bytes[i]);

	std::lock_guard<std::mutex> lk(token_mutex_);
	token_             = hex;
	token_song_id_     = song_id;
	token_caption_ids_ = caption_ids;
	token_expires_     = std::chrono::steady_clock::now() + CAST_TOKEN_TTL;
	return token_;
	}

std::string CastManager::token() const
	{
	std::lock_guard<std::mutex> lk(token_mutex_);
	return token_;
	}

// Constant-time over the token, which is not really about timing - 128 bits
// makes that academic - but about not having the wrong primitive sitting in
// the one place that decides whether an unauthenticated request is served.
static bool token_eq(const std::string& a, const std::string& b)
	{
	if (a.size() != b.size()) return false;
	unsigned diff = 0;
	for (size_t i = 0; i < a.size(); ++i)
		diff |= static_cast<unsigned char>(a[i]) ^ static_cast<unsigned char>(b[i]);
	return diff == 0;
	}

bool CastManager::valid_token(const std::string& t, int song_id) const
	{
	if (!active_) return false;
	std::lock_guard<std::mutex> lk(token_mutex_);
	if (token_.empty() || !token_eq(token_, t)) return false;
	if (std::chrono::steady_clock::now() > token_expires_) return false;
	return song_id == token_song_id_;
	}

bool CastManager::valid_caption_token(const std::string& t, int song_id,
                                       int caption_id) const
	{
	if (!valid_token(t, song_id)) return false;
	// Only the captions this LOAD actually declared. getVideoInfo and
	// getCaptions must agree about what exists, and so must this: a track the
	// LOAD never offered is one the receiver has no reason to ask for.
	std::lock_guard<std::mutex> lk(token_mutex_);
	return std::find(token_caption_ids_.begin(), token_caption_ids_.end(),
	                 caption_id) != token_caption_ids_.end();
	}

bool CastManager::start(const CastDevice& dev)
	{
	device_ = dev;
	active_ = true;

	// No token is minted here. It used to be, once per session and bound to
	// nothing; it is now minted per LOAD by cast_load_song(), which is what
	// scopes it to a single song. Until the first LOAD there is nothing for a
	// receiver to fetch, so there is nothing to authorise.
	{
	std::lock_guard<std::mutex> lk(token_mutex_);
	token_.clear();
	token_song_id_ = -1;
	token_caption_ids_.clear();
	}

	// Start the background status polling thread.
	poll_active_ = true;
	std::thread([this]{ poll_loop(); }).detach();

	std::cout << stamp() << "Cast: enabled → " << dev.name
	          << " (" << dev.address << ":" << dev.port << ")" << std::endl;
	return true;
	}

nlohmann::json CastManager::build_load(const LoadRequest& req, int request_id)
	{
	nlohmann::json media = {
		{"contentId",   req.url},
		{"contentType", req.mime},
		{"streamType",  "BUFFERED"},
		{"duration",    req.duration}
		};
	// Omitted rather than sent empty when there are none: an empty tracks array
	// is legal but says "this medium has no subtitles", and some receiver
	// versions take the trouble to render a disabled CC control for it.
	if (!req.tracks.empty()) {
		media["tracks"] = req.tracks;
		// Defensive, in the same spirit as `autoplay` below: the receiver has a
		// default style and the spec does not require this, but a style that
		// resolves to transparent-on-transparent renders cues that are present
		// and invisible, which is indistinguishable from cues that never
		// arrived.  White on semi-opaque black is what every player defaults to
		// anyway, and stating it costs nothing.
		media["textTrackStyle"] = {
			{"backgroundColor", "#00000080"},
			{"foregroundColor", "#FFFFFFFF"},
			{"edgeType",        "OUTLINE"},
			{"edgeColor",       "#000000FF"},
			{"fontScale",       1.0}
			};
		}

	nlohmann::json msg = {
		{"type",      "LOAD"},
		{"requestId", request_id},
		{"autoplay",  true},
		{"media",     media}
		};
	// currentTime tells the receiver to start playback at this offset
	// within the loaded media - the receiver handles the seek itself
	// using the file's XING/seek tables.  This avoids server-side
	// transcoding and gives the receiver real duration metadata.
	if (req.current_time > 0.0f) msg["currentTime"] = req.current_time;
	if (!req.active_track_ids.empty())
		msg["activeTrackIds"] = req.active_track_ids;
	return msg;
	}

void CastManager::load(const LoadRequest& request)
	{
	const std::string& url = request.url;
	// Signal any running content-provider thread to stop immediately.
	int gen = ++load_gen_;
	std::cout << stamp() << "Cast: load gen=" << gen << " url=" << url
	          << " currentTime=" << request.current_time << std::endl;

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
	status_.duration    = static_cast<float>(request.duration);
	last_load_          = request;
	retry_pending_      = true;
	// A new attempt supersedes whatever the last one had to say. The sequence
	// number is deliberately not bumped: a client shows a notice when the
	// sequence changes *and* the text is non-empty, so clearing the text is
	// enough to stop a page that reloads now being told about a load that has
	// since been replaced.
	notice_.clear();
	}

	// All TLS/blocking work happens in a detached thread so that the
	// castLoad.view handler (and the httplib thread it runs on) returns
	// immediately.  load_gen_ acts as a cancellation token: if a newer
	// load() fires before this worker reaches a blocking call, the worker
	// detects the stale generation and exits without doing any work.
	std::thread([this, request, gen]{
		load_worker(request, gen);
		}).detach();
	}

std::string CastManager::ensure_transport(Tls& t, const std::string& src,
                                          int gen)
	{
	// Every wait here ends the moment its message arrives, and ends early if
	// the user has picked something else in the meantime.
	auto cancelled = [this, gen] { return load_gen_.load() != gen; };
	auto ours = [](const nlohmann::json& j) {
		return jstr(j, "type") == "RECEIVER_STATUS"
		    && !jstr(running_app(j, MEDIA_APP_ID), "transportId").empty();
		};

	cast_send(t.ssl, NS_CONN, src, "receiver-0", {{"type", "CONNECT"}});

	// Ask before launching. A LAUNCH against a running app *recreates* it, so a
	// receiver already showing ours would pay the whole cold start again - ten
	// to twenty seconds of black on a television - and the media session it was
	// holding would go with it.
	//
	// Waited for by requestId, which the receiver echoes on a reply and sets to
	// 0 on a broadcast. That is what makes this step cost nothing when the app
	// is *not* running: the reply is the answer either way, so a receiver that
	// is idle or showing something else is launched at once rather than after
	// the full STATUS_WAIT_MS. Matching on "any RECEIVER_STATUS" instead would
	// be satisfied by a volume push, which says nothing about applications and
	// would send a LAUNCH into a running app - the one thing this step exists
	// to avoid.
	const int status_req = next_request_id();
	cast_send(t.ssl, NS_RECV, src, "receiver-0",
	          {{"type", "GET_STATUS"}, {"requestId", status_req}});
	auto m = cast_wait_for(t, src, in_ms(STATUS_WAIT_MS),
	                       [status_req](const nlohmann::json& j) {
	                           return jstr(j, "type") == "RECEIVER_STATUS"
	                               && jint(j, "requestId") == status_req;
	                           },
	                       cancelled);
	if (cancelled()) return {};
	std::string running = jstr(running_app(m, MEDIA_APP_ID), "transportId");
	if (!running.empty()) {
		std::cout << stamp() << "Cast: joined the running receiver, transport "
		          << running << std::endl;
		return running;
		}

	cast_send(t.ssl, NS_RECV, src, "receiver-0",
	          {{"type", "LAUNCH"}, {"appId", MEDIA_APP_ID},
	           {"requestId", next_request_id()}});
	// The transport may arrive on the LAUNCH's own reply or on a broadcast that
	// follows it, so this one is matched on content rather than on requestId.
	m = cast_wait_for(t, src, in_ms(LAUNCH_WAIT_MS), ours, cancelled);
	if (m.is_null()) return {};

	std::string tid = jstr(running_app(m, MEDIA_APP_ID), "transportId");
	std::cout << stamp() << "Cast: receiver launched, transport " << tid
	          << std::endl;
	return tid;
	}

int CastManager::send_load_once(const LoadRequest& req, int gen,
                                 bool* used_cached_transport)
	{
	const std::string src = "sender-0";
	if (load_gen_.load() != gen) return 0;

	std::string tid;
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	tid = transport_id_;
	}
	if (used_cached_transport) *used_cached_transport = !tid.empty();

	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		// A device that will not accept a connection is not holding a transport
		// worth keeping either; the next LOAD resolves a fresh one.
		{
		std::lock_guard<std::mutex> lk(tid_mutex_);
		transport_id_.clear();
		}
		std::cout << stamp() << "Cast: connect failed ("
		          << device_.address << ":" << device_.port << ")" << std::endl;
		note_load_abandoned("The receiver could not be reached, so nothing was "
		                    "sent to it.");
		return 0;
		}

	if (tid.empty()) {
		tid = ensure_transport(t, src, gen);
		if (load_gen_.load() != gen) return 0;
		if (tid.empty()) {
			// Said out loud and reported, because this was the invisible
			// abandon: the receiver finished launching seconds later, published
			// its transport, and sat on the Chromecast backdrop with a live
			// control channel and nothing loaded.
			std::cout << stamp() << "Cast: no transportId from receiver"
			          << std::endl;
			note_load_abandoned("The television did not finish starting up, so "
			                    "nothing was sent to it. Try again.");
			return 0;
			}
		std::lock_guard<std::mutex> lk(tid_mutex_);
		transport_id_ = tid;
		}
	else {
		// The app is already running, but the virtual connection to the
		// platform still has to exist on this fresh socket before anything else
		// is accepted. ensure_transport() sends it on the other branch.
		cast_send(t.ssl, NS_CONN, src, "receiver-0", {{"type", "CONNECT"}});
		}

	if (load_gen_.load() != gen) return 0;

	cast_send(t.ssl, NS_CONN, src, tid, {{"type", "CONNECT"}});
	// A fresh requestId every time, which matters for the re-send above all:
	// a receiver correlates its replies by it, so repeating one it has already
	// seen is a worse thing to do than sending nothing.
	nlohmann::json msg = build_load(req, next_request_id());
	std::cout << stamp() << "Cast: LOAD " << msg.dump() << std::endl;
	if (!cast_send(t.ssl, NS_MEDIA, src, tid, msg)) {
		std::cout << stamp() << "Cast: LOAD could not be written" << std::endl;
		note_load_abandoned("The connection to the receiver was lost before the "
		                    "track was sent.");
		return 0;
		}
	// Returned rather than read back by the caller, so that a degrade retry
	// firing microseconds after this send cannot hand its own attempt number to
	// the watcher this one is about to arm.
	const int attempt = ++load_attempt_;

	// poll_loop() receives the MEDIA_STATUS response and updates status_. There
	// is nothing to read here - but there is something to wait for; see
	// await_load_ack().
	std::cout << stamp() << "Cast: → " << req.url << std::endl;
	return attempt;
	}

bool CastManager::await_load_ack(int gen, int attempt)
	{
	// On the condition variable every status push already notifies, rather than
	// sleeping the window out: a healthy receiver answers in well under a
	// second, and there is no reason for the worker thread to outlive it by
	// eight. wait_for returns the predicate's value at the deadline, which is
	// the answer wanted.
	std::unique_lock<std::mutex> lk(status_mutex_);
	return status_cv_.wait_for(
		lk, std::chrono::milliseconds(LOAD_ACK_WAIT_MS),
		[this, gen, attempt] {
			// The last is the acknowledgement. The three before it are not an
			// unanswered LOAD either: the session is gone, the user has chosen
			// something else, or a degrade retry fired by the failure path owns
			// the session now.
			return !active_
			    || load_gen_.load() != gen
			    || load_attempt_.load() != attempt
			    || !retry_pending_;
			});
	}

void CastManager::load_worker(LoadRequest req, int gen)
	{
	const std::string& url = req.url;

	// Make the URL answerable before anyone is told to fetch it.  A receiver
	// gives up after about a minute of silence on the HTTP body, and a film's
	// soundtrack takes longer than that to transcode, so the wait has to
	// happen here - with nothing yet loaded - rather than on the socket.
	//
	// This thread is detached: an exception escaping it is std::terminate, a
	// dead server rather than a failed load.
	if (req.prepare) {
		if (load_gen_.load() != gen) return;
		bool ready = false;
		try { ready = req.prepare(); }
		catch (const std::exception& e) {
			std::cout << stamp() << "Cast: prepare threw: " << e.what()
			          << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "Cast: prepare threw" << std::endl;
			}
		if (!ready) {
			std::cout << stamp() << "Cast: prepare failed, not loading " << url
			          << std::endl;
			return;
			}
		// A prepare that took minutes is exactly when the user has moved on.
		if (load_gen_.load() != gen) return;
		}

	bool cached  = false;
	int  attempt = send_load_once(req, gen, &cached);
	if (!attempt) return;
	if (await_load_ack(gen, attempt)) return;

	// A *cached* transport that answered nothing is the one most likely to be
	// dead - the receiver tore the app down and nothing told us - so throw it
	// away and let the second attempt resolve a fresh one. One we had just
	// resolved is reused as it is.
	if (cached) {
		std::lock_guard<std::mutex> lk(tid_mutex_);
		transport_id_.clear();
		}
	std::cout << stamp() << "Cast: no answer " << LOAD_ACK_WAIT_MS
	          << "ms after LOAD; sending it once more" << std::endl;
	attempt = send_load_once(req, gen, nullptr);
	if (!attempt) return;

	// Once, never a loop. What this recovers from is a receiver that was a
	// moment too early; if the second is ignored too then something else is
	// wrong and repeating would bury it. The wait still has to happen, because
	// retry_pending_ must be resolved either way - a flag left armed is
	// consumed by an unrelated status minutes later and re-LOADs whatever is
	// playing then.
	if (await_load_ack(gen, attempt)) return;
	note_load_abandoned("The receiver did not answer, so playback did not "
	                    "start.");
	}

void CastManager::note_load_abandoned(const char* what)
	{
	// **Never degrade_load().** Every rung of that ladder answers "the receiver
	// refused this" - drop the subtitle tracks, become the soundtrack - and
	// none of them answers "the receiver was never told". The flag still has to
	// be cleared, or it survives to be consumed by an unrelated status minutes
	// later and re-LOADs whatever is playing then.
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	retry_pending_ = false;
	// Set *before* the status below, because publishing that status is what
	// wakes every SSE listener: a notice written afterwards would miss the very
	// push it belongs to and arrive on the next one, up to fifteen seconds
	// later.
	notice_      = what;
	notice_seq_ += 1;
	}
	std::cout << stamp() << "Cast: LOAD abandoned - " << what << std::endl;

	// The status the receiver never sent, published the way note_load_failure()
	// publishes the one a refused LOAD never sends. Without it nothing on the
	// SSE ever says this attempt is over, and the client sits on "Preparing…"
	// for ever.
	update_status({
		{"status", nlohmann::json::array({
			nlohmann::json{
				{"playerState",    "IDLE"},
				{"idleReason",     "ERROR"},
				{"mediaSessionId", 0}
				}
			})}
		});
	}

CastManager::Notice CastManager::notice() const
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return Notice{ notice_, notice_seq_ };
	}

int CastManager::next_request_id()
	{
	return request_id_++;
	}

void CastManager::update_status(const nlohmann::json& msg)
	{
	const auto& s = jidx(jsub(msg, "status"), 0);
	if (!s.is_object()) return;
	CastStatus cs;
	std::string state   = jstr(s, "playerState");
	cs.player_state     = state.empty() ? "IDLE" : state;
	cs.current_time     = static_cast<float>(jnum(s, "currentTime"));
	cs.media_session_id = jint(s, "mediaSessionId");
	cs.idle_reason      = jstr(s, "idleReason");
	// Left at 0 when the receiver does not report one, which the caller below
	// reads as "keep the duration we already had".
	cs.duration         = static_cast<float>(jnum(jsub(s, "media"), "duration"));

	bool        do_retry  = false;
	const char* retry_how = nullptr;
	LoadRequest retry_req;
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	if (cs.player_state != "IDLE")
		last_known_time_ = cs.current_time;
	if (cs.duration == 0.0f)
		cs.duration = status_.duration;
	status_ = cs;

	// Auto-retry decision: only consider pushes for the NEW media session
	// (msid != last_load_old_msid_).  Otherwise a stale PLAYING update for
	// the old session - typically poll_loop's GET_STATUS reply that
	// crosses our LOAD on the wire - would clear the flag prematurely and
	// the real IDLE/ERROR for the new msid would arrive too late to
	// trigger a retry.  IDLE/INTERRUPTED also carries the old msid, so it
	// is skipped here as well; that's fine - the next push (the new msid
	// going to PLAYING or ERROR) is the one we actually need.
	if (retry_pending_ && cs.media_session_id != last_load_old_msid_) {
		if (cs.player_state == "IDLE" && cs.idle_reason == "ERROR") {
			retry_pending_ = false;
			retry_req      = last_load_;
			// Degrade rather than repeat: replaying a LOAD the receiver has
			// already rejected only fails again.  Which rung, and why each
			// exists, is in degrade_load().
			retry_how      = degrade_load(retry_req);
			do_retry       = retry_how != nullptr;
			// Re-armed, and it has to be: without this only the first rung is
			// ever observed, because spawn_retry() goes straight to
			// load_worker() rather than back through load().  Bounded by the
			// ladder running out, and cleared below the moment the receiver
			// reports it is playing - which is what the note about a stale
			// flag being consumed by an unrelated status minutes later relies
			// on, and why it is set only when there is a further rung.
			if (do_retry) {
				retry_pending_ = true;
				last_load_     = retry_req;
				}
			}
		else if (cs.player_state == "PLAYING"
		      || cs.player_state == "BUFFERING"
		      || cs.player_state == "LOADING") {
			retry_pending_ = false;
			}
		}
	else if (retry_pending_ && cs.player_state == "IDLE"
	      && cs.idle_reason == "ERROR") {
		// An IDLE/ERROR carrying the *old* msid - both are 0 on a receiver
		// with no session yet, which is the ordinary shape of a first LOAD
		// failing outright.  No retry, because this cannot be told apart
		// from a stale push about the session being replaced; but the flag
		// must be cleared, or it survives to be consumed by an unrelated
		// status minutes later and re-LOADs whatever is playing then.
		retry_pending_ = false;
		}
	}
	status_cv_.notify_all();  // wake any SSE handlers waiting for the next push

	if (do_retry)
		spawn_retry(retry_req, "receiver went IDLE/ERROR after the last attempt",
		            retry_how);
	}

const char* CastManager::degrade_load(LoadRequest& req)
	{
	if (!req.tracks.empty()) {
		req.tracks = nlohmann::json::array();
		req.active_track_ids.clear();
		return "retrying without subtitle tracks";
		}
	if (req.fallback) {
		// A reference-counted copy of the pointer first.  `req = *req.fallback`
		// would destroy the pointee - through the member being overwritten -
		// partway through reading it.  The fallback's own `fallback`, usually
		// null, is what bounds the ladder.
		auto next = req.fallback;
		req = *next;
		return "retrying with the caller's fallback stream";
		}
	return nullptr;
	}

void CastManager::spawn_retry(const LoadRequest& req, const char* why,
                              const char* how)
	{
	// The same gen we're already on.  load_worker self-aborts if a fresh
	// user-driven load() has bumped load_gen_ in the meantime, so an unwanted
	// retry can never race with a newer LOAD the user just clicked.
	int gen = load_gen_.load();
	std::cout << stamp() << "Cast: auto-retry LOAD (gen=" << gen << ") - "
	          << why << " (" << how << ")" << std::endl;
	std::thread([this, req, gen]{
		if (load_gen_.load() != gen) return;
		load_worker(req, gen);
		}).detach();
	}

void CastManager::note_load_failure(int media_session_id)
	{
	// The retry is claimed *before* the status is published, and deliberately
	// without update_status's media-session filter.  That filter exists to
	// ignore stale pushes about the session being replaced; an ERROR on the
	// media namespace is not a push about anything else, it is the answer to
	// our own LOAD, so it is always ours to act on.  Claiming it first is also
	// what stops update_status below either firing a second attempt or
	// swallowing the flag.
	LoadRequest retry_req;
	bool        do_retry  = false;
	const char* retry_how = nullptr;
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	if (retry_pending_) {
		retry_pending_ = false;
		retry_req      = last_load_;
		retry_how      = degrade_load(retry_req);
		do_retry       = retry_how != nullptr;
		}
	}

	// Routed through update_status rather than writing status_ by hand, so the
	// duration carry-forward and the condition-variable wake every SSE listener
	// is blocked on stay in one place.
	update_status({
		{"status", nlohmann::json::array({
			nlohmann::json{
				{"playerState",    "IDLE"},
				{"idleReason",     "ERROR"},
				{"mediaSessionId", media_session_id}
				}
			})}
		});

	// Re-armed only *after* update_status has run, and that ordering is the
	// whole reason this is a second critical section rather than part of the
	// one above.  The synthetic status published there is an IDLE/ERROR, so a
	// flag already re-armed would be consumed by it - degrading a second rung
	// and firing a second load for one refusal.  It is the same hazard the
	// note at the top of this function describes, one step further along:
	// claiming the flag first is what stops update_status acting on it, and
	// re-arming early would hand it straight back.
	if (do_retry) {
		{
		std::lock_guard<std::mutex> lk(status_mutex_);
		retry_pending_ = true;
		last_load_     = retry_req;
		}
		spawn_retry(retry_req, "receiver refused the LOAD", retry_how);
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

bool CastManager::cast_tracks(const std::vector<int>& track_ids)
	{
	int         msid;
	std::string state;
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	msid  = status_.media_session_id;
	state = status_.player_state;
	}
	// EDIT_TRACKS_INFO names the session it edits, and there is no session
	// until the receiver has answered a LOAD with one.  media_session_id is 0
	// until then, and a command naming session 0 is dropped without a reply -
	// so a track chosen in the second before playback starts would silently do
	// nothing.  The caller turns false into "ask again after the LOAD" rather
	// than into an error.
	if (msid == 0) return false;
	if (state != "PLAYING" && state != "PAUSED" && state != "BUFFERING")
		return false;

	send_media_cmd({{"type",           "EDIT_TRACKS_INFO"},
	                {"requestId",      13},
	                {"mediaSessionId", msid},
	                {"activeTrackIds", track_ids}});
	// Recorded on the load itself, which serves both readers of it: a page
	// reloading asks caption_state() what is on, and an auto-retry replays the
	// selection the viewer actually made rather than the one the film started
	// with.
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	last_load_.active_track_ids = track_ids;
	}
	std::cout << stamp() << "Cast: EDIT_TRACKS_INFO active="
	          << nlohmann::json(track_ids).dump() << std::endl;
	return true;
	}

CastManager::CaptionState CastManager::caption_state() const
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return CaptionState{ last_load_.caption_ids, last_load_.active_track_ids };
	}

CastManager::VolumeState CastManager::volume_state() const
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	return volume_;
	}

void CastManager::cast_volume(float level)
	{
	level = std::min(1.0f, std::max(0.0f, level));
	// A receiver-level command, like stop()'s STOP: it goes to receiver-0 on
	// the receiver namespace, needs no transport, and so works before
	// anything has been loaded.
	Tls t;
	if (!tls_connect(t, device_.address, device_.port)) {
		std::cout << stamp() << "Cast: connect failed for volume" << std::endl;
		return;
		}
	std::string src = "sender-0", dst = "receiver-0";
	cast_send(t.ssl, NS_CONN, src, dst, {{"type", "CONNECT"}});
	cast_send(t.ssl, NS_RECV, src, dst,
	          {{"type", "SET_VOLUME"}, {"requestId", next_request_id()},
	           {"volume", nlohmann::json{{"level", level}}}});
	std::cout << stamp() << "Cast: SET_VOLUME → " << level << std::endl;
	}

void CastManager::update_volume(const nlohmann::json& msg)
	{
	// A RECEIVER_STATUS need not carry a volume block: an application push may
	// omit it, just as a volume-only push omits the applications array. Absent
	// keeps the last known value: the same asymmetry lists_applications()
	// exists for, in the other direction.
	const auto& v = jsub(jsub(msg, "status"), "volume");
	if (!v.is_object()) return;
	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	volume_.known = true;
	volume_.level = (float)jnum(v, "level");
	const auto& mj = jsub(v, "muted");
	volume_.muted = mj.is_boolean() && mj.get<bool>();
	volume_.fixed = jstr(v, "controlType") == "fixed";
	}
	status_cv_.notify_all();
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
		// And one receiver-level snapshot for the volume: RECEIVER_STATUS is
		// only pushed on changes, so without asking once the level stays
		// unknown until someone turns a knob.
		cast_send(t.ssl, NS_RECV, src, "receiver-0",
		          {{"type", "GET_STATUS"}, {"requestId", 102}});
		connected_tid = tid;

		std::cout << stamp() << "Cast: poll_loop connected to transport " << tid << std::endl;

		// The receiver's messages are the only untrusted input on this thread,
		// and this thread is detached: anything escaping it is std::terminate, a
		// dead server rather than a failed request. Nothing in here should throw
		// - every field is read through jsonread.hh - so this is a backstop, and
		// it drops out to the reconnect above rather than ending the loop, since
		// a thread that has quietly stopped polling looks exactly like a
		// Chromecast that has stopped answering.
		try {
			while (poll_active_) {
				// If load() has started a new track, reconnect to the new transport.
				{
				std::lock_guard<std::mutex> lk(tid_mutex_);
				if (transport_id_ != connected_tid) break;
				}

				Recv why = Recv::Message;
				auto m = cast_recv(t.ssl, &why);
				if (m.is_null()) {
					// A timeout is SO_RCVTIMEO firing with no data - loop to
					// re-check poll_active_ and transport_id_ before blocking
					// again. Unusable is a message we could not read, which
					// says nothing about the ones after it.
					if (why == Recv::Timeout) {
						// Chromecast only pushes MEDIA_STATUS on state changes, not
						// during continuous playback - poll for position explicitly.
						cast_send(t.ssl, NS_MEDIA, src, tid,
						          {{"type", "GET_STATUS"}, {"requestId", 101}});
						continue;
						}
					if (why == Recv::Unusable) continue;
					break;  // closed connection - reconnect
					}

				std::string type = jstr(m, "type");
				if (type == "PING") {
					cast_send(t.ssl, NS_HEARTBEAT, src, "receiver-0",
					          {{"type", "PONG"}});
					}
				else if (type == "MEDIA_STATUS") {
					const auto& s = jidx(jsub(m, "status"), 0);
					if (s.is_object()) {
						std::string idle_reason = jstr(s, "idleReason");
						std::string state       = jstr(s, "playerState");
						std::cout << stamp() << "Cast rx MEDIA_STATUS"
						          << " state="    << (state.empty() ? "?" : state)
						          << " t="        << jnum(s, "currentTime")
						          << " dur="      << jnum(jsub(s, "media"), "duration")
						          << " msid="     << jint(s, "mediaSessionId")
						          << (idle_reason.empty() ? std::string{}
						                                  : " idleReason=" + idle_reason)
						          << std::endl;
						// Dump the full payload for every push - fields like autoplay,
						// loadingItemId, extendedStatus, supportedMediaCommands and
						// preloadedItemId reveal what the receiver thinks it's doing
						// and are essential for diagnosing stuck-IDLE sessions.
						std::cout << stamp() << "Cast rx MEDIA_STATUS payload: "
						          << m.dump() << std::endl;
						}
					update_status(m);
					}
				else if (type == "ERROR" || type == "LOAD_FAILED"
				      || type == "LOAD_CANCELLED") {
					std::cout << stamp() << "Cast rx " << type << ": "
					          << m.dump() << std::endl;
					// A failed LOAD comes back on this branch, not as a
					// MEDIA_STATUS - so before this, status_ stayed at its
					// default: IDLE with an *empty* idle_reason, which the SSE
					// reports as a track that is simply not playing yet. The
					// browser parked on a dead progress bar with nothing
					// anywhere saying why, and the retry watcher never saw the
					// failure it exists for. Synthesising the status the
					// receiver did not send puts both back on the ordinary
					// path.
					note_load_failure(jint(m, "mediaSessionId"));
					}
				else if (type == "RECEIVER_STATUS") {
					// Dump the full payload so we can see whether the Default
					// Media Receiver app is still running and what transportId
					// it's advertising - STOP can trigger an idle-app teardown
					// that invalidates our cached transport_id.
					std::cout << stamp() << "Cast rx RECEIVER_STATUS: "
					          << m.dump() << std::endl;
					update_volume(m);
					// And act on it, which nothing here used to do:
					// transport_id_ was written only by a load and cleared only
					// by stop(), so a receiver that tore the app down left a
					// dead transport cached for the rest of the session and
					// every LOAD after it went into the void - no MEDIA_STATUS,
					// no error, nothing in the log past "Cast: LOAD {…}".
					//
					// Only when the status actually enumerated what the
					// receiver is running. A volume-change push carries
					// `volume` and no applications array at all, and reading
					// that as "our app is gone" throws away a transport that is
					// still perfectly good - which costs the *next* load the
					// whole GET_STATUS-and-LAUNCH path.
					if (lists_applications(m)) {
						std::string tid_now =
							jstr(running_app(m, MEDIA_APP_ID), "transportId");
						std::lock_guard<std::mutex> lk(tid_mutex_);
						// The second test keeps this connection from speaking
						// for a session it no longer belongs to: a load that has
						// just launched a new receiver has already published its
						// transport, and a status arriving on the connection
						// this loop is about to drop must not put the old one
						// back.
						if (tid_now != connected_tid
						    && transport_id_ == connected_tid) {
							std::cout << stamp() << "Cast: transport "
							          << (tid_now.empty()
							                ? std::string("gone")
							                : "now " + tid_now)
							          << std::endl;
							// The inner loop's transport_id_ != connected_tid
							// check reconnects, or drops out to wait for the
							// next load if it is now empty.
							transport_id_ = tid_now;
							}
						}
					}
				else if (type != "PONG") {
					std::cout << stamp() << "Cast rx " << type << std::endl;
					}
				}
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "Cast: poll_loop error: " << e.what()
			          << " - reconnecting" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		catch (...) {
			std::cout << stamp() << "Cast: poll_loop error: unknown exception"
			          << " - reconnecting" << std::endl;
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			}
		}
	}

void CastManager::stop()
	{
	poll_active_ = false;
	active_ = false;
	{
	std::lock_guard<std::mutex> lk(token_mutex_);
	token_.clear();
	token_song_id_ = -1;
	token_caption_ids_.clear();
	}
	{
	std::lock_guard<std::mutex> lk(tid_mutex_);
	transport_id_.clear();
	}

	{
	std::lock_guard<std::mutex> lk(status_mutex_);
	status_ = CastStatus{};
	volume_ = VolumeState{};
	// The session is over, so anything it had to say is stale; a client
	// restoring one has nothing to be told about.
	notice_.clear();
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
	cast_send(t.ssl, NS_RECV, src, dst,
	          {{"type", "GET_STATUS"}, {"requestId", next_request_id()}});

	// Matched on appId like everywhere else: applications[0] is whatever the
	// set happens to be running, and stopping the wrong one is worse than
	// stopping nothing. Through cast_wait_for() so the deadline is a real one
	// and a PING does not consume one of the five reads this used to get.
	auto m = cast_wait_for(t, src, in_ms(STATUS_WAIT_MS),
	                       [](const nlohmann::json& j) {
	                           return jstr(j, "type") == "RECEIVER_STATUS";
	                           });
	std::string session_id = jstr(running_app(m, MEDIA_APP_ID), "sessionId");

	if (!session_id.empty())
		cast_send(t.ssl, NS_RECV, src, dst,
		          {{"type", "STOP"}, {"requestId", next_request_id()},
		           {"sessionId", session_id}});

	std::cout << stamp() << "Cast: stopped" << std::endl;
	}

// gaindrive-cast — a command-line front end to the Chromecast code, for
// working out why we see fewer devices than the Google Cast SDK does.
//
// Two subcommands, deliberately different in kind:
//
//   discover  drives CastManager::discover() and so shows exactly what the
//             server sees. Every knob of DiscoverOpts is a flag, so the
//             candidate causes can be tried one at a time.
//   browse    does *not* go through CastManager. It opens its own sockets on
//             every interface and prints every _googlecast._tcp record as it
//             arrives, indefinitely. That is the ground truth to measure
//             discover() against — run it beside `avahi-browse -rt
//             _googlecast._tcp` and the gap is the bug.
//
// This binary links only castmanager.cc and stamp.cc: no database, no HTTP
// server, no embedded web client.

#include <mdns.h>

#include "castmanager.hh"
#include "stamp.hh"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cxxopts.hpp>

static const char SVC[] = "_googlecast._tcp.local.";

// Ctrl-C ends browse cleanly rather than killing it mid-line.
static std::atomic<bool> stop_flag{false};
static void on_sigint(int) { stop_flag = true; }

// ---- browse -------------------------------------------------------
//
// A socket per interface per family, so nothing is hidden by the kernel's
// choice of a single default multicast interface.

struct BrowseSocket {
	int         fd = -1;
	bool        v6 = false;
	std::string iface;
	};

static int browse_cb(int, const struct sockaddr* from, size_t,
                     mdns_entry_type_t entry, uint16_t,
                     uint16_t rtype, uint16_t, uint32_t ttl,
                     const void* data, size_t size,
                     size_t name_off, size_t,
                     size_t rec_off, size_t rec_len,
                     void* user_data)
	{
	const char* iface = reinterpret_cast<const char*>(user_data);
	char buf1[256], buf2[256];

	size_t off = name_off;
	mdns_string_t rname = mdns_string_extract(data, size, &off, buf1, sizeof(buf1));
	std::string name(rname.str, rname.length);

	char src[INET6_ADDRSTRLEN + IF_NAMESIZE + 2] = "?";
	if (from && from->sa_family == AF_INET) {
		inet_ntop(AF_INET,
		          &reinterpret_cast<const struct sockaddr_in*>(from)->sin_addr,
		          src, sizeof(src));
		}
	else if (from && from->sa_family == AF_INET6) {
		const auto* s6 = reinterpret_cast<const struct sockaddr_in6*>(from);
		inet_ntop(AF_INET6, &s6->sin6_addr, src, sizeof(src));
		char nm[IF_NAMESIZE] = {};
		if (s6->sin6_scope_id && if_indextoname(s6->sin6_scope_id, nm)) {
			strncat(src, "%", sizeof(src) - strlen(src) - 1);
			strncat(src, nm, sizeof(src) - strlen(src) - 1);
			}
		}

	const char* sect = entry == MDNS_ENTRYTYPE_ANSWER    ? "answer"
	                 : entry == MDNS_ENTRYTYPE_AUTHORITY ? "auth  "
	                                                     : "extra ";

	std::cout << stamp() << "[" << iface << "] " << sect
	          << " from=" << src << " ttl=" << ttl << " " << name << " ";

	// Every record type is printed, including the ones discover() ignores —
	// a PTR-only responder is one of the things we are looking for.
	if (rtype == MDNS_RECORDTYPE_PTR) {
		mdns_string_t p = mdns_record_parse_ptr(data, size, rec_off, rec_len,
		                                        buf2, sizeof(buf2));
		std::cout << "PTR  -> " << std::string(p.str, p.length);
		}
	else if (rtype == MDNS_RECORDTYPE_SRV) {
		mdns_record_srv_t srv = mdns_record_parse_srv(data, size, rec_off, rec_len,
		                                              buf2, sizeof(buf2));
		std::cout << "SRV  -> " << std::string(srv.name.str, srv.name.length)
		          << ":" << srv.port;
		}
	else if (rtype == MDNS_RECORDTYPE_A) {
		struct sockaddr_in a4 = {};
		mdns_record_parse_a(data, size, rec_off, rec_len, &a4);
		char ip[INET_ADDRSTRLEN];
		inet_ntop(AF_INET, &a4.sin_addr, ip, sizeof(ip));
		std::cout << "A    -> " << ip;
		}
	else if (rtype == MDNS_RECORDTYPE_AAAA) {
		struct sockaddr_in6 a6 = {};
		mdns_record_parse_aaaa(data, size, rec_off, rec_len, &a6);
		char ip[INET6_ADDRSTRLEN];
		inet_ntop(AF_INET6, &a6.sin6_addr, ip, sizeof(ip));
		std::cout << "AAAA -> " << ip;
		}
	else if (rtype == MDNS_RECORDTYPE_TXT) {
		mdns_record_txt_t txt[64];
		size_t n = mdns_record_parse_txt(data, size, rec_off, rec_len, txt, 64);
		std::cout << "TXT ";
		for (size_t i = 0; i < n; i++)
			std::cout << " " << std::string(txt[i].key.str, txt[i].key.length)
			          << "=" << std::string(txt[i].value.str, txt[i].value.length);
		}
	else {
		std::cout << "type=" << rtype;
		}

	std::cout << std::endl;
	return 0;
	}

static int run_browse(int timeout_ms, int interval_ms, const std::string& want_iface)
	{
	std::vector<BrowseSocket> socks;

	struct ifaddrs* iflist = nullptr;
	if (getifaddrs(&iflist) < 0) {
		std::cout << stamp() << "getifaddrs failed (errno " << errno << ")" << std::endl;
		return 1;
		}
	std::vector<unsigned int> seen6;
	for (struct ifaddrs* ifa = iflist; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr) continue;
		if (ifa->ifa_flags & IFF_LOOPBACK) continue;
		if (!(ifa->ifa_flags & IFF_UP)) continue;
		if (!(ifa->ifa_flags & IFF_MULTICAST)) continue;
		if (!want_iface.empty() && want_iface != ifa->ifa_name) continue;

		if (ifa->ifa_addr->sa_family == AF_INET) {
			struct sockaddr_in sa = {};
			sa.sin_family = AF_INET;
			sa.sin_addr   = reinterpret_cast<struct sockaddr_in*>(ifa->ifa_addr)->sin_addr;
			sa.sin_port   = htons(MDNS_PORT);
			int s = mdns_socket_open_ipv4(&sa);
			if (s < 0) {
				std::cout << stamp() << "IPv4 socket on " << ifa->ifa_name
				          << " failed (errno " << errno << ")" << std::endl;
				continue;
				}
			socks.push_back({s, false, ifa->ifa_name});
			}
		else if (ifa->ifa_addr->sa_family == AF_INET6) {
			unsigned int idx = if_nametoindex(ifa->ifa_name);
			if (!idx) continue;
			bool dup = false;
			for (auto i : seen6) if (i == idx) dup = true;
			if (dup) continue;
			seen6.push_back(idx);

			// Bind port 5353 rather than letting mdns.h take an ephemeral one:
			// that is what clears the QU bit in the outgoing query
			// (mdns.h:1080), so replies come back as multicast and every
			// listener on the link sees them. The ephemeral-port default asks
			// for unicast replies, which is the asymmetry discover() has today.
			struct sockaddr_in6 sa6 = {};
			sa6.sin6_family = AF_INET6;
			sa6.sin6_addr   = in6addr_any;
			sa6.sin6_port   = htons(MDNS_PORT);
			int s = mdns_socket_open_ipv6(&sa6);
			if (s < 0) {
				std::cout << stamp() << "IPv6 socket on " << ifa->ifa_name
				          << " failed (errno " << errno << ")" << std::endl;
				continue;
				}
			setsockopt(s, IPPROTO_IPV6, IPV6_MULTICAST_IF, &idx, sizeof(idx));
			struct ipv6_mreq req = {};
			req.ipv6mr_multiaddr.s6_addr[0]  = 0xFF;
			req.ipv6mr_multiaddr.s6_addr[1]  = 0x02;
			req.ipv6mr_multiaddr.s6_addr[15] = 0xFB;
			req.ipv6mr_interface = idx;
			setsockopt(s, IPPROTO_IPV6, IPV6_JOIN_GROUP, &req, sizeof(req));
			socks.push_back({s, true, ifa->ifa_name});
			}
		}
	freeifaddrs(iflist);

	if (socks.empty()) {
		std::cout << stamp() << "no usable interface"
		          << (want_iface.empty() ? "" : " matching " + want_iface)
		          << std::endl;
		return 1;
		}

	std::cout << stamp() << "browsing " << SVC << " on " << socks.size()
	          << " socket(s):";
	for (auto& s : socks)
		std::cout << " " << (s.v6 ? "IPv6" : "IPv4") << "/" << s.iface;
	std::cout << "  (Ctrl-C to stop)" << std::endl;

	std::vector<uint8_t> sendbuf(2048);
	std::vector<uint8_t> recvbuf(9000);   // RFC 6762 §17 maximum message size

	auto start     = std::chrono::steady_clock::now();
	auto next_send = start;
	bool forever   = timeout_ms <= 0;
	auto deadline  = start + std::chrono::milliseconds(forever ? 0 : timeout_ms);

	while (!stop_flag) {
		auto t = std::chrono::steady_clock::now();
		if (!forever && t >= deadline) break;

		if (t >= next_send) {
			for (auto& s : socks) {
				int r = mdns_query_send(s.fd, MDNS_RECORDTYPE_PTR, SVC, strlen(SVC),
				                        sendbuf.data(), sendbuf.size(), 0);
				std::cout << stamp() << "[" << s.iface << "] query "
				          << (s.v6 ? "IPv6 " : "IPv4 ")
				          << (r < 0 ? "failed" : "sent") << std::endl;
				}
			next_send = t + std::chrono::milliseconds(interval_ms);
			}

		auto wake = next_send;
		if (!forever && deadline < wake) wake = deadline;
		auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		              wake - std::chrono::steady_clock::now()).count();
		if (us < 0) us = 0;
		// Wake at least ten times a second so Ctrl-C is noticed promptly.
		if (us > 100000) us = 100000;
		struct timeval tv = { (time_t)(us / 1000000), (suseconds_t)(us % 1000000) };

		fd_set fds;
		FD_ZERO(&fds);
		int maxfd = 0;
		for (auto& s : socks) { FD_SET(s.fd, &fds); maxfd = std::max(maxfd, s.fd); }
		int r = select(maxfd + 1, &fds, nullptr, nullptr, &tv);
		if (r < 0) {
			if (errno == EINTR) continue;
			std::cout << stamp() << "select failed (errno " << errno << ")" << std::endl;
			break;
			}
		if (r == 0) continue;
		for (auto& s : socks) {
			if (!FD_ISSET(s.fd, &fds)) continue;
			mdns_query_recv(s.fd, recvbuf.data(), recvbuf.size(), browse_cb,
			                const_cast<char*>(s.iface.c_str()), 0);
			}
		}

	for (auto& s : socks) mdns_socket_close(s.fd);
	std::cout << stamp() << "browse stopped" << std::endl;
	return 0;
	}

// ---- discover -----------------------------------------------------

static int run_discover(const CastManager::DiscoverOpts& opts)
	{
	CastManager cm;
	auto devs = cm.discover(opts);

	std::cout << std::endl;
	std::cout << devs.size() << " device(s) returned" << std::endl;
	for (auto& d : devs)
		std::cout << "  " << d.address << ":" << d.port
		          << "  id=" << d.id
		          << "  name=" << d.name << std::endl;

	// Non-zero on an empty result so a shell loop can tell the runs apart.
	return devs.empty() ? 1 : 0;
	}

// ---- probe --------------------------------------------------------

/// Ask one device, by address, whether it is there.
///
/// The same CastManager::probe() the server runs over its configured devices at
/// startup, so the tool and the log line cannot disagree about what "answered"
/// means. Address form is [name=]address[:port], matching --cast-device, with
/// an IPv6 address bracketed if it carries a port.
static int run_probe(const std::string& target, int timeout_ms)
	{
	CastManager::CastDevice dev;
	dev.address = target;
	dev.port    = 8009;

	auto close  = target.rfind(']');
	auto colon  = target.rfind(':');
	bool braces = !target.empty() && target.front() == '['
	           && close != std::string::npos;
	// A port only where it can be told from an IPv6 address: after the closing
	// bracket, or when there is exactly one colon in the whole string.
	if (colon != std::string::npos
	        && (braces ? colon > close : target.find(':') == colon)) {
		std::string digits = target.substr(colon + 1);
		if (!digits.empty()
		        && digits.find_first_not_of("0123456789") == std::string::npos) {
			dev.port    = std::atoi(digits.c_str());
			dev.address = target.substr(0, colon);
			}
		}
	if (dev.address.size() >= 2 && dev.address.front() == '['
	        && dev.address.back() == ']')
		dev.address = dev.address.substr(1, dev.address.size() - 2);

	CastManager cm;
	auto result = cm.probe(dev, timeout_ms);
	std::cout << dev.address << ":" << dev.port << " — "
	          << CastManager::probe_text(result) << std::endl;
	return result == CastManager::Probe::ANSWERED ? 0 : 1;
	}

int main(int argc, char** argv)
	{
	signal(SIGINT, on_sigint);

	cxxopts::Options options("gaindrive-cast",
		"Chromecast discovery diagnostics.\n"
		"\n"
		"  gaindrive-cast discover [options]      what the gaindrive server sees\n"
		"  gaindrive-cast browse   [options]      every record on the wire\n"
		"  gaindrive-cast probe    <address>      ask one device directly\n");

	options.add_options()
		("h,help",      "Show this help")
		("timeout",     "Milliseconds to run for (browse: 0 = until Ctrl-C)",
		                cxxopts::value<int>()->default_value("-1"))
		("queries",     "discover: PTR queries to send per socket",
		                cxxopts::value<int>()->default_value("1"))
		("gap",         "discover: ms before the 2nd query; doubles thereafter",
		                cxxopts::value<int>()->default_value("1000"))
		("interval",    "browse: ms between repeated queries",
		                cxxopts::value<int>()->default_value("5000"))
		("per-interface", "discover: open a socket per interface, not one for all")
		("iface",       "Restrict to one interface by name",
		                cxxopts::value<std::string>()->default_value(""))
		("no-ipv4",     "discover: skip IPv4")
		("no-ipv6",     "discover: skip IPv6")
		("any-id",      "discover: keep devices that sent no TXT id")
		("q,quiet",     "discover: do not log every record")
		("command",     "discover | browse | probe", cxxopts::value<std::string>())
		("address",     "probe: the device, as address[:port]",
		                cxxopts::value<std::string>()->default_value(""));

	options.parse_positional({"command", "address"});
	options.positional_help("discover|browse|probe [address]");

	std::string cmd;
	cxxopts::ParseResult args;
	try {
		args = options.parse(argc, argv);
		if (args.count("command")) cmd = args["command"].as<std::string>();
		}
	catch (const std::exception& e) {
		std::cout << "gaindrive-cast: " << e.what() << std::endl;
		return 2;
		}

	if (args.count("help") || cmd.empty()) {
		std::cout << options.help() << std::endl;
		return cmd.empty() && !args.count("help") ? 2 : 0;
		}

	int timeout = args["timeout"].as<int>();

	if (cmd == "probe") {
		std::string target = args["address"].as<std::string>();
		if (target.empty()) {
			std::cout << "gaindrive-cast: probe needs an address" << std::endl;
			return 2;
			}
		// 6000 matches CastManager::probe's own default, which is what the
		// server's startup check uses.
		return run_probe(target, timeout > 0 ? timeout : 6000);
		}

	if (cmd == "browse")
		return run_browse(timeout < 0 ? 0 : timeout,       // browse: run forever
		                  args["interval"].as<int>(),
		                  args["iface"].as<std::string>());

	if (cmd == "discover") {
		CastManager::DiscoverOpts opts;
		if (timeout >= 0) opts.timeout_ms = timeout;       // else the 4000 default
		opts.queries       = args["queries"].as<int>();
		opts.query_gap_ms  = args["gap"].as<int>();
		opts.per_interface = args.count("per-interface") > 0;
		opts.iface         = args["iface"].as<std::string>();
		opts.ipv4          = args.count("no-ipv4") == 0;
		opts.ipv6          = args.count("no-ipv6") == 0;
		opts.require_id    = args.count("any-id") == 0;
		opts.verbose       = args.count("quiet") == 0;
		if (!opts.ipv4 && !opts.ipv6) {
			std::cout << "gaindrive-cast: --no-ipv4 and --no-ipv6 leave nothing "
			             "to query on" << std::endl;
			return 2;
			}
		return run_discover(opts);
		}

	std::cout << "gaindrive-cast: unknown command '" << cmd << "'" << std::endl
	          << options.help() << std::endl;
	return 2;
	}

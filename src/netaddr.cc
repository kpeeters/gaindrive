#include "netaddr.hh"
#include "subsonic.hh"
#include "stamp.hh"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

// Not declared in httplib.h: upstream defines this only in the implementation
// half, so since the split (third_party/README.md) the declaration has to live
// here. One more piece of httplib we use beyond its public contract.
namespace httplib {
std::string get_client_ip(const std::string& x_forwarded_for,
                          const std::vector<std::string>& trusted_proxies);
}

static std::vector<std::string> trusted_proxies_ = { "127.0.0.1", "::1" };

// An IPv4 peer on a dual-stack listener shows up as `::ffff:127.0.0.1`, which
// is the same host as `127.0.0.1` and must match the same configuration entry.
// Compared both ways so a proxy list written in either spelling works.
static std::string strip_v4_mapped(const std::string& addr)
	{
	static const std::string prefix = "::ffff:";
	if (addr.rfind(prefix, 0) == 0 &&
	    addr.find('.') != std::string::npos)
		return addr.substr(prefix.size());
	return addr;
	}

void gaindrive_set_trusted_proxies(std::vector<std::string> addrs)
	{
	trusted_proxies_ = std::move(addrs);
	}

static std::string public_url_;

void gaindrive_set_public_url(std::string origin)
	{
	public_url_ = std::move(origin);
	}

const std::string& public_url()
	{
	return public_url_;
	}

static bool from_trusted_proxy(const httplib::Request& req)
	{
	const std::string peer = strip_v4_mapped(req.remote_addr);
	for (const auto& p : trusted_proxies_)
		if (strip_v4_mapped(p) == peer) return true;
	return false;
	}

std::string client_addr(const httplib::Request& req)
	{
	if (!from_trusted_proxy(req)) return req.remote_addr;

	const std::string xff = req.get_header_value("X-Forwarded-For");
	if (xff.empty()) return req.remote_addr;

	// Rightmost untrusted entry — see the comment on trusted_proxies_.
	std::string addr = httplib::get_client_ip(xff, trusted_proxies_);
	if (addr.empty()) return req.remote_addr;

	// Some proxies write `1.2.3.4:5678` or `[2001:db8::1]:443`. The throttle
	// keys on this string, so a port would split one client into many buckets.
	if (addr.front() == '[') {
		const auto close = addr.find(']');
		if (close != std::string::npos) addr = addr.substr(1, close - 1);
		}
	else if (std::count(addr.begin(), addr.end(), ':') == 1)
		addr = addr.substr(0, addr.find(':'));
	return addr;
	}

// The subnets this machine is directly attached to.
//
// Interfaces are chosen by flag and never by name, because a tunnel is
// precisely what the two flag tests exclude: WireGuard, OpenVPN in tun mode
// and PPP are all point-to-point and none carries broadcast, so a VPN client's
// address falls in no subnet listed here even though it is private and even
// though it reaches us perfectly well. Matching on `wg`/`tun`/`utun` instead
// would do the same job until somebody renamed an interface or built this on
// another platform. castmanager.cc's list_ifaces4() makes the same kind of
// judgement with IFF_MULTICAST, for its own unrelated reason.
//
// What no test at this layer can see is a VPN bridged into the LAN, or a
// router handing VPN clients addresses out of the LAN's own pool: such a
// client *is* on the subnet by every question we are able to ask. ISSUES.md
// carries it as an accepted risk.
struct LocalNet { int family; size_t len; uint8_t addr[16], mask[16]; };

static std::vector<LocalNet> local_nets()
	{
	// Re-read rather than computed once at startup: a DHCP renewal or a new
	// IPv6 prefix changes the answer, and a server up for a month would
	// otherwise still be deciding on the network it booted into. Cached for a
	// few seconds because this is consulted per request.
	static std::mutex                             mu;
	static std::vector<LocalNet>                  cache;
	static std::chrono::steady_clock::time_point  taken;

	std::lock_guard<std::mutex> lk(mu);
	const auto now = std::chrono::steady_clock::now();
	if (!cache.empty() && now - taken < std::chrono::seconds(10)) return cache;

	std::vector<LocalNet> out;
	struct ifaddrs* iflist;
	if (getifaddrs(&iflist) < 0) {
		// Deliberately not cached, and deliberately logged: an empty answer
		// refuses every cast, and freezing one in for ten seconds would turn a
		// momentary failure into a window nobody could explain afterwards.
		std::cout << stamp() << "getifaddrs failed (" << std::strerror(errno)
		          << "); no address can be recognised as local" << std::endl;
		return out;
		}
	for (struct ifaddrs* ifa = iflist; ifa; ifa = ifa->ifa_next) {
		if (!ifa->ifa_addr || !ifa->ifa_netmask)  continue;
		if (!(ifa->ifa_flags & IFF_UP))           continue;
		if (ifa->ifa_flags & IFF_LOOPBACK)        continue;
		if (ifa->ifa_flags & IFF_POINTOPOINT)     continue;
		if (!(ifa->ifa_flags & IFF_BROADCAST))    continue;

		LocalNet n{};
		if (ifa->ifa_addr->sa_family == AF_INET) {
			n.family = AF_INET;
			n.len    = 4;
			std::memcpy(n.addr, &reinterpret_cast<struct sockaddr_in*>
			                     (ifa->ifa_addr)->sin_addr, 4);
			std::memcpy(n.mask, &reinterpret_cast<struct sockaddr_in*>
			                     (ifa->ifa_netmask)->sin_addr, 4);
			}
		else if (ifa->ifa_addr->sa_family == AF_INET6) {
			// The netmask of an IPv6 address is its prefix, so a link-local
			// fe80::/64 and a global /64 are both handled by the same compare.
			n.family = AF_INET6;
			n.len    = 16;
			std::memcpy(n.addr, &reinterpret_cast<struct sockaddr_in6*>
			                     (ifa->ifa_addr)->sin6_addr, 16);
			std::memcpy(n.mask, &reinterpret_cast<struct sockaddr_in6*>
			                     (ifa->ifa_netmask)->sin6_addr, 16);
			}
		else continue;
		out.push_back(n);
		}
	freeifaddrs(iflist);
	cache = out;
	taken = now;
	return out;
	}

bool client_is_local(const httplib::Request& req)
	{
	const std::string who = strip_v4_mapped(client_addr(req));

	uint8_t raw[16];
	size_t  len;
	int     family;

	struct in_addr  v4;
	struct in6_addr v6;
	if (inet_pton(AF_INET, who.c_str(), &v4) == 1) {
		if (((ntohl(v4.s_addr) >> 24) & 0xff) == 127) return public_url_.empty();
		family = AF_INET;
		len    = 4;
		std::memcpy(raw, &v4, 4);
		}
	else {
		// A scope suffix is how a link-local address is written and inet_pton
		// does not accept one; is_ip_literal() in main.cc strips it the same way.
		const std::string bare = who.substr(0, who.find('%'));
		if (inet_pton(AF_INET6, bare.c_str(), &v6) != 1) return false;
		if (IN6_IS_ADDR_LOOPBACK(&v6)) return public_url_.empty();
		family = AF_INET6;
		len    = 16;
		std::memcpy(raw, &v6, 16);
		}

	for (const auto& n : local_nets()) {
		if (n.family != family) continue;
		bool same = true;
		for (size_t i = 0; i < len && same; i++)
			if ((raw[i] & n.mask[i]) != (n.addr[i] & n.mask[i])) same = false;
		if (same) return true;
		}
	return false;
	}

// ---- Outbound fetch guard ---------------------------------------------

// True if `addr` is one an outbound fetch may go to: a globally routable
// unicast address and nothing else.
//
// Anything else is a request the *server* can make and the caller cannot, and
// that difference is the whole of an SSRF: loopback reaches admin interfaces
// bound to 127.0.0.1, link-local reaches 169.254.169.254, and RFC1918 reaches
// every other machine on the LAN. The unspecified and multicast cases are here
// because 0.0.0.0 is another spelling of loopback on Linux and a multicast
// destination is never a web server.
static bool addr_is_global(const struct sockaddr* addr)
	{
	if (addr->sa_family == AF_INET) {
		const uint32_t a = ntohl(
			reinterpret_cast<const struct sockaddr_in*>(addr)->sin_addr.s_addr);
		const uint8_t b0 = (a >> 24) & 0xff, b1 = (a >> 16) & 0xff;
		if (b0 == 0)                        return false;  // 0.0.0.0/8
		if (b0 == 10)                       return false;  // 10/8
		if (b0 == 127)                      return false;  // loopback
		if (b0 == 169 && b1 == 254)         return false;  // link-local
		if (b0 == 172 && (b1 & 0xf0) == 16) return false;  // 172.16/12
		if (b0 == 192 && b1 == 168)         return false;  // 192.168/16
		if (b0 == 100 && (b1 & 0xc0) == 64) return false;  // CGNAT 100.64/10
		if (b0 >= 224)                      return false;  // multicast, reserved
		return true;
		}
	if (addr->sa_family == AF_INET6) {
		const auto& s6 = reinterpret_cast<const struct sockaddr_in6*>(addr)->sin6_addr;
		if (IN6_IS_ADDR_LOOPBACK(&s6) || IN6_IS_ADDR_UNSPECIFIED(&s6)
		    || IN6_IS_ADDR_LINKLOCAL(&s6) || IN6_IS_ADDR_SITELOCAL(&s6)
		    || IN6_IS_ADDR_MULTICAST(&s6))
			return false;
		// Unique local (fc00::/7), and IPv4-mapped, which would otherwise be a
		// straight bypass of every rule above. V4COMPAT is the deprecated
		// spelling of the same idea (::1.2.3.4, RFC 4291 §2.5.5.1) — modern
		// stacks refuse to route it, but "refuse" is their promise, not
		// ours, and the check costs one macro beside the one that matters.
		if ((s6.s6_addr[0] & 0xfe) == 0xfc) return false;
		if (IN6_IS_ADDR_V4MAPPED(&s6) || IN6_IS_ADDR_V4COMPAT(&s6)) {
			struct sockaddr_in v4{};
			v4.sin_family = AF_INET;
			std::memcpy(&v4.sin_addr.s_addr, s6.s6_addr + 12, 4);
			return addr_is_global(reinterpret_cast<struct sockaddr*>(&v4));
			}
		return true;
		}
	return false;
	}

bool host_is_global(const std::string& host)
	{
	// Strip a bracketed IPv6 literal and any :port.
	std::string h = host;
	if (!h.empty() && h.front() == '[') {
		auto close = h.find(']');
		if (close == std::string::npos) return false;
		h = h.substr(1, close - 1);
		}
	else {
		auto colon = h.find(':');
		if (colon != std::string::npos) h = h.substr(0, colon);
		}
	if (h.empty()) return false;

	struct addrinfo hints{};
	hints.ai_family   = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	struct addrinfo* result = nullptr;
	if (getaddrinfo(h.c_str(), nullptr, &hints, &result) != 0 || !result)
		return false;

	bool all_global = true;
	for (auto* ai = result; ai; ai = ai->ai_next)
		if (!addr_is_global(ai->ai_addr)) {
			all_global = false;
			break;
			}
	freeaddrinfo(result);
	return all_global;
	}

std::pair<std::string, int> split_host_port(const std::string& host,
                                            int default_port)
	{
	if (!host.empty() && host.front() == '[') {
		auto close = host.find(']');
		if (close == std::string::npos) return { host, default_port };
		std::string h = host.substr(1, close - 1);
		if (close + 1 < host.size() && host[close + 1] == ':')
			return { h, to_int(host.substr(close + 2), default_port) };
		return { h, default_port };
		}
	auto colon = host.find(':');
	if (colon == std::string::npos) return { host, default_port };
	return { host.substr(0, colon),
	         to_int(host.substr(colon + 1), default_port) };
	}


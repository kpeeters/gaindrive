#pragma once

// Everything that decides something from a network address, in both
// directions: who an inbound request is really from and whether it is on this
// machine's own network, and whether an outbound fetch is allowed to go where
// it has been pointed.
//
// The two halves live together because they are the same question asked from
// opposite ends, and because both are consulted from several route files as
// well as from the info resolver.

#include <string>
#include <utility>
#include <vector>

#include <httplib.h>

// Which peers' X-Forwarded-For header may be believed. Defaults to loopback,
// which is the reverse proxy the packaging installs.
//
// Believing the header from an untrusted peer is not merely a wrong log line:
// the login throttle keys on the result, so it would let a caller pick a fresh
// rate-limit bucket per request.
void gaindrive_set_trusted_proxies(std::vector<std::string> addrs);

// The origin this server is reachable at — scheme and host, no trailing
// slash — when a deployment knows it. Consulted wherever a URL for a third
// device is composed; empty means "believe the request's Host header", which
// is right on a LAN, where there is no other source, and wrong on the public
// internet, where Host is whatever the caller typed.
void gaindrive_set_public_url(std::string origin);
const std::string& public_url();

// The address to attribute a request to in the log.
//
// Behind a reverse proxy every request arrives *from the proxy*, so
// `remote_addr` is the same value for all of them and distinguishes nothing —
// which matters most for exactly the question it is there to answer: whether a
// request came from a browser or from a Chromecast fetching for itself. Apache
// and nginx both pass the original along in `X-Forwarded-For`, a
// comma-separated chain.
//
// **The entry taken is the rightmost one that is not a trusted proxy, never
// the first.** Proxies *append* the peer they heard from, so what arrives here
// is `<whatever the client sent>, <real client>` — the leftmost entry is the
// one string in the chain the attacker composed in full. This code used to
// take it, which handed the login throttle's bucket to the caller: send a
// fresh X-Forwarded-For per request and there is no rate limit at all.
// httplib::get_client_ip() walks from the right and skips configured proxies,
// which is the standard answer, and one copy of it beats a second one here.
//
// **Trusted only from a configured proxy.** The header is client-supplied and
// forgeable, so honouring it from any peer lets a caller write whatever it
// likes into the log — and, since the login throttle keys on this, choose its
// own rate-limit bucket. It used to be trusted unconditionally, which was
// defensible while nothing read it back; the throttle is what made that stop
// being true.
//
// The default list is loopback, because a proxy on the same host is what the
// packaging sets up and what `host: 127.0.0.1` in the example config assumes.
// A server reachable directly gets `remote_addr`, which cannot be spoofed past
// the TCP handshake.
std::string client_addr(const httplib::Request& req);

// True when the request came from a network this machine is attached to.
//
// The address tested is client_addr()'s, so behind a reverse proxy this is
// only ever as good as that proxy's X-Forwarded-For. A proxy that sends none
// makes every caller in the world look like loopback, which is the one way
// this check can fail open — hence loopback counts only where public_url is
// unset and the deployment is therefore saying it is not on a public address.
// The same distinction cast_load_song() draws for the Host header. README's
// public-address checklist carries the operator's half of it.
bool client_is_local(const httplib::Request& req);

// ---- Outbound fetch guard ---------------------------------------------

// How many redirects an outbound image fetch will follow before giving up.
// Shared by setCoverArt, which follows a URL a person typed, and by
// portrait_fetch, which follows one a metadata provider returned.
inline constexpr int MAX_COVER_REDIRECTS = 5;


// True if every address `host` resolves to is globally routable. Every one,
// not any: a name that resolves to both a public address and 127.0.0.1 is a
// standard way of getting a checked fetch to connect somewhere else.
//
// This is not a complete defence — the resolution here and the one httplib
// performs when it connects are two separate lookups, so a name whose answer
// changes between them is still a hole (DNS rebinding). Closing that needs the
// connection to be made to an address we resolved ourselves, which httplib's
// client does not offer. It is recorded rather than fixed because the
// remaining hole needs an attacker-controlled nameserver, while what it
// replaced needed only a typed URL.
bool host_is_global(const std::string& host);

// The authority part of a fetched URL, split for the client constructors.
// SSLClient's single-string constructor does not parse a `:port` — the colon
// and digits go into the TLS SNI and the connect fails — so the split has to
// happen here, for both clients, or an https URL with an explicit port
// silently fetches nothing.
std::pair<std::string, int> split_host_port(const std::string& host,
                                            int default_port);

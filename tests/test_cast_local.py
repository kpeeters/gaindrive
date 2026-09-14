#!/usr/bin/env python3
"""Casting is refused from off the server's own network.

The point of the feature: opening the web client from a hotel in another
country must not be able to start music playing in an empty house, and a VPN
must not qualify either -- a full tunnel from that same hotel reaches the
speakers just as well.

**No Chromecast and no second network are needed.** `trusted_proxies` defaults
to loopback, so a request from localhost carrying `X-Forwarded-For` has that
header believed, and `client_addr()` returns whatever address it names. That
is the whole test rig: one header stands in for standing somewhere else.

`public_url` must stay unset, which is the default -- with it set, loopback
stops counting as local and the baseline calls below would be refused too.
That asymmetry is deliberate and is what stops a proxy which forgets
`X-Forwarded-For` from making every caller in the world look like loopback.

Start the server with a device at an address nothing will answer on:

    ./build/gaindrive --db /tmp/gd_test.db --artist-root music=/music \\
        --cast-device fake=192.0.2.1:8009

192.0.2.0/24 is TEST-NET-1 (RFC 5737) and is not routable, so the detached
load worker fails quietly and costs one log line. Then:

    python3 tests/test_cast_local.py

The account must have castRole.
"""

import json
import socket
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

# Must match --cast-device, as in test_cast_session.py: a configured device's
# id is derived from its address rather than random, so it survives a restart.
DEVICE_ID = "manual:192.0.2.1:8009"

# The hotel. TEST-NET-3, so it can never be a network this machine is on.
ELSEWHERE = "203.0.113.5"
# The routed VPN: private, reaches us perfectly well, and still not ours.
# 10/8 is deliberate -- "private" is exactly what a naive test would accept.
VPN = "10.99.99.99"

# Every cast endpoint except stopCast. stopCast is absent on purpose: refusing
# it would leave the music playing in the house with no way to end it from
# wherever the person holding the laptop now is.
GATED = ["listCastDevices", "setCastDevicePref", "startCast", "castEvents",
         "castSession", "castControl", "castLoad"]


def _url(endpoint, controller=None, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "json"}
    if controller is not None:
        p["castController"] = controller
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(p)}"


def _get(endpoint, controller=None, extra=None, forwarded_for=None):
    """A Subsonic JSON call. Returns the subsonic-response object."""
    req = urllib.request.Request(_url(endpoint, controller, extra))
    if forwarded_for is not None:
        req.add_header("X-Forwarded-For", forwarded_for)
    with urllib.request.urlopen(req, timeout=10) as r:
        return json.loads(r.read())["subsonic-response"]


def _ok(resp, why=""):
    assert resp.get("status") == "ok", f"{why}: {resp}"


def _failed(resp, code=None, why=""):
    assert resp.get("status") == "failed", f"{why}: expected failure, got {resp}"
    if code is not None:
        got = resp.get("error", {}).get("code")
        assert got == code, f"{why}: expected error {code}, got {got}: {resp}"


def _own_address():
    """This machine's address on the route out, without sending anything.

    A UDP connect only selects a route and binds a local address; no packet
    leaves. Returns None where there is no default route, which is the one
    case the positive test cannot be run.
    """
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    try:
        s.connect(("8.8.8.8", 9))
        return s.getsockname()[0]
    except OSError:
        return None
    finally:
        s.close()


def test_ping_reports_locality():
    _ok(_get("ping"), "ping from loopback")
    assert _get("ping")["localNetwork"] is True, \
        "loopback should be local while public_url is unset"
    assert _get("ping", forwarded_for=ELSEWHERE)["localNetwork"] is False, \
        "a public address is not local"
    assert _get("ping", forwarded_for=VPN)["localNetwork"] is False, \
        "a private address that is not one of ours is not local"


def test_own_subnet_is_local():
    mine = _own_address()
    if mine is None:
        print("      (skipped: no default route)")
        return
    assert _get("ping", forwarded_for=mine)["localNetwork"] is True, \
        f"{mine} is this machine's own address and must count as local"
    _ok(_get("listCastDevices", "c-own", forwarded_for=mine),
        "listCastDevices from our own subnet")


def test_every_gated_endpoint_refuses_from_elsewhere():
    for ep in GATED:
        _failed(_get(ep, "c-hotel", {"id": DEVICE_ID}, forwarded_for=ELSEWHERE),
                50, f"{ep} from {ELSEWHERE}")


def test_every_gated_endpoint_refuses_over_a_vpn():
    for ep in GATED:
        _failed(_get(ep, "c-vpn", {"id": DEVICE_ID}, forwarded_for=VPN),
                50, f"{ep} from {VPN}")


def test_stop_still_works_from_elsewhere():
    # The one exemption, and the reason for it: somebody who has walked out of
    # the house must still be able to silence it.
    _ok(_get("stopCast", "c-hotel", forwarded_for=ELSEWHERE),
        "stopCast from off the network")


def test_leaving_the_network_ends_the_session():
    ctl = "c-leaver"
    _ok(_get("startCast", ctl, {"id": DEVICE_ID}), "startCast from loopback")
    assert _get("castSession", ctl)["castSession"]["active"] is True, \
        "session should be live after startCast"

    # One refused call from off the network is enough: the owner is recognised
    # before being turned away, and the session goes with them.
    _failed(_get("castSession", ctl, forwarded_for=ELSEWHERE), 50,
            "castSession from off the network")

    assert _get("castSession", ctl)["castSession"]["active"] is False, \
        "the session should have been torn down when its owner left"


def test_a_stranger_leaving_ends_nothing():
    ctl = "c-owner"
    _ok(_get("startCast", ctl, {"id": DEVICE_ID}), "startCast from loopback")
    # A different controller is not the owner, so its refusal must not reach
    # somebody else's session -- the same rule stopCast already follows.
    _failed(_get("castSession", "c-other", forwarded_for=ELSEWHERE), 50,
            "castSession for a non-owner from off the network")
    assert _get("castSession", ctl)["castSession"]["active"] is True, \
        "a non-owner being refused must not tear down the owner's session"
    _ok(_get("stopCast", ctl), "cleanup")


TESTS = [
    test_ping_reports_locality,
    test_own_subnet_is_local,
    test_every_gated_endpoint_refuses_from_elsewhere,
    test_every_gated_endpoint_refuses_over_a_vpn,
    test_stop_still_works_from_elsewhere,
    test_leaving_the_network_ends_the_session,
    test_a_stranger_leaving_ends_nothing,
]

if __name__ == "__main__":
    failed = 0
    for t in TESTS:
        try:
            t()
            print(f"ok    {t.__name__}")
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    try:
        _get("stopCast", "c-leaver")
        _get("stopCast", "c-owner")
    except Exception:
        pass
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

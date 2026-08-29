#!/usr/bin/env python3
"""Cast session ownership tests.

A cast session belongs to one account *and* one client instance, named by the
`castController` parameter. Everything below is about the consequences of that:
a client that does not own the session must be able to play normally, must not
see the session, and must not be able to steer or stop it.

The bug these exist for: `CastManager::active()` is a process-global bool, so
before ownership the redirect in `stream.view` fired for every caller in the
server. Casting from the web client and then playing a track in the Android app
under the same login sent that track to the television instead of the phone.

**No Chromecast is needed.** `CastManager::start()` never opens a connection —
it only sets a flag and detaches the poll loop — so a configured device is
enough to drive the whole matrix. Start the server with one at an address
nothing will answer on:

    ./build/gaindrive --db /tmp/gd_test.db --artist-root music=/music \
        --cast-device fake=192.0.2.1:8009

192.0.2.0/24 is TEST-NET-1 (RFC 5737) and is not routable, so the detached load
worker fails quietly and costs one log line. Then:

    python3 tests/test_cast_session.py

The account must have castRole.
"""

import sys
import urllib.error
import urllib.parse
import urllib.request

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

# Must match --cast-device: the id of a configured device is derived from its
# address rather than random, precisely so it survives a restart.
DEVICE_ID = "manual:192.0.2.1:8009"

# Two client instances of the same account. The whole point is that these are
# distinguishable, which `c=` alone could not do.
CTRL_A = "aaaaaaaaaaaaaaaa"
CTRL_B = "bbbbbbbbbbbbbbbb"


def _url(endpoint, controller=None, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "json"}
    if controller is not None:
        p["castController"] = controller
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(p)}"


def _get(endpoint, controller=None, extra=None):
    """A Subsonic JSON call. Returns the subsonic-response object."""
    import json
    with urllib.request.urlopen(_url(endpoint, controller, extra),
                                timeout=10) as r:
        return json.loads(r.read())["subsonic-response"]


def _raw(endpoint, controller=None, extra=None):
    """(status, bytes seen) for stream.view, which answers 204 to the session
    owner and real audio to everyone else. Only the first few kB are read —
    the point is whether a body exists, and a whole FLAC is a lot of it."""
    req = urllib.request.Request(_url(endpoint, controller, extra))
    try:
        with urllib.request.urlopen(req, timeout=10) as r:
            return r.status, len(r.read(4096))
    except urllib.error.HTTPError as e:
        e.read()
        return e.code, 0


def _ok(resp, why=""):
    assert resp.get("status") == "ok", f"{why}: {resp}"


def _failed(resp, code=None, why=""):
    assert resp.get("status") == "failed", f"{why}: expected failure, got {resp}"
    if code is not None:
        got = resp.get("error", {}).get("code")
        assert got == code, f"{why}: expected error {code}, got {got}: {resp}"


def _active(controller):
    """castSession.active as seen by one controller."""
    r = _get("castSession", controller)
    _ok(r, "castSession")
    return r["castSession"]["active"]


def _stop_all():
    """Leave no session behind, whichever controller happens to own it."""
    for c in (CTRL_A, CTRL_B):
        try:
            _get("stopCast", c)
        except Exception:
            pass


def _a_song_id():
    """Any song in the library. Via getAlbumList2 rather than search3, which
    requires a non-empty query and so cannot be asked for "anything"."""
    r = _get("getAlbumList2", CTRL_A, {"type": "newest", "size": 20})
    _ok(r, "getAlbumList2")
    albums = r.get("albumList2", {}).get("album", [])
    assert albums, "No albums in the library to stream from"
    for album in albums:
        a = _get("getAlbum", CTRL_A, {"id": album["id"]})
        _ok(a, "getAlbum")
        songs = a.get("album", {}).get("song", [])
        if songs:
            return songs[0]["id"]
    raise AssertionError("No songs in the library to stream")


# ── Tests ───────────────────────────────────────────────────────────────────

def test_start_requires_controller():
    """A nameless client cannot own a session, which is what stops two installs
    of one app collapsing into a single identity."""
    _stop_all()
    r = _get("startCast", None, {"id": DEVICE_ID})
    _failed(r, 10, "startCast without castController")


def test_owner_sees_its_session():
    _stop_all()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")
    assert _active(CTRL_A) is True, "owner cannot see its own session"
    _stop_all()


def test_others_see_no_session():
    """Another controller, and a client that sends none at all, are both told
    there is nothing — otherwise a second browser adopts the session on load."""
    _stop_all()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")
    assert _active(CTRL_B) is False, "B can see A's session"
    r = _get("castSession", None)
    _ok(r, "castSession without controller")
    assert r["castSession"]["active"] is False, "a nameless client sees a session"
    _stop_all()


def test_stream_redirects_only_the_owner():
    """The reported bug, in one assertion: the phone must get audio."""
    _stop_all()
    song = _a_song_id()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")

    status, _ = _raw("stream", CTRL_A, {"id": song})
    assert status == 204, f"owner should be redirected, got {status}"

    for label, ctrl in (("another controller", CTRL_B), ("no controller", None)):
        status, n = _raw("stream", ctrl, {"id": song})
        assert status == 200, f"{label}: expected 200, got {status}"
        assert n > 0, f"{label}: expected audio, got an empty body"
    _stop_all()


def test_non_owner_cannot_steer():
    _stop_all()
    song = _a_song_id()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")
    _failed(_get("castLoad", CTRL_B, {"id": song}), 0, "castLoad as B")
    _failed(_get("castControl", CTRL_B, {"action": "pause"}), 0,
            "castControl as B")
    _failed(_get("castLoad", None, {"id": song}), 0, "castLoad with no id")
    _stop_all()


def test_non_owner_stop_is_a_no_op():
    """Succeeds without stopping anything: a client displaced by a takeover
    runs its own cleanup, and that must not kill the session that replaced it."""
    _stop_all()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")
    _ok(_get("stopCast", CTRL_B), "stopCast as B")
    assert _active(CTRL_A) is True, "B's stopCast killed A's session"
    _ok(_get("stopCast", CTRL_A), "stopCast as A")
    assert _active(CTRL_A) is False, "A's own stopCast did nothing"


def test_start_takes_over():
    """One control channel, so a second owner displaces the first."""
    _stop_all()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")
    _ok(_get("startCast", CTRL_B, {"id": DEVICE_ID}), "startCast B")
    assert _active(CTRL_B) is True, "B does not own the session it claimed"
    assert _active(CTRL_A) is False, "A still owns a session it lost"
    _stop_all()


def test_events_only_for_the_owner():
    """castEvents answers 204 to a non-owner and opens a stream for the owner.

    The owner's side is deliberately lenient: with no real receiver there is no
    status to push, so the connection may hold silent for the full 15 s
    keepalive window. A read timeout there means the stream was opened, which is
    the opposite of 204 and all this needs to establish."""
    _stop_all()
    _ok(_get("startCast", CTRL_A, {"id": DEVICE_ID}), "startCast A")

    req = urllib.request.Request(_url("castEvents", CTRL_B))
    with urllib.request.urlopen(req, timeout=10) as r:
        r.read()
        assert r.status == 204, f"B got {r.status} from castEvents"

    try:
        req = urllib.request.Request(_url("castEvents", CTRL_A))
        with urllib.request.urlopen(req, timeout=3) as r:
            assert r.status != 204, "owner was refused its own event stream"
    except TimeoutError:
        pass
    except OSError as e:
        if "timed out" not in str(e):
            raise
    _stop_all()


TESTS = [
    test_start_requires_controller,
    test_owner_sees_its_session,
    test_others_see_no_session,
    test_stream_redirects_only_the_owner,
    test_non_owner_cannot_steer,
    test_non_owner_stop_is_a_no_op,
    test_start_takes_over,
    test_events_only_for_the_owner,
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
    _stop_all()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

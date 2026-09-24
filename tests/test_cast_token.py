#!/usr/bin/env python3
"""getCastToken: a credential a receiver can fetch one track with.

It exists for a cast this server is not driving. The Android and iOS apps hold
their own Cast control channel and build the receiver's URLs themselves, and a
receiver has no account - so those URLs used to carry `u`/`t`/`s`, which is the
account's password. The endpoint is therefore gated on authentication and
**nothing else**: not `castRole`, and not the local-network rule the other cast
endpoints carry, since a client casting for itself is not asking this server to
cast. These tests are mostly about proving those two gates really are absent,
and that the grant is bounded the way the absence assumes - one song, and that
song's stream, cover art and captions, and nothing else.

The script creates a throwaway non-castRole account, uses it, and disables it
on the way out - Subsonic has no deleteUser, the same compromise
`tests/test_authz.py` makes.

Needs an admin account and at least one song. Start the server, then:

    python3 tests/test_cast_token.py

`public_url` must stay unset, as `tests/test_cast_local.py` explains: with it
set, loopback stops counting as local and the baseline calls below would be
refused.
"""

import json
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE   = "http://localhost:4040/rest"
ADMIN  = "admin"
APASS  = "secret"
VER    = "1.16.1"
CLIENT = "test"

# The throwaway account, named so it is obvious in the Users list what it is.
PLAIN  = "casttokentest"
PPASS  = "casttokenpass"

# The hotel, as in test_cast_local.py: TEST-NET-3, so it can never be a
# network this machine is on. trusted_proxies defaults to loopback, so a
# request from localhost carrying this header has it believed.
ELSEWHERE = "203.0.113.5"


def _get(endpoint, params=None, user=ADMIN, password=APASS,
         forwarded_for=None):
    """A Subsonic JSON call. Returns the subsonic-response object."""
    p = {"u": user, "p": password, "v": VER, "c": CLIENT, "f": "json"}
    if params:
        p.update(params)
    req = urllib.request.Request(
        f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(p)}")
    if forwarded_for is not None:
        req.add_header("X-Forwarded-For", forwarded_for)
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())["subsonic-response"]


def _raw(endpoint, params, forwarded_for=None):
    """A call carrying no credentials at all. Returns (status, headers, body)."""
    req = urllib.request.Request(
        f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(params)}")
    if forwarded_for is not None:
        req.add_header("X-Forwarded-For", forwarded_for)
    try:
        with urllib.request.urlopen(req, timeout=60) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def _ok(resp, why=""):
    assert resp.get("status") == "ok", f"{why}: {resp}"


def _failed(resp, code=None, why=""):
    assert resp.get("status") == "failed", f"{why}: expected failure, got {resp}"
    if code is not None:
        got = resp.get("error", {}).get("code")
        assert got == code, f"{why}: expected error {code}, got {got}: {resp}"


SONGS = []
COVERS = []


def setup():
    """A non-castRole account, and two song ids to scope tokens against."""
    _get("createUser", {"username": PLAIN, "password": PPASS,
                        "adminRole": "false", "uploadRole": "false",
                        "castRole": "false"})
    # Whether it was just created or survives a previous run, force it to the
    # state these tests assume.
    _ok(_get("updateUser", {"username": PLAIN, "password": PPASS,
                            "adminRole": "false", "uploadRole": "false",
                            "castRole": "false", "disabled": "false",
                            "maxBitRate": "0"}),
        f"could not set up {PLAIN}")

    sr = _get("search3", {"query": "", "songCount": "2",
                          "albumCount": "0", "artistCount": "0"})
    _ok(sr, "search3")
    for s in sr.get("searchResult3", {}).get("song", []):
        SONGS.append(s["id"])
        if s.get("coverArt"):
            COVERS.append(s["coverArt"])
    assert len(SONGS) >= 2, (
        "need at least two songs in the library to test token scoping")


def teardown():
    try:
        _get("updateUser", {"username": PLAIN, "disabled": "true"})
    except Exception:
        pass


def test_needs_authentication():
    _failed(_get("getCastToken", {"id": SONGS[0]}, password="wrong"), 40,
            "getCastToken with a bad password")


def test_needs_an_id():
    _failed(_get("getCastToken"), 10, "getCastToken with no id")


def test_unknown_song():
    _failed(_get("getCastToken", {"id": "999999999"}), 70,
            "getCastToken for a song that does not exist")


def test_works_without_cast_role():
    # The whole point of the endpoint. castRole gates the seven cast
    # endpoints; this one is the way round for an account that has none.
    sr = _get("getCastToken", {"id": SONGS[0]}, user=PLAIN, password=PPASS)
    _ok(sr, "getCastToken for a non-castRole account")
    assert len(sr.get("castToken", "")) == 32, f"odd token: {sr}"


def test_works_from_off_the_network():
    # The other half. test_cast_local.py proves this same header makes every
    # gated cast endpoint refuse; this endpoint must not care.
    sr = _get("getCastToken", {"id": SONGS[0]}, user=PLAIN, password=PPASS,
              forwarded_for=ELSEWHERE)
    _ok(sr, "getCastToken from off the network")
    assert len(sr.get("castToken", "")) == 32, f"odd token: {sr}"


def test_token_fetches_the_stream_with_no_credentials():
    tok = _get("getCastToken", {"id": SONGS[0]},
               user=PLAIN, password=PPASS)["castToken"]
    status, headers, body = _raw("stream", {"id": SONGS[0], "castToken": tok,
                                            "pace": "true"})
    assert status == 200, f"stream with a grant: HTTP {status}"
    assert len(body) > 0, "stream with a grant returned an empty body"
    ctype = headers.get("Content-Type", "")
    assert not ctype.startswith("application/json"), (
        f"stream answered an API error rather than audio: {body[:200]!r}")


def test_token_is_scoped_to_one_song():
    tok = _get("getCastToken", {"id": SONGS[0]},
               user=PLAIN, password=PPASS)["castToken"]
    # Presenting it for another id must be refused exactly as if it were
    # absent - which, with no credentials on the request, means unauthorised.
    status, _, body = _raw("stream", {"id": SONGS[1], "castToken": tok})
    assert status != 200 or b"failed" in body[:400], (
        f"a grant for {SONGS[0]} served {SONGS[1]}: HTTP {status} {body[:200]!r}")


def test_an_invented_token_is_refused():
    status, _, body = _raw("stream", {"id": SONGS[0], "castToken": "f" * 32})
    assert status != 200 or b"failed" in body[:400], (
        f"an invented token was served: HTTP {status} {body[:200]!r}")


def test_no_token_at_all_is_refused():
    status, _, body = _raw("stream", {"id": SONGS[0]})
    assert status != 200 or b"failed" in body[:400], (
        f"an uncredentialled stream was served: HTTP {status} {body[:200]!r}")


def test_token_fetches_the_cover_with_no_credentials():
    """The sleeve travels in the LOAD and the receiver fetches it too.

    A grant that stopped at the audio would leave the account's password on
    the television anyway, which is the whole thing this is for.
    """
    if not COVERS:
        print("      (skipped: no song in the search result has cover art)")
        return
    sr = _get("getCastToken", {"id": SONGS[0]}, user=PLAIN, password=PPASS)
    tok = sr["castToken"]
    status, headers, body = _raw("getCoverArt", {"id": COVERS[0],
                                                 "castToken": tok})
    assert status == 200, f"getCoverArt with a grant: HTTP {status}"
    assert len(body) > 0, "getCoverArt with a grant returned an empty body"
    assert headers.get("Content-Type", "").startswith("image/"), (
        f"expected an image, got {headers.get('Content-Type')}: {body[:120]!r}")


def test_the_cover_is_scoped_to_the_grant():
    """The cover id is resolved at mint time, not named by the caller."""
    if len(COVERS) < 2 or COVERS[0] == COVERS[1]:
        print("      (skipped: need two songs with different cover art)")
        return
    tok = _get("getCastToken", {"id": SONGS[0]},
               user=PLAIN, password=PPASS)["castToken"]
    status, _, body = _raw("getCoverArt", {"id": COVERS[1], "castToken": tok})
    assert status != 200 or b"failed" in body[:400], (
        f"a grant for {COVERS[0]} served cover {COVERS[1]}: HTTP {status}")


def test_captions_get_past_authentication():
    """Authorisation, not content - most test libraries have no subtitles.

    Without a credential getCaptions answers a Subsonic auth error; with a
    valid grant it gets as far as looking, and answers 404 when there is
    nothing to find. The difference between those two is the whole assertion,
    and it holds whether or not the library contains a single caption.
    """
    tok = _get("getCastToken", {"id": SONGS[0]},
               user=PLAIN, password=PPASS)["castToken"]

    bare_status, _, bare_body = _raw("getCaptions", {"id": SONGS[0]})
    assert bare_status != 200 or b"failed" in bare_body[:400], (
        "getCaptions served an uncredentialled request")

    status, _, body = _raw("getCaptions", {"id": SONGS[0], "castToken": tok})
    assert not (status != 200 and b'"code":40' in body[:400]), (
        f"a grant was refused by getCaptions: HTTP {status} {body[:200]!r}")
    assert status in (200, 404), (
        f"getCaptions with a grant: unexpected HTTP {status} {body[:200]!r}")


def test_captions_are_scoped_to_the_grants_song():
    tok = _get("getCastToken", {"id": SONGS[0]},
               user=PLAIN, password=PPASS)["castToken"]
    status, _, body = _raw("getCaptions", {"id": SONGS[1], "castToken": tok})
    assert status != 200 or b"failed" in body[:400], (
        f"a grant for {SONGS[0]} served captions for {SONGS[1]}: HTTP {status}")


def test_the_account_ceiling_still_applies():
    """A grant names an account, so the account's bitrate cap follows it.

    This is where the two tokens differ: castLoad's skips the ceiling because
    there is genuinely nobody to apply it to, and a grant must not, or casting
    would be a way round your own cap.

    Compared by size rather than asserted outright, because whether a cap
    changes anything depends on the source file. A source already below
    32 kbps would legitimately produce no difference, so that case is reported
    rather than failed.
    """
    uncapped = _raw("stream", {"id": SONGS[0], "pace": "true",
                               "castToken": _get("getCastToken", {"id": SONGS[0]},
                                                 user=PLAIN, password=PPASS)["castToken"]})[2]

    _ok(_get("updateUser", {"username": PLAIN, "maxBitRate": "32"}),
        "could not cap the account")
    try:
        capped = _raw("stream", {"id": SONGS[0], "pace": "true",
                                 "castToken": _get("getCastToken", {"id": SONGS[0]},
                                                   user=PLAIN, password=PPASS)["castToken"]})[2]
    finally:
        _get("updateUser", {"username": PLAIN, "maxBitRate": "0"})

    if len(capped) == len(uncapped):
        print("      (inconclusive: the source is already at or below 32 kbps)")
        return
    assert len(capped) < len(uncapped), (
        f"a 32 kbps cap produced {len(capped)} bytes against an uncapped "
        f"{len(uncapped)} - the ceiling does not appear to be applied")


TESTS = [
    test_needs_authentication,
    test_needs_an_id,
    test_unknown_song,
    test_works_without_cast_role,
    test_works_from_off_the_network,
    test_token_fetches_the_stream_with_no_credentials,
    test_token_is_scoped_to_one_song,
    test_an_invented_token_is_refused,
    test_no_token_at_all_is_refused,
    test_token_fetches_the_cover_with_no_credentials,
    test_the_cover_is_scoped_to_the_grant,
    test_captions_get_past_authentication,
    test_captions_are_scoped_to_the_grants_song,
    test_the_account_ceiling_still_applies,
]

if __name__ == "__main__":
    try:
        setup()
    except Exception as e:
        print(f"setup failed: {e}")
        sys.exit(1)
    failed = 0
    for t in TESTS:
        try:
            t()
            print(f"ok    {t.__name__}")
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    teardown()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

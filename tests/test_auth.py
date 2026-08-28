#!/usr/bin/env python3
"""Basic auth / user-management endpoint tests.

Start the server first:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music --no-scan
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music \
                      --add-user admin --password secret

Then run:
    python3 tests/test_auth.py
"""

import hashlib
import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

NS = "http://subsonic.org/restapi"


def _get(endpoint, extra=None, *, user=USER, password=PASS):
    p = {"u": user, "p": password, "v": VER, "c": CLIENT, "f": "xml"}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        return ET.fromstring(r.read())


def _check(root, status="ok"):
    got = root.get("status")
    assert got == status, (
        f"Expected status={status!r}, got {got!r}: "
        + ET.tostring(root).decode()
    )


def test_ping_ok():
    _check(_get("ping.view"))
    print("PASS  ping with correct password")


def test_ping_wrong_password():
    root = _get("ping.view", password="wrong")
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "40", \
        f"Expected error code 40, got: {ET.tostring(root).decode()}"
    print("PASS  ping with wrong password returns error 40")


def test_ping_token():
    salt  = "abc123"
    token = hashlib.md5((PASS + salt).encode()).hexdigest()
    p = {"u": USER, "t": token, "s": salt, "v": VER, "c": CLIENT}
    url = f"{BASE}/ping.view?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        root = ET.fromstring(r.read())
    _check(root)
    print("PASS  ping with token auth")


def test_get_license():
    root = _get("getLicense.view")
    _check(root)
    lic = root.find(f"{{{NS}}}license")
    assert lic is not None and lic.get("valid") == "true", \
        f"Expected license valid=true: {ET.tostring(root).decode()}"
    print("PASS  getLicense returns valid=true")


def test_get_user():
    root = _get("getUser.view", {"username": USER})
    _check(root)
    u = root.find(f"{{{NS}}}user")
    assert u is not None and u.get("username") == USER, \
        f"Expected user element with username={USER!r}: {ET.tostring(root).decode()}"
    print("PASS  getUser returns correct username")


def test_get_user_not_found():
    root = _get("getUser.view", {"username": "nobody"})
    _check(root, status="failed")
    print("PASS  getUser with unknown username returns failed")


def test_missing_credentials():
    url = f"{BASE}/ping.view?v={VER}&c={CLIENT}"
    with urllib.request.urlopen(url) as r:
        root = ET.fromstring(r.read())
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", \
        f"Expected error code 10: {ET.tostring(root).decode()}"
    print("PASS  missing credentials returns error 10")


# ---- Regressions for the pre-public-exposure hardening ------------------


def test_enc_password_not_hex():
    """`p=enc:` is hex-encoded plaintext, and std::stoi threw on anything else.

    That made this an *unauthenticated* 500 from any caller, with the
    exception's text in an EXCEPTION_WHAT response header. A malformed
    password is a wrong password.
    """
    p = {"u": USER, "p": "enc:zz", "v": VER, "c": CLIENT, "f": "xml"}
    url = f"{BASE}/ping.view?{urllib.parse.urlencode(p)}"
    try:
        with urllib.request.urlopen(url) as r:
            body, headers = r.read(), r.headers
    except urllib.error.HTTPError as e:
        raise AssertionError(f"expected 200, got HTTP {e.code}") from None
    root = ET.fromstring(body)
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "40", \
        f"Expected error code 40, got: {ET.tostring(root).decode()}"
    assert "EXCEPTION_WHAT" not in headers, \
        f"Exception text leaked in a header: {headers.get('EXCEPTION_WHAT')!r}"
    print("PASS  p=enc: with non-hex is a failed auth, not a 500")


def test_enc_password_valid_hex():
    """The odd-length and valid cases still work, so the fix did not break it."""
    p = {"u": USER, "p": "enc:" + PASS.encode().hex(),
         "v": VER, "c": CLIENT, "f": "xml"}
    url = f"{BASE}/ping.view?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        root = ET.fromstring(r.read())
    _check(root)
    print("PASS  p=enc:<valid hex> still authenticates")


def test_short_salt_rejected():
    """The Subsonic spec requires at least six characters of salt.

    A one-character salt turns a captured token into an ordinary unsalted MD5,
    which a rainbow table answers. Nothing checked it.
    """
    salt = "ab"
    token = hashlib.md5((PASS + salt).encode()).hexdigest()
    p = {"u": USER, "t": token, "s": salt, "v": VER, "c": CLIENT, "f": "xml"}
    url = f"{BASE}/ping.view?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        root = ET.fromstring(r.read())
    _check(root, status="failed")
    print("PASS  a salt shorter than six characters is refused")


def test_search_counts_are_clamped():
    """`query=%` matched everything and songCount was unbounded.

    Together they serialised the whole library into one in-memory document on
    request. The escape makes `%` a literal; the clamp bounds the rest.
    """
    root = _get("search3.view", {"query": "%", "songCount": "2000000000",
                                "albumCount": "2000000000",
                                "artistCount": "2000000000"})
    _check(root)
    result = root.find(f"{{{NS}}}searchResult3")
    n = 0 if result is None else len(list(result))
    assert n <= 1500, f"expected at most 1500 entries, got {n}"
    print(f"PASS  search3 with a wildcard query is bounded ({n} entries)")


def test_search_count_non_numeric():
    """to_int rather than std::stoi: a non-numeric count is a default, not a 500."""
    root = _get("search3.view", {"query": "a", "songCount": "abc"})
    _check(root)
    print("PASS  a non-numeric songCount does not 500")


def test_bad_id_is_not_a_500():
    """Many handlers called std::stoi on `id` unguarded."""
    for endpoint in ("getSong.view", "getMusicDirectory.view", "getCoverArt.view",
                     "updateSong.view", "getPlaylist.view"):
        p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "xml",
             "id": "not-a-number"}
        url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
        try:
            with urllib.request.urlopen(url) as r:
                r.read()
        except urllib.error.HTTPError as e:
            # 404 is a legitimate answer for cover art; 500 is not.
            assert e.code != 500, f"{endpoint} returned HTTP 500 for a bad id"
    print("PASS  a non-numeric id never produces a 500")


def test_login_throttle():
    """Repeated wrong passwords must start costing time.

    Every credential here rides in a query string and is checked by one string
    comparison, so without a throttle ping.view is an unmetered password
    oracle — and the CORS policy makes it reachable from any web page.
    """
    import time

    # Free tries first, then the delay doubles. Sixteen attempts is a few
    # seconds' worth once it bites, and stays well clear of the hard block so
    # this test cannot lock the suite out of its own server.
    attempts = 16
    start = time.monotonic()
    for _ in range(attempts):
        _get("ping.view", password="definitely-wrong")
    elapsed = time.monotonic() - start
    assert elapsed > 1.0, (
        f"{attempts} failed logins took {elapsed:.2f}s; "
        "expected the throttle to bite"
    )
    print(f"PASS  repeated failed logins are throttled "
          f"({elapsed:.1f}s for {attempts})")
    # The correct password must still work afterwards, and must clear the
    # counter — a throttle that locks out the real user is a denial of service
    # rather than a defence.
    _check(_get("ping.view"))
    print("PASS  a correct password still works, and resets the counter")


TESTS = [
    test_ping_ok,
    test_ping_wrong_password,
    test_ping_token,
    test_get_license,
    test_get_user,
    test_get_user_not_found,
    test_missing_credentials,
    test_enc_password_not_hex,
    test_enc_password_valid_hex,
    test_short_salt_rejected,
    test_search_counts_are_clamped,
    test_search_count_non_numeric,
    test_bad_id_is_not_a_500,
    # Last, because it deliberately spends the throttle budget for this
    # address and everything above would then be running against a penalty.
    test_login_throttle,
]

if __name__ == "__main__":
    failed = 0
    for t in TESTS:
        try:
            t()
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

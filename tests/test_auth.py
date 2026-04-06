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


TESTS = [
    test_ping_ok,
    test_ping_wrong_password,
    test_ping_token,
    test_get_license,
    test_get_user,
    test_get_user_not_found,
    test_missing_credentials,
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

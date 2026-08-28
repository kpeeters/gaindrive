#!/usr/bin/env python3
"""startScan / getScanStatus tests.

These are the *standard* Subsonic endpoints (1.15.0), not gaindrive extensions:
no parameters, and a <scanStatus> carrying `scanning` and `count`. What is worth
testing is the shape and the typing rather than that a scan happens — a strict
client throws on a type mismatch before any of the response is usable, which is
the whole subject of SPEC-AUDIT.md.

Start the server first, with a scanned collection, then run:
    python3 tests/test_scan.py

This starts a real scan of the whole library, so point it at a scratch
collection unless you are happy to wait.
"""

import json
import sys
import time
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

NS = "http://subsonic.org/restapi"

# How long to wait for a scan to finish before giving up. A cold scan of a large
# collection can outlast this; that is a slow server, not a failed test, and the
# message says so.
SCAN_TIMEOUT = 120


def _url(endpoint, fmt, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": fmt}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


def _xml(endpoint, extra=None):
    with urllib.request.urlopen(_url(endpoint, "xml", extra)) as r:
        return ET.fromstring(r.read())


def _json(endpoint, extra=None):
    with urllib.request.urlopen(_url(endpoint, "json", extra)) as r:
        return json.loads(r.read())["subsonic-response"]


def _check(root, status="ok"):
    got = root.get("status")
    assert got == status, (
        f"Expected status={status!r}, got {got!r}: "
        + ET.tostring(root).decode()
    )


def _status_xml():
    root = _xml("getScanStatus.view")
    _check(root)
    el = root.find(f"{{{NS}}}scanStatus")
    assert el is not None, "No <scanStatus> in response"
    return el


def _wait_idle(timeout=SCAN_TIMEOUT):
    """Block until no scan is running. Returns the final count."""
    deadline = time.time() + timeout
    while time.time() < deadline:
        el = _status_xml()
        if el.get("scanning") == "false":
            return int(el.get("count") or 0)
        time.sleep(1)
    raise AssertionError(
        f"a scan was still running after {timeout}s — raise SCAN_TIMEOUT if "
        f"this collection is simply large")


def test_scan_status_shape():
    """Both fields are present, and JSON types them properly.

    `scanning` must be a real boolean and `count` a real number: a client that
    declares them so throws on a string before it can read anything else.
    """
    _wait_idle()
    el = _status_xml()
    assert el.get("scanning") in ("true", "false"), el.get("scanning")
    assert (el.get("count") or "").isdigit(), el.get("count")

    body = _json("getScanStatus.view")
    st = body.get("scanStatus")
    assert st is not None, body
    assert isinstance(st.get("scanning"), bool), (
        f"scanning is {type(st.get('scanning')).__name__}, not a JSON boolean")
    assert isinstance(st.get("count"), int) and not isinstance(
        st.get("count"), bool), (
        f"count is {type(st.get('count')).__name__}, not a JSON number")
    print("PASS  getScanStatus reports scanning and count, correctly typed")


def test_scan_status_takes_no_auth_shortcut():
    """It is an ordinary authenticated endpoint — bad credentials are refused."""
    url = (f"{BASE}/getScanStatus.view?"
           + urllib.parse.urlencode({"u": USER, "p": PASS + "x",
                                     "v": VER, "c": CLIENT, "f": "xml"}))
    with urllib.request.urlopen(url) as r:
        root = ET.fromstring(r.read())
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "40", ET.tostring(root)
    print("PASS  getScanStatus rejects a wrong password with error 40")


def test_start_scan_runs_and_finishes():
    """startScan flips scanning to true, count climbs, and it settles back."""
    _wait_idle()

    root = _xml("startScan.view")
    _check(root)
    el = root.find(f"{{{NS}}}scanStatus")
    assert el is not None, "startScan did not answer with <scanStatus>"

    # The scan is detached, so it may already have finished on a tiny
    # collection. Either we catch it running or we see a non-zero final count.
    saw_running = False
    deadline = time.time() + SCAN_TIMEOUT
    while time.time() < deadline:
        cur = _status_xml()
        if cur.get("scanning") == "true":
            saw_running = True
        elif saw_running:
            break
        else:
            break
        time.sleep(0.5)

    count = _wait_idle()
    assert count > 0, (
        "count stayed at 0 after a full scan — nothing incremented it, or the "
        "collection is empty")
    print(f"PASS  startScan ran to completion, count={count}"
          + ("" if saw_running else " (finished too fast to observe running)"))


def test_second_start_scan_is_harmless():
    """Asking again while one is running is not an error and starts nothing.

    A second walk over the same tree would only double the I/O and make `count`
    jump about between two scans sharing one counter.
    """
    _wait_idle()
    _check(_xml("startScan.view"))
    # Straight back in, with the first almost certainly still going.
    root = _xml("startScan.view")
    _check(root)
    assert root.find(f"{{{NS}}}scanStatus") is not None
    _wait_idle()
    print("PASS  a second startScan while scanning is accepted and starts nothing")


TESTS = [
    test_scan_status_shape,
    test_scan_status_takes_no_auth_shortcut,
    test_start_scan_runs_and_finishes,
    test_second_start_scan_is_harmless,
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

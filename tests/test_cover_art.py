#!/usr/bin/env python3
"""Cover art scaling and the thumbnail cache.

getCoverArt is asked for a pixel size by every client gaindrive has, and each
of those requests used to fork ffmpeg and decode the full-size source. Scaling
now happens in process and the result is stored in the music DB, so what these
tests check is that the answer is a real image *of the size that was asked
for* — which nothing in tests/ checked before, because measuring pixels needs
a few lines of parser and asserting "some bytes came back" does not.

Start the server first and let the startup scan finish:
    ./build/gaindrive --db /tmp/gd_test.db --artist-root music=/music

Then run:
    python3 tests/test_cover_art.py

Tests that need material the library may not have — a PNG cover, a video with
art, an artist with a portrait — skip rather than fail. Everything else runs
against any collection.
"""

import http.client
import json
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE   = "http://localhost:4040/rest"
HOST   = "localhost"
PORT   = 4040
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"


class Skip(Exception):
    """Not a failure: there is simply nothing of this kind to test."""


def _query(extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    return urllib.parse.urlencode(p)


def _url(endpoint, extra=None):
    return f"{BASE}/{endpoint}?{_query(extra)}"


def _raw(endpoint, extra=None, headers=None):
    """Returns (status, headers, body). Does not raise on 4xx/5xx."""
    req = urllib.request.Request(_url(endpoint, extra))
    for k, v in (headers or {}).items():
        req.add_header(k, v)
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, dict(r.headers), r.read()
    except urllib.error.HTTPError as e:
        return e.code, dict(e.headers), e.read()


def _json(endpoint, extra=None):
    extra = dict(extra or {})
    extra["f"] = "json"
    status, _, body = _raw(endpoint, extra)
    try:
        return json.loads(body)["subsonic-response"]
    except (json.JSONDecodeError, KeyError):
        raise AssertionError(
            f"{endpoint} returned HTTP {status} with a non-JSON body: "
            f"{body[:200]!r}") from None


# ---- measuring an image, without PIL -----------------------------------
#
# A dimension is the only thing that can distinguish "scaled" from "served
# whole", and the whole point of this file is to assert it.

def _jpeg_size(b):
    """(width, height) from the first SOFn marker."""
    i = 2                                        # past SOI
    while i + 3 < len(b):
        if b[i] != 0xFF:                         # resync past fill bytes
            i += 1
            continue
        m = b[i + 1]
        if m == 0xFF:
            i += 1
            continue
        if m in (0xD8, 0x01) or 0xD0 <= m <= 0xD7:
            i += 2
            continue
        if m in (0xD9, 0xDA):
            break
        seg = int.from_bytes(b[i + 2:i + 4], "big")
        # Every SOFn except DHT(C4), DAC(C8) and the RSTn range.
        if m in (0xC0, 0xC1, 0xC2, 0xC3, 0xC5, 0xC6, 0xC7,
                 0xC9, 0xCA, 0xCB, 0xCD, 0xCE, 0xCF):
            h = int.from_bytes(b[i + 5:i + 7], "big")
            w = int.from_bytes(b[i + 7:i + 9], "big")
            return w, h
        i += 2 + seg
    raise AssertionError("no SOF marker found: this is not a JPEG")


def _png_size(b):
    if b[12:16] != b"IHDR":
        raise AssertionError("no IHDR: this is not a PNG")
    return (int.from_bytes(b[16:20], "big"), int.from_bytes(b[20:24], "big"))


def _image_size(b):
    if b[:3] == b"\xff\xd8\xff":
        return _jpeg_size(b)
    if b[:8] == b"\x89PNG\r\n\x1a\n":
        return _png_size(b)
    raise AssertionError(f"not an image: starts {b[:8]!r}")


# ---- finding something to look at --------------------------------------

_CACHE = {}


def _albums():
    if "albums" not in _CACHE:
        r = _json("getAlbumList2.view", {"type": "alphabeticalByName",
                                         "size": "100"})
        _CACHE["albums"] = r.get("albumList2", {}).get("album", [])
    return _CACHE["albums"]


def _cover_id():
    """An album cover id whose source is comfortably bigger than a thumbnail."""
    if "cover" in _CACHE:
        return _CACHE["cover"]
    for a in _albums():
        cid = a.get("coverArt")
        if not cid:
            continue
        status, _, body = _raw("getCoverArt.view", {"id": cid})
        if status != 200:
            continue
        try:
            w, h = _image_size(body)
        except AssertionError:
            continue
        if max(w, h) >= 200:
            _CACHE["cover"] = (cid, w, h)
            return _CACHE["cover"]
    raise Skip("no album in the library has a cover larger than 200px")


# ---- scaling -----------------------------------------------------------

def test_scaled_cover_has_the_requested_long_edge():
    cid, sw, sh = _cover_id()
    for want in (64, 80, 144, 400):
        status, _, body = _raw("getCoverArt.view", {"id": cid, "size": want})
        assert status == 200, f"size={want} returned HTTP {status}"
        w, h = _image_size(body)
        expect = min(want, max(sw, sh))
        assert max(w, h) == expect, \
            f"size={want} on a {sw}x{sh} source gave {w}x{h}, wanted a long " \
            f"edge of {expect}"
    print("PASS  size= produces an image of that size")


def test_aspect_ratio_is_preserved():
    cid, sw, sh = _cover_id()
    _, _, body = _raw("getCoverArt.view", {"id": cid, "size": 400})
    w, h = _image_size(body)
    want, got = sw / sh, w / h
    # One pixel of rounding on the short edge is the whole tolerance needed.
    assert abs(want - got) < (1.0 / min(w, h)) + 0.01, \
        f"a {sw}x{sh} source came back {w}x{h}: ratio {got:.3f} not {want:.3f}"
    print(f"PASS  aspect ratio preserved ({sw}x{sh} -> {w}x{h})")


def test_never_upscales():
    cid, sw, sh = _cover_id()
    status, _, body = _raw("getCoverArt.view", {"id": cid, "size": 4000})
    assert status == 200, f"size=4000 returned HTTP {status}"
    w, h = _image_size(body)
    assert w <= sw and h <= sh, \
        f"a {sw}x{sh} source was enlarged to {w}x{h}"
    print("PASS  a size larger than the source does not upscale")


def test_odd_size_rounds_up_and_is_not_smaller_than_asked():
    """The ladder rounds up, so a client never gets less than it asked for."""
    cid, sw, sh = _cover_id()
    for want in (100, 137, 201):
        status, _, body = _raw("getCoverArt.view", {"id": cid, "size": want})
        assert status == 200, f"size={want} returned HTTP {status}"
        w, h = _image_size(body)
        if max(sw, sh) <= want:
            continue                     # source smaller than asked; fine
        assert max(w, h) >= want, \
            f"size={want} came back {w}x{h}, smaller than requested"
    print("PASS  an off-ladder size rounds up, never down")


# ---- the cache ---------------------------------------------------------

def test_repeat_request_is_byte_identical():
    """The cheapest possible proof that the second request was a cache hit.

    A re-encode is not bit-deterministic across runs of a scaler that is
    handed different buffers, and a cache that re-encodes would also break
    every ETag it has handed out.
    """
    cid, _, _ = _cover_id()
    _, h1, b1 = _raw("getCoverArt.view", {"id": cid, "size": 80})
    _, h2, b2 = _raw("getCoverArt.view", {"id": cid, "size": 80})
    assert b1 == b2, "two identical requests returned different bytes"
    assert h1.get("ETag") == h2.get("ETag"), "the ETag moved between requests"
    print(f"PASS  a repeat request is byte-identical ({len(b1)} bytes)")


def test_sizes_are_independent():
    cid, sw, sh = _cover_id()
    _, h1, b1 = _raw("getCoverArt.view", {"id": cid, "size": 80})
    _, h2, b2 = _raw("getCoverArt.view", {"id": cid, "size": 400})
    assert _image_size(b1) != _image_size(b2), \
        "size=80 and size=400 returned the same dimensions"
    assert h1.get("ETag") != h2.get("ETag"), \
        "two sizes share an ETag; a client would cache one as the other"
    print("PASS  different sizes are cached separately")


def test_scaled_response_revalidates():
    """The 304 path, *with* a size — the existing suite only tests it without."""
    cid, _, _ = _cover_id()
    status, hdrs, _ = _raw("getCoverArt.view", {"id": cid, "size": 80})
    assert status == 200
    assert "no-cache" in hdrs.get("Cache-Control", ""), \
        "a scaled cover must be revalidated: folders.id moves across a rescan"
    etag = hdrs.get("ETag")
    assert etag, "no ETag on a scaled cover"
    status, _, body = _raw("getCoverArt.view", {"id": cid, "size": 80},
                           {"If-None-Match": etag})
    assert status == 304, f"If-None-Match returned HTTP {status}, not 304"
    assert not body, "a 304 must have no body"
    print("PASS  a scaled cover revalidates to 304")


# ---- Content-Type ------------------------------------------------------

def test_full_size_content_type_matches_the_bytes():
    """It used to say image/jpeg whatever was on disk."""
    seen_png = False
    for a in _albums():
        cid = a.get("coverArt")
        if not cid:
            continue
        status, hdrs, body = _raw("getCoverArt.view", {"id": cid})
        if status != 200 or not body:
            continue
        ct = hdrs.get("Content-Type", "")
        if body[:8] == b"\x89PNG\r\n\x1a\n":
            assert ct.startswith("image/png"), \
                f"a PNG cover was served as {ct!r}"
            seen_png = True
        elif body[:3] == b"\xff\xd8\xff":
            assert ct.startswith("image/jpeg"), \
                f"a JPEG cover was served as {ct!r}"
    if not seen_png:
        raise Skip("no PNG cover in the library — the JPEG half still ran")
    print("PASS  full-size Content-Type follows the magic bytes")


# ---- video art ---------------------------------------------------------

def test_video_art_honours_size():
    r = _json("getVideos.view")
    vids = [v for v in r.get("videos", {}).get("video", []) if v.get("coverArt")]
    if not vids:
        raise Skip("no video in the library has cover art")
    cid = vids[0]["coverArt"]
    status, _, full = _raw("getCoverArt.view", {"id": cid})
    if status != 200:
        raise Skip(f"video cover id {cid} returned HTTP {status}")
    fw, fh = _image_size(full)
    status, _, small = _raw("getCoverArt.view", {"id": cid, "size": 80})
    assert status == 200, f"video cover at size=80 returned HTTP {status}"
    w, h = _image_size(small)
    expect = min(80, max(fw, fh))
    assert max(w, h) == expect, \
        f"a {fw}x{fh} video poster came back {w}x{h} at size=80 — video art " \
        f"used to be served at its stored size whatever was asked"
    print(f"PASS  video art honours size ({fw}x{fh} -> {w}x{h})")


# ---- artist portraits --------------------------------------------------

def test_artist_portrait_is_pending_or_present():
    r = _json("getArtists.view")
    artists = [a for idx in r.get("artists", {}).get("index", [])
               for a in idx.get("artist", [])]
    if not artists:
        raise Skip("no artists in the library")
    for a in artists[:5]:
        status, hdrs, body = _raw("getCoverArt.view",
                                  {"id": a["id"], "size": 288})
        if status == 200:
            _image_size(body)            # raises if it is not an image
            print(f"PASS  {a['name']!r} has a portrait")
            return
        assert status == 404, \
            f"an artist portrait returned HTTP {status}, not 200 or 404"
        cc = hdrs.get("Cache-Control", "")
        assert "no-store" in cc or "max-age" in cc, \
            f"a portrait 404 carried Cache-Control {cc!r}: a pending one must " \
            f"be no-store, or a client caches the miss and never re-asks"
    raise Skip("no artist among the first five has a portrait yet")


# ---- framing -----------------------------------------------------------

def test_pipelined_requests_are_not_off_by_one():
    """The regression that made every thumbnail the previous album's.

    urllib opens a fresh connection per request, so the existing suite cannot
    see this at all: it only appears when several responses share one
    keep-alive connection and one of them fails to say where its body ends.
    """
    wanted = []
    for a in _albums():
        cid = a.get("coverArt")
        if cid:
            for size in (64, 80, 144, 400):
                wanted.append((cid, size))
        if len(wanted) >= 20:
            break
    if len(wanted) < 4:
        raise Skip("not enough covers in the library to pipeline")

    conn = http.client.HTTPConnection(HOST, PORT, timeout=30)
    try:
        for cid, size in wanted:
            conn.request("GET", f"/rest/getCoverArt.view?"
                                f"{_query({'id': cid, 'size': size})}")
            r = conn.getresponse()
            body = r.read()
            assert r.status == 200, \
                f"id={cid} size={size} returned HTTP {r.status} on a reused " \
                f"connection"
            w, h = _image_size(body)
            assert max(w, h) <= size, \
                f"id={cid} size={size} came back {w}x{h} — this response " \
                f"belongs to a different request"
    finally:
        conn.close()
    print(f"PASS  {len(wanted)} responses on one connection each match their "
          f"request")


# ---- negative ----------------------------------------------------------

def test_unknown_id_is_not_a_server_error():
    status, _, _ = _raw("getCoverArt.view", {"id": "1999999999"})
    assert status == 404, f"an unknown id returned HTTP {status}, not 404"
    print("PASS  an unknown cover id 404s")


TESTS = [
    test_scaled_cover_has_the_requested_long_edge,
    test_aspect_ratio_is_preserved,
    test_never_upscales,
    test_odd_size_rounds_up_and_is_not_smaller_than_asked,
    test_repeat_request_is_byte_identical,
    test_sizes_are_independent,
    test_scaled_response_revalidates,
    test_full_size_content_type_matches_the_bytes,
    test_video_art_honours_size,
    test_artist_portrait_is_pending_or_present,
    test_pipelined_requests_are_not_off_by_one,
    test_unknown_id_is_not_a_server_error,
]

if __name__ == "__main__":
    failed = skipped = 0
    for t in TESTS:
        try:
            t()
        except Skip as e:
            print(f"SKIP  {t.__name__}: {e}")
            skipped += 1
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    passed = len(TESTS) - failed - skipped
    print(f"\n{passed}/{len(TESTS)} passed"
          + (f", {skipped} skipped" if skipped else ""))
    sys.exit(failed)

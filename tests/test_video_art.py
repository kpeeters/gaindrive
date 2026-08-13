#!/usr/bin/env python3
"""Cover art manufactured from video files.

Video containers carry no tag anything writes, so gaindrive derives a cover
from the file itself — an embedded cover image, or failing that a frame — and
caches it in the music DB. A song or album whose art came from there has its
cover_path set to the *media file's* own path, so what these tests really check
is that such an id round-trips through getCoverArt as an image rather than
404ing or handing back a slice of the video.

Start the server first, against a collection containing video, and let the
startup scan finish:
    ./build/gaindrive --db /tmp/gd_test.db --category-root video=/videos

Then run:
    python3 tests/test_video_art.py

Everything here **skips** when no video in the library has art, rather than
failing. That is a legitimate state, and currently the common one: only the
embedded-cover tier is on by default, and most collections have no embedded
covers. Start the server with --video-art-frames to exercise these properly.
"""

import json
import sys
import urllib.error
import urllib.parse
import urllib.request

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

IMAGE_TYPES = ("image/jpeg", "image/png")


def _url(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


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


_CACHE = {}


def _videos():
    if "videos" not in _CACHE:
        r = _json("getVideos.view")
        _CACHE["videos"] = r.get("videos", {}).get("video", [])
    return _CACHE["videos"]


def _need_video():
    assert _videos(), \
        "no videos in the library — scan a collection with video first"


def _with_art():
    return [v for v in _videos() if v.get("coverArt")]


class Skip(Exception):
    """Not a failure: there is simply nothing of this kind to test."""


def _need_art():
    _need_video()
    if not _with_art():
        raise Skip("no video in the library has cover art — only the embedded "
                   "tier is on by default; try --video-art-frames")


# The magic bytes rather than the declared type: the point of the whole
# exercise is that the response is a real image, and a Content-Type header
# costs nothing to get right while serving the wrong bytes.
def _looks_like_an_image(body):
    return body[:3] == b"\xff\xd8\xff" or body[:8] == b"\x89PNG\r\n\x1a\n"


# ---- ids resolve ------------------------------------------------------

def test_videos_carry_cover_art_ids():
    _need_art()
    got = _with_art()
    print(f"PASS  {len(got)}/{len(_videos())} videos carry a coverArt id")


def test_cover_art_ids_serve_images():
    _need_art()
    checked = 0
    for v in _with_art():
        status, hdrs, body = _raw("getCoverArt.view", {"id": v["coverArt"]})
        assert status == 200, \
            f"{v.get('title')}: coverArt {v['coverArt']} returned HTTP {status}"
        ctype = hdrs.get("Content-Type", "")
        assert ctype in IMAGE_TYPES, f"{v.get('title')}: Content-Type {ctype!r}"
        assert _looks_like_an_image(body), \
            f"{v.get('title')}: {len(body)} bytes that are not JPEG or PNG"
        checked += 1
    print(f"PASS  {checked} video cover ids served real images")


def test_cover_art_is_revalidated_not_cached_blindly():
    """folders.id is a rowid and moves across a rescan, so a cover URL must
    carry a validator or a browser keeps showing the previous film's frame."""
    _need_art()
    v = _with_art()[0]
    status, hdrs, _ = _raw("getCoverArt.view", {"id": v["coverArt"]})
    assert status == 200
    assert "no-cache" in hdrs.get("Cache-Control", ""), \
        f"Cache-Control is {hdrs.get('Cache-Control')!r}"
    etag = hdrs.get("ETag")
    assert etag, "no ETag on manufactured cover art"

    status, _, body = _raw("getCoverArt.view", {"id": v["coverArt"]},
                           headers={"If-None-Match": etag})
    assert status == 304, f"revalidation returned HTTP {status}, not 304"
    assert not body, "304 must not carry a body"
    print("PASS  manufactured cover art revalidates (ETag + 304)")


def test_size_parameter_is_accepted():
    """The stored image is served at its stored size whatever `size` says —
    what must not happen is a 500 or an empty body."""
    _need_art()
    got = _with_art()
    for size in ("64", "200", "400"):
        status, _, body = _raw("getCoverArt.view",
                               {"id": got[0]["coverArt"], "size": size})
        assert status == 200, f"size={size} returned HTTP {status}"
        assert _looks_like_an_image(body), f"size={size} returned no image"
    print("PASS  size= is accepted on manufactured cover art")


# ---- the album side ---------------------------------------------------

def test_album_holding_a_video_has_a_cover():
    """A single-video album folder with no image of its own takes the video's
    art, which is what makes the browse grid fill in rather than showing a
    wall of placeholders."""
    _need_art()
    seen = 0
    for v in _with_art():
        album_id = v.get("albumId") or v.get("parent")
        if not album_id:
            continue
        r = _json("getAlbum.view", {"id": album_id})
        album = r.get("album")
        if not album:
            continue
        seen += 1
        assert album.get("coverArt"), \
            f"album {album.get('name')!r} holds video but reports no coverArt"
        status, _, body = _raw("getCoverArt.view", {"id": album["coverArt"]})
        assert status == 200 and _looks_like_an_image(body), \
            f"album {album.get('name')!r}: coverArt returned HTTP {status}"
        if seen >= 5:      # a sample is enough; this walks the library
            break
    assert seen, "no album could be resolved from a video entry"
    print(f"PASS  {seen} albums holding video resolve a cover")


# ---- negative ---------------------------------------------------------

def test_unknown_cover_id_is_not_a_server_error():
    status, _, _ = _raw("getCoverArt.view", {"id": "1999999999"})
    assert status in (404, 200), f"unknown cover id returned HTTP {status}"
    assert status == 404, "an id with no art must 404, not serve something"
    print("PASS  an unknown song-cover id 404s")


TESTS = [
    test_videos_carry_cover_art_ids,
    test_cover_art_ids_serve_images,
    test_cover_art_is_revalidated_not_cached_blindly,
    test_size_parameter_is_accepted,
    test_album_holding_a_video_has_a_cover,
    test_unknown_cover_id_is_not_a_server_error,
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

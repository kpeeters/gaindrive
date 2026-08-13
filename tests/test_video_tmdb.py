#!/usr/bin/env python3
"""Posters and descriptions fetched from TMDB.

A video's filename is parsed into a title and a year, TMDB is asked whether
there is such a film, and a match supplies the poster, the plot and the
canonical title. What these tests check is that a match arrives through the
channels that already existed — the ordinary `coverArt` id, and the `notes`
field of `getAlbumInfo2` where an album's liner notes go — because that is what
let this land without an API change or a client change.

Needs a server with a TMDB key configured (Settings → Server) and a scan that
has run since. Everything **skips** rather than fails when no video turns out
to be matched, since that is a legitimate state: no key, no network, or a
library of home videos that nothing would identify.

    python3 tests/test_video_tmdb.py
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


def _url(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


def _raw(endpoint, extra=None):
    req = urllib.request.Request(_url(endpoint, extra))
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


class Skip(Exception):
    """Not a failure: there is nothing of this kind to test."""


_CACHE = {}


def _videos():
    if "videos" not in _CACHE:
        r = _json("getVideos.view")
        _CACHE["videos"] = r.get("videos", {}).get("video", [])
    return _CACHE["videos"]


def _albums_with_video():
    """Album ids that hold at least one video, with one video entry each."""
    if "albums" not in _CACHE:
        seen = {}
        for v in _videos():
            aid = v.get("albumId") or v.get("parent")
            if aid and aid not in seen:
                seen[aid] = v
        _CACHE["albums"] = seen
    return _CACHE["albums"]


def _described():
    """Albums holding video whose getAlbumInfo2 carries notes — i.e. matched."""
    if "described" not in _CACHE:
        out = []
        for aid in list(_albums_with_video())[:40]:
            r = _json("getAlbumInfo2.view", {"id": aid})
            notes = (r.get("albumInfo2") or {}).get("notes", "")
            if notes:
                out.append((aid, notes))
        _CACHE["described"] = out
    return _CACHE["described"]


def _need_key():
    r = _json("getServerSettings.view")
    if not (r.get("serverSettings") or {}).get("tmdbKey"):
        raise Skip("no TMDB key configured — Settings → Server")


def _need_match():
    _need_key()
    if not _videos():
        raise Skip("no videos in the library")
    if not _described():
        raise Skip("no video album has a description — nothing matched yet; "
                   "check the scan log for 'tmdb:' lines")


def _looks_like_an_image(body):
    return body[:3] == b"\xff\xd8\xff" or body[:8] == b"\x89PNG\r\n\x1a\n"


# ---- the description ---------------------------------------------------

def test_matched_album_has_notes():
    """A film's plot lands in the album-notes field, which is the whole reason
    this needed no new endpoint."""
    _need_match()
    for aid, notes in _described():
        assert len(notes) > 20, f"album {aid}: notes are only {notes!r}"
    print(f"PASS  {len(_described())} video albums carry a description")


# ---- the poster --------------------------------------------------------

def test_matched_album_serves_a_poster():
    _need_match()
    checked = 0
    for aid, _ in _described():
        r = _json("getAlbum.view", {"id": aid})
        album = r.get("album") or {}
        cover = album.get("coverArt")
        if not cover:
            continue
        status, hdrs, body = _raw("getCoverArt.view", {"id": cover})
        assert status == 200, \
            f"album {album.get('name')!r}: coverArt returned HTTP {status}"
        assert _looks_like_an_image(body), \
            f"album {album.get('name')!r}: {len(body)} bytes, not an image"
        assert hdrs.get("Content-Type", "").startswith("image/"), \
            f"album {album.get('name')!r}: {hdrs.get('Content-Type')!r}"
        checked += 1
    assert checked, "no described album reported a coverArt id"
    print(f"PASS  {checked} matched albums serve a poster")


# ---- the title ---------------------------------------------------------

def test_matched_titles_are_not_release_names():
    """A matched album takes TMDB's title, so nothing that survived should
    still be carrying release junk."""
    _need_match()
    JUNK = ("1080p", "720p", "2160p", "bluray", "webrip", "x264", "x265",
            "hevc", "xvid", "dvdrip", "hdtv")
    for aid, _ in _described():
        r = _json("getAlbum.view", {"id": aid})
        name = (r.get("album") or {}).get("name", "")
        low  = name.lower()
        for j in JUNK:
            assert j not in low, f"album {name!r} still contains {j!r}"
    print(f"PASS  {len(_described())} matched titles are clean")


# ---- the settings round trip -------------------------------------------

def test_saving_one_setting_keeps_the_other():
    """saveServerSettings writes only what it is given. With one field that
    distinction did not exist; with two, saving the TMDB key must not blank
    the Discogs token."""
    before = _json("getServerSettings.view").get("serverSettings") or {}
    token  = before.get("discogsToken", "")
    tmdb   = before.get("tmdbKey", "")
    if not token and not tmdb:
        raise Skip("neither setting is set, so there is nothing to preserve")

    _json("saveServerSettings.view", {"tmdbKey": tmdb})
    after = _json("getServerSettings.view").get("serverSettings") or {}
    assert after.get("discogsToken", "") == token, \
        "saving the TMDB key blanked the Discogs token"
    assert after.get("tmdbKey", "") == tmdb, "the TMDB key did not survive"
    print("PASS  saving one server setting leaves the other alone")


TESTS = [
    test_matched_album_has_notes,
    test_matched_album_serves_a_poster,
    test_matched_titles_are_not_release_names,
    test_saving_one_setting_keeps_the_other,
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

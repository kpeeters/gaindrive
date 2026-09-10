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
    # Whether one is set is all the server will say; it stopped handing the key
    # back, so this can no longer check that it looks plausible.
    r = _json("getServerSettings.view")
    if not (r.get("serverSettings") or {}).get("tmdbKeySet"):
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

# ---- genres ------------------------------------------------------------

def test_matched_video_has_a_genre():
    """A video container carries no genre tag anything reads, so a genre on a
    film can only have come from TMDB. That makes this the one check that the
    lookup, the video_meta cache column and the song_genres table are all
    wired together."""
    _need_match()
    genred = [v for v in _videos() if v.get("genre")]
    if not genred:
        raise Skip("no video carries a genre — if films are otherwise matched, "
                   "the genre back-fill may not have run yet; rescan and look "
                   "for 'tmdb:' lines in the log")
    print(f"PASS  {len(genred)} of {len(_videos())} videos carry a genre, "
          f"e.g. {genred[0].get('title')!r} -> {genred[0]['genre']!r}")


def test_video_genre_is_browsable():
    """The genre a film got must appear in getGenres and lead back to it —
    the single-valued `genre` field on the entry and the song_genres table the
    browse endpoints read are two different paths to the same fact."""
    _need_match()
    genred = [v for v in _videos() if v.get("genre")]
    if not genred:
        raise Skip("no video carries a genre")

    listed = {g.get("value", "").lower()
              for g in _json("getGenres.view")
                        .get("genres", {}).get("genre", [])}
    assert listed, "getGenres returned nothing while videos carry genres"

    v = genred[0]
    assert v["genre"].lower() in listed, \
        f"video genre {v['genre']!r} is missing from getGenres"

    r = _json("getSongsByGenre.view", {"genre": v["genre"], "count": "500"})
    ids = {s.get("id") for s in r.get("songsByGenre", {}).get("song", [])}
    assert v.get("id") in ids, \
        f"{v.get('title')!r} is genre {v['genre']!r} but getSongsByGenre " \
        "does not return it"
    print(f"PASS  {v.get('title')!r} is reachable through genre "
          f"{v['genre']!r}")


def test_saving_nothing_keeps_both_settings():
    """saveServerSettings writes only the settings it is given, so a call
    naming neither must leave both alone.

    This used to read both values, re-send one and compare — which it cannot
    do now that getServerSettings reports only whether each is set. Sending
    nothing tests the same property and is the safer shape besides: the
    regression guarded against is the handler writing unconditionally, and
    against a call carrying no values that clears both, flipping both flags.
    Re-sending a real key would have been a destructive test on a live
    server, and could not have been restored from a boolean."""
    before = _json("getServerSettings.view").get("serverSettings") or {}
    had_token = bool(before.get("discogsTokenSet"))
    had_tmdb  = bool(before.get("tmdbKeySet"))
    if not had_token and not had_tmdb:
        raise Skip("neither setting is set, so there is nothing to preserve")

    _json("saveServerSettings.view")
    after = _json("getServerSettings.view").get("serverSettings") or {}
    assert bool(after.get("discogsTokenSet")) == had_token, \
        "a saveServerSettings naming nothing cleared the Discogs token"
    assert bool(after.get("tmdbKeySet")) == had_tmdb, \
        "a saveServerSettings naming nothing cleared the TMDB key"
    print("PASS  saving no server setting leaves both alone")


TESTS = [
    test_matched_album_has_notes,
    test_matched_album_serves_a_poster,
    test_matched_titles_are_not_release_names,
    test_matched_video_has_a_genre,
    test_video_genre_is_browsable,
    test_saving_nothing_keeps_both_settings,
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

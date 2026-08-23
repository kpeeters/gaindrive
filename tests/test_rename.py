#!/usr/bin/env python3
"""renameAlbum tests.

The point of these is not that the rename works — that is visible by eye — but
that the client state hanging off the old path survives it. Every reference to a
media file in either database is the path string itself, so a directory rename
invalidates the key; stars, play counts, playlist entries and bookmarks are read
through INNER JOINs and therefore go *invisible* rather than erroring when they
go stale. Nothing in the scanner will ever repair them.

Start the server first, with a scanned collection, then run:
    python3 tests/test_rename.py

The album named below is renamed and renamed back, so the collection is left as
it was found — but point this at a scratch library rather than your own.
"""

import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

NS = "http://subsonic.org/restapi"

# The album to exercise. Must exist in the scanned collection and must NOT be a
# loose-file section (a folder that directly holds media is its own album, and
# renameAlbum deliberately refuses those).
ALBUM_TITLE = None   # None = use the first album of the first artist


def _get(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "xml"}
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


def _first_album():
    """(album_id, album_title, artist_id, artist_name) of a usable album."""
    root = _get("getIndexes.view")
    _check(root)
    for idx in root.find(f"{{{NS}}}indexes").findall(f"{{{NS}}}index"):
        for artist in idx.findall(f"{{{NS}}}artist"):
            d = _get("getMusicDirectory.view", {"id": artist.get("id")})
            _check(d)
            for child in d.find(f"{{{NS}}}directory").findall(f"{{{NS}}}child"):
                if child.get("isDir") == "true":
                    if ALBUM_TITLE and child.get("title") != ALBUM_TITLE:
                        continue
                    return (child.get("id"), child.get("title"),
                            artist.get("id"), artist.get("name"))
    raise AssertionError("No album found in the collection to test against")


def _rename(album_id, album, artist):
    root = _get("renameAlbum.view",
                {"id": album_id, "album": album, "artist": artist})
    _check(root)
    el = root.find(f"{{{NS}}}renamedAlbum")
    assert el is not None, "No <renamedAlbum> in response"
    return el


def _songs(album_id):
    d = _get("getMusicDirectory.view", {"id": album_id})
    _check(d)
    return [c for c in d.find(f"{{{NS}}}directory").findall(f"{{{NS}}}child")
            if c.get("isDir") != "true"]


def test_rename_round_trip():
    """The album comes back with the new names, and reverts cleanly."""
    album_id, title, _, artist = _first_album()
    new_title = title + " RENAMETEST"

    el = _rename(album_id, new_title, artist)
    assert el.get("album") == new_title, el.get("album")
    new_id = el.get("id")
    assert new_id and new_id != "0", "renameAlbum returned no new album id"

    d = _get("getMusicDirectory.view", {"id": new_id})
    _check(d)
    assert d.find(f"{{{NS}}}directory").get("name") == new_title

    # Back the way it was.
    back = _rename(new_id, title, artist)
    assert back.get("album") == title
    print(f"PASS  renameAlbum round trip on '{title}'")


def test_star_survives_rename():
    """A starred track stays starred. This is the regression that matters."""
    album_id, title, _, artist = _first_album()
    songs = _songs(album_id)
    assert songs, f"Album '{title}' has no tracks to star"
    song_id = songs[0].get("id")

    _check(_get("star.view", {"id": song_id}))
    try:
        new_title = title + " RENAMETEST"
        el = _rename(album_id, new_title, artist)
        new_id = el.get("id")

        starred = _get("getStarred.view")
        _check(starred)
        paths = [s.get("title") for s
                 in starred.find(f"{{{NS}}}starred").findall(f"{{{NS}}}song")]
        assert songs[0].get("title") in paths, (
            "Track lost its star across the rename — relocate_prefix did not "
            "rewrite client.stars.song_path")
        print("PASS  star survives renameAlbum")
        _rename(new_id, title, artist)
    finally:
        # Unstar by whichever id currently resolves.
        for sid in {s.get("id") for s in _songs(_first_album()[0])}:
            _get("unstar.view", {"id": sid})


def _artists():
    """{name: albumCount} from getArtists."""
    root = _get("getArtists.view")
    _check(root)
    out = {}
    for idx in root.find(f"{{{NS}}}artists").findall(f"{{{NS}}}index"):
        for a in idx.findall(f"{{{NS}}}artist"):
            out[a.get("name")] = int(a.get("albumCount") or 0)
    return out


def test_refile_removes_emptied_artist():
    """Re-filing an artist's only album must not strand the old artist.

    The relocate moves the album folder's path but leaves its parent_id on the
    old artist row, and folders.parent_id has no ON DELETE CASCADE — so if the
    emptied source directory is rescanned before the destination, the prune's
    DELETE FROM folders trips a foreign key, the whole prune rolls back, and the
    old artist survives reading "0 albums" with nothing in the log.
    """
    # Find an artist with exactly one album, so re-filing empties it.
    target = None
    for name, count in _artists().items():
        if count != 1:
            continue
        root = _get("getIndexes.view")
        for idx in root.find(f"{{{NS}}}indexes").findall(f"{{{NS}}}index"):
            for a in idx.findall(f"{{{NS}}}artist"):
                if a.get("name") != name:
                    continue
                d = _get("getMusicDirectory.view", {"id": a.get("id")})
                dirs = [c for c in d.find(f"{{{NS}}}directory")
                        .findall(f"{{{NS}}}child") if c.get("isDir") == "true"]
                if len(dirs) == 1:
                    target = (dirs[0].get("id"), dirs[0].get("title"), name)
        if target:
            break
    if not target:
        print("SKIP  refile-empties-artist (no single-album artist found)")
        return

    album_id, album_title, old_artist = target
    new_artist = old_artist + " RENAMETEST"

    el = _rename(album_id, album_title, new_artist)
    try:
        after = _artists()
        assert old_artist not in after, (
            f"'{old_artist}' survived with albumCount="
            f"{after.get(old_artist)} — the emptied artist folder was not "
            f"pruned (destination must be rescanned before the source)")
        assert after.get(new_artist) == 1, (
            f"'{new_artist}' should hold exactly one album, got "
            f"{after.get(new_artist)}")
        print("PASS  re-filing an artist's only album prunes the old artist")
    finally:
        _rename(el.get("id"), album_title, old_artist)


def test_rename_to_existing_is_refused():
    """Renaming onto a name that already exists must fail, not merge.

    relocate_prefix's plain UPDATEs are only safe because this check happened:
    half the path columns sit in a UNIQUE or composite key.
    """
    album_id, title, artist_id, artist = _first_album()
    d = _get("getMusicDirectory.view", {"id": artist_id})
    _check(d)
    siblings = [c.get("title") for c
                in d.find(f"{{{NS}}}directory").findall(f"{{{NS}}}child")
                if c.get("isDir") == "true" and c.get("title") != title]
    if not siblings:
        print("SKIP  rename-onto-existing (artist has only one album)")
        return

    root = _get("renameAlbum.view",
                {"id": album_id, "album": siblings[0], "artist": artist})
    _check(root, status="failed")
    print("PASS  renameAlbum onto an existing album is refused")


def test_empty_name_refused():
    album_id, title, _, artist = _first_album()
    # "..." sanitises to nothing: leading/trailing dots are stripped.
    root = _get("renameAlbum.view",
                {"id": album_id, "album": "...", "artist": artist})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", ET.tostring(root)
    print("PASS  a name that sanitises to nothing is refused")


def test_missing_id():
    root = _get("renameAlbum.view", {"album": "x"})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10"
    print("PASS  renameAlbum with missing id returns error 10")


def test_bad_id():
    root = _get("renameAlbum.view", {"id": "99999999", "album": "x"})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "70"
    print("PASS  renameAlbum with an unknown id returns error 70")


TESTS = [
    test_missing_id,
    test_bad_id,
    test_empty_name_refused,
    test_rename_round_trip,
    test_star_survives_rename,
    test_refile_removes_emptied_artist,
    test_rename_to_existing_is_refused,
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

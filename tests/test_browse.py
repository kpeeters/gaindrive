#!/usr/bin/env python3
"""Browsing endpoint tests: getIndexes, getMusicDirectory.

Start the server first (with a scanned music collection):
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_browse.py
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


def test_get_indexes():
    root = _get("getIndexes.view")
    _check(root)
    indexes = root.find(f"{{{NS}}}indexes")
    assert indexes is not None, "No <indexes> element in response"
    index_list = indexes.findall(f"{{{NS}}}index")
    assert len(index_list) > 0, "Expected at least one <index> element"
    # Each index must have a name attribute and at least one artist child.
    for idx in index_list:
        assert idx.get("name"), "Index missing name attribute"
        artists = idx.findall(f"{{{NS}}}artist")
        assert len(artists) > 0, f"Index '{idx.get('name')}' has no artists"
        for a in artists:
            assert a.get("id"), "Artist missing id"
            assert a.get("name"), "Artist missing name"
    print(f"PASS  getIndexes — {len(index_list)} index letters, "
          f"{sum(len(i.findall(f'{{{NS}}}artist')) for i in index_list)} artists")


def test_get_indexes_ignoredArticles():
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    arts = indexes.get("ignoredArticles", "")
    assert "The" in arts, "ignoredArticles should contain 'The'"
    print("PASS  getIndexes — ignoredArticles present")


def test_get_music_directory_artist():
    """getMusicDirectory on an artist folder should return album sub-dirs."""
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    first_artist = indexes.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found — run scan first"

    artist_id = first_artist.get("id")
    droot = _get("getMusicDirectory.view", {"id": artist_id})
    _check(droot)
    directory = droot.find(f"{{{NS}}}directory")
    assert directory is not None, "No <directory> element"
    children = directory.findall(f"{{{NS}}}child")
    assert len(children) > 0, "Artist directory has no children"
    dirs = [c for c in children if c.get("isDir") == "true"]
    assert len(dirs) > 0, "Expected album sub-directories"
    print(f"PASS  getMusicDirectory (artist) — {len(dirs)} albums in "
          f"'{directory.get('name')}'")


def test_get_music_directory_album():
    """getMusicDirectory on an album folder should return song files."""
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    first_artist = indexes.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found — run scan first"

    # Drill into first artist, grab first album.
    droot = _get("getMusicDirectory.view", {"id": first_artist.get("id")})
    directory = droot.find(f"{{{NS}}}directory")
    first_album = next(
        (c for c in directory.findall(f"{{{NS}}}child") if c.get("isDir") == "true"),
        None,
    )
    assert first_album is not None, "No album sub-directory found"

    aroot = _get("getMusicDirectory.view", {"id": first_album.get("id")})
    _check(aroot)
    adirectory = aroot.find(f"{{{NS}}}directory")
    songs = [c for c in adirectory.findall(f"{{{NS}}}child")
             if c.get("isDir") == "false"]
    assert len(songs) > 0, "Album directory has no songs"
    # Each song should have the key audio attributes.
    for s in songs:
        assert s.get("title"), "Song missing title"
        assert s.get("duration"), "Song missing duration"
        assert s.get("contentType"), "Song missing contentType"
    print(f"PASS  getMusicDirectory (album) — {len(songs)} songs in "
          f"'{adirectory.get('name')}'")


def _artist_fields_ok(song, where):
    """Every song entry carries all three artist fields, and they agree
    with the rule the server applies to them."""
    for f in ("artist", "displayArtist", "displayAlbumArtist"):
        assert song.get(f) is not None, (
            f"{where}: song {song.get('id')} has no {f} attribute — "
            "OpenSubsonic requires a supported field to be sent even when empty"
        )
    assert song.get("displayArtist") == song.get("artist"), (
        f"{where}: displayArtist {song.get('displayArtist')!r} != "
        f"artist {song.get('artist')!r}"
    )
    # artist falls back to the folder's artist, so it is empty only when that
    # is too — never merely because the file carries no tag.
    if song.get("displayAlbumArtist"):
        assert song.get("artist"), (
            f"{where}: song {song.get('id')} has an album artist but no artist"
        )


def _first_album_songs():
    """The songs of the first album the library offers, however it is shaped."""
    root = _get("getIndexes.view")
    first_artist = root.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found — run scan first"
    droot = _get("getMusicDirectory.view", {"id": first_artist.get("id")})
    directory = droot.find(f"{{{NS}}}directory")
    album = next(
        (c for c in directory.findall(f"{{{NS}}}child") if c.get("isDir") == "true"),
        None,
    )
    # A section holding loose files is its own album, so the artist listing may
    # already be the songs.
    target = album.get("id") if album is not None else first_artist.get("id")
    aroot = _get("getMusicDirectory.view", {"id": target})
    adir = aroot.find(f"{{{NS}}}directory")
    return target, [c for c in adir.findall(f"{{{NS}}}child")
                    if c.get("isDir") == "false"]


def test_song_artist_fields():
    """The three artist fields, across the endpoints that return songs.

    This is the contract of artist_of() seen from outside: displayAlbumArtist
    is the folder-derived artist, artist is the file's own when it names
    somebody else, and both OpenSubsonic fields are always present. Checked on
    more than one endpoint deliberately — the columns are selected by thirteen
    separate queries, and one of them forgetting is exactly the failure this
    catches.
    """
    album_id, songs = _first_album_songs()
    assert songs, "No songs found — run scan first"
    for s in songs:
        _artist_fields_ok(s, "getMusicDirectory")

    checked = ["getMusicDirectory"]

    # Reported only when it actually returned something: the id above is an
    # album folder unless the library's first artist holds loose files, and a
    # silently empty check is worse than a missing one.
    aroot = _get("getAlbum.view", {"id": album_id})
    found = aroot.findall(f".//{{{NS}}}song")
    for s in found:
        _artist_fields_ok(s, "getAlbum")
    if found:
        checked.append("getAlbum")

    sroot = _get("getSong.view", {"id": songs[0].get("id")})
    found = sroot.findall(f".//{{{NS}}}song")
    for s in found:
        _artist_fields_ok(s, "getSong")
    if found:
        checked.append("getSong")

    # A query with no results proves nothing, so search on a title we know.
    title = songs[0].get("title") or ""
    if title:
        qroot = _get("search3.view", {"query": title, "artistCount": "0",
                                      "albumCount": "0", "songCount": "20"})
        found = qroot.findall(f".//{{{NS}}}song")
        for s in found:
            _artist_fields_ok(s, "search3")
        if found:
            checked.append("search3")

    # These two are empty on a fresh install; check whatever is there.
    for endpoint, tag in (("getStarred.view", "song"), ("getBookmarks.view", "entry")):
        root = _get(endpoint)
        for s in root.findall(f".//{{{NS}}}{tag}"):
            _artist_fields_ok(s, endpoint)

    print(f"PASS  song artist fields — {len(songs)} songs, "
          f"endpoints: {', '.join(checked)}")


def test_get_music_directory_not_found():
    root = _get("getMusicDirectory.view", {"id": "999999"})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "70"
    print("PASS  getMusicDirectory with bad id returns error 70")


def test_get_music_directory_missing_id():
    root = _get("getMusicDirectory.view")
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10"
    print("PASS  getMusicDirectory with missing id returns error 10")


TESTS = [
    test_get_indexes,
    test_get_indexes_ignoredArticles,
    test_get_music_directory_artist,
    test_get_music_directory_album,
    test_song_artist_fields,
    test_get_music_directory_not_found,
    test_get_music_directory_missing_id,
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

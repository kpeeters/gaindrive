#!/usr/bin/env python3
"""moveAlbum tests.

The point of these is not that the move works — that is visible by eye — but
that the client state hanging off the old path survives it. Every reference to a
media file in either database is the path string itself, so a directory move
invalidates the key; stars, play counts, playlist entries and bookmarks are read
through INNER JOINs and therefore go *invisible* rather than erroring when they
go stale. Nothing in the scanner will ever repair them.

`moveAlbum` replaced `renameAlbum` and `promoteAlbum`, which were the same
operation described two ways. The rule the tests below lean on is that **naming
`musicFolderId` re-roots and omitting it stays put**, and that every other
parameter means "unchanged" when omitted.

Start the server first, with a scanned collection, then run:
    python3 tests/test_move.py

Everything moved here is moved back, so the collection is left as it was found
— but point this at a scratch library rather than your own.
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
# moveAlbum refuses to rename one in place — there is no artist level to change).
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


def _roots():
    """[(id, name, contentType)] from getMusicFolders — never the uploads root."""
    root = _get("getMusicFolders.view")
    _check(root)
    return [(f.get("id"), f.get("name"), f.get("contentType"))
            for f in root.find(f"{{{NS}}}musicFolders")
                         .findall(f"{{{NS}}}musicFolder")]


def _move(album_id, **params):
    """A successful moveAlbum. Returns the <movedAlbum> element."""
    root = _get("moveAlbum.view", dict({"id": album_id}, **params))
    _check(root)
    el = root.find(f"{{{NS}}}movedAlbum")
    assert el is not None, "No <movedAlbum> in response"
    return el


def _songs(album_id):
    d = _get("getMusicDirectory.view", {"id": album_id})
    _check(d)
    return [c for c in d.find(f"{{{NS}}}directory").findall(f"{{{NS}}}child")
            if c.get("isDir") != "true"]


def test_rename_round_trip():
    """The album comes back with the new name, and reverts cleanly."""
    album_id, title, _, artist = _first_album()
    new_title = title + " MOVETEST"

    el = _move(album_id, album=new_title)
    assert el.get("album") == new_title, el.get("album")
    # Omitted folder means unchanged, and the response says which it kept.
    assert el.get("artist") == artist, el.get("artist")
    new_id = el.get("id")
    assert new_id and new_id != "0", "moveAlbum returned no new album id"

    d = _get("getMusicDirectory.view", {"id": new_id})
    _check(d)
    assert d.find(f"{{{NS}}}directory").get("name") == new_title

    # Back the way it was.
    back = _move(new_id, album=title)
    assert back.get("album") == title
    print(f"PASS  moveAlbum rename round trip on '{title}'")


def test_no_op_move_is_ok():
    """Sending the names it already has changes nothing and still answers."""
    album_id, title, _, artist = _first_album()
    el = _move(album_id, album=title, folder=artist)
    assert el.get("id") == album_id, (el.get("id"), album_id)
    assert el.get("tagFailures") == "0", el.get("tagFailures")
    print("PASS  a move that changes nothing is not an error")


def test_star_survives_rename():
    """A starred track stays starred. This is the regression that matters."""
    album_id, title, _, artist = _first_album()
    songs = _songs(album_id)
    assert songs, f"Album '{title}' has no tracks to star"
    song_id = songs[0].get("id")

    _check(_get("star.view", {"id": song_id}))
    try:
        new_title = title + " MOVETEST"
        el = _move(album_id, album=new_title)
        new_id = el.get("id")

        starred = _get("getStarred.view")
        _check(starred)
        titles = [s.get("title") for s
                  in starred.find(f"{{{NS}}}starred").findall(f"{{{NS}}}song")]
        assert songs[0].get("title") in titles, (
            "Track lost its star across the move — relocate_prefix did not "
            "rewrite client.stars.song_path")
        print("PASS  star survives a rename")
        _move(new_id, album=title)
    finally:
        # Unstar by whichever id currently resolves.
        for sid in {s.get("id") for s in _songs(_first_album()[0])}:
            _get("unstar.view", {"id": sid})


def test_star_survives_cross_root_move():
    """The same, across roots — the move renameAlbum could never do.

    Worth its own case rather than trusting the rename one: re-rooting takes the
    other branch of the destination rule, so it builds a completely different
    target path, and relocate_prefix is what has to cope either way.
    """
    roots = _roots()
    if len(roots) < 2:
        print("SKIP  star-survives-cross-root (server has one library root)")
        return

    album_id, title, artist_id, artist = _first_album()
    d = _get("getMusicDirectory.view", {"id": artist_id})
    _check(d)
    home = None
    for rid, name, _ctype in roots:
        # The root this album already lives in, found by asking which root's
        # listing holds its artist.
        idx = _get("getArtists.view", {"musicFolderId": rid})
        names = [a.get("name")
                 for i in idx.find(f"{{{NS}}}artists").findall(f"{{{NS}}}index")
                 for a in i.findall(f"{{{NS}}}artist")]
        if artist in names:
            home = rid
            break
    if home is None:
        print("SKIP  star-survives-cross-root (could not locate the album's root)")
        return
    dest = next((r for r in roots if r[0] != home), None)
    if dest is None:
        print("SKIP  star-survives-cross-root (no second root to move into)")
        return

    songs = _songs(album_id)
    assert songs, f"Album '{title}' has no tracks to star"
    _check(_get("star.view", {"id": songs[0].get("id")}))
    moved_id = None
    try:
        el = _move(album_id, musicFolderId=dest[0], folder=artist)
        moved_id = el.get("id")
        assert moved_id and moved_id != "0"

        starred = _get("getStarred.view")
        _check(starred)
        titles = [s.get("title") for s
                  in starred.find(f"{{{NS}}}starred").findall(f"{{{NS}}}song")]
        assert songs[0].get("title") in titles, (
            "Track lost its star across a cross-root move")
        print(f"PASS  star survives a move from root {home} to {dest[0]}")
    finally:
        if moved_id:
            _move(moved_id, musicFolderId=home, folder=artist)
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
    new_artist = old_artist + " MOVETEST"

    el = _move(album_id, folder=new_artist)
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
        _move(el.get("id"), folder=old_artist)


def test_move_to_existing_is_refused():
    """Moving onto a name that already exists must fail, not merge.

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
        print("SKIP  move-onto-existing (artist has only one album)")
        return

    root = _get("moveAlbum.view",
                {"id": album_id, "album": siblings[0], "folder": artist})
    _check(root, status="failed")
    print("PASS  moveAlbum onto an existing album is refused")


def test_empty_name_refused():
    album_id, title, _, artist = _first_album()
    # "..." sanitises to nothing: leading/trailing dots are stripped.
    root = _get("moveAlbum.view", {"id": album_id, "album": "..."})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", ET.tostring(root)
    print("PASS  a name that sanitises to nothing is refused")


def test_missing_id():
    root = _get("moveAlbum.view", {"album": "x"})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10"
    print("PASS  moveAlbum with missing id returns error 10")


def test_bad_id():
    root = _get("moveAlbum.view", {"id": "99999999", "album": "x"})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "70"
    print("PASS  moveAlbum with an unknown id returns error 70")


def test_reroot_without_folder_is_refused():
    """Naming a root without a folder is error 10, not a guess.

    The folder half was briefly allowed to default to the source's own level-1
    name, which for a fetched video is the channel that published it and never a
    category. A caller that does not know where something belongs is made to
    decide rather than have it decided badly.
    """
    album_id, _title, _aid, _artist = _first_album()
    roots = _roots()
    assert roots, "server reports no library roots"
    root = _get("moveAlbum.view",
                {"id": album_id, "musicFolderId": roots[0][0]})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", ET.tostring(root)
    print("PASS  musicFolderId without folder returns error 10")


def test_bad_music_folder_is_refused():
    """An id that is not a browsable library root — the uploads root included."""
    album_id, _title, _aid, artist = _first_album()
    root = _get("moveAlbum.view",
                {"id": album_id, "musicFolderId": "99999999", "folder": artist})
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "70", ET.tostring(root)
    print("PASS  an unusable destination root returns error 70")


TESTS = [
    test_missing_id,
    test_bad_id,
    test_empty_name_refused,
    test_reroot_without_folder_is_refused,
    test_bad_music_folder_is_refused,
    test_no_op_move_is_ok,
    test_rename_round_trip,
    test_star_survives_rename,
    test_star_survives_cross_root_move,
    test_refile_removes_emptied_artist,
    test_move_to_existing_is_refused,
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

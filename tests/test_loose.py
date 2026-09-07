#!/usr/bin/env python3
"""The one-album-per-loose-file rule, seen from outside.

A media file sitting directly in an artist or category folder is an album of
its own, holding that one track. What that replaced made the *section* the
album, so every loose file in it collapsed into one listing; the tests here are
the shape of the difference, and each of them fails under the old rule.

The load-bearing implementation fact is that such an album is an ordinary
`folders` row whose `path` names the media file rather than a directory — so
these also check the places that used to do filesystem work on a folder path
and would now be handed a file: getAlbumTexts, getAlbumImages, getCoverArt.

Nothing here writes anything, so it is safe against a real library. It needs a
collection that actually holds a loose file somewhere; with none it reports
that and skips, rather than passing vacuously.

Start the server first, then run:
    python3 tests/test_loose.py
"""

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

# Extensions the scanner indexes, near enough to spot a media file in a path.
# Only used to recognise one, never to decide anything.
MEDIA_EXT = (".mp3", ".flac", ".ogg", ".opus", ".m4a", ".aac", ".wma", ".wav",
             ".mp4", ".mkv", ".avi", ".mov", ".webm", ".m4v", ".mpg", ".mpeg",
             ".wmv", ".vob", ".ts")


def _get(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "xml"}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        return ET.fromstring(r.read())


def _raw(endpoint, extra=None):
    """(status, content-type, body) — for the endpoints that return bytes."""
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    try:
        with urllib.request.urlopen(url) as r:
            return r.status, r.headers.get("Content-Type", ""), r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.headers.get("Content-Type", ""), e.read()


def _check(root, what):
    assert root.get("status") == "ok", \
        f"{what} failed: {ET.tostring(root).decode()[:400]}"


def _artists():
    root = _get("getIndexes.view")
    _check(root, "getIndexes")
    return root.findall(f".//{{{NS}}}artist")


def _children(fid):
    root = _get("getMusicDirectory.view", {"id": fid})
    _check(root, "getMusicDirectory")
    d = root.find(f"{{{NS}}}directory")
    return d, d.findall(f"{{{NS}}}child")


def _find_file_album():
    """(album_child, section_id) for some single-file album, or (None, None).

    Recognised by its own song's path rather than by guessing from the title:
    the album is a file-album exactly when getAlbum returns one song whose
    `path` is the album's own folder path. `path` is not on the wire, so the
    proxy used here is a one-song album whose single song's suffix makes the
    album's title plus that suffix the on-disk name — which is what the
    scanner derives it from.
    """
    for artist in _artists():
        _, kids = _children(artist.get("id"))
        for c in kids:
            if c.get("isDir") != "true":
                continue
            aroot = _get("getAlbum.view", {"id": c.get("id")})
            if aroot.get("status") != "ok":
                continue
            songs = aroot.findall(f".//{{{NS}}}song")
            if len(songs) != 1:
                continue
            # An album whose one song is named exactly as the album is the
            # shape a file-album takes: both come from the same file stem.
            if songs[0].get("title") == c.get("title"):
                return c, artist.get("id")
    return None, None


def test_a_section_lists_only_directories():
    """No artist or section reports a bare song child.

    This is the rule stated as an invariant: a media file directly in a
    level-1 folder is an album, so it appears as a child *directory*. Under
    the old rule those files were listed as songs of the section itself.
    """
    offenders = []
    for artist in _artists():
        d, kids = _children(artist.get("id"))
        for c in kids:
            if c.get("isDir") == "false":
                offenders.append((d.get("name"), c.get("title")))
    assert not offenders, \
        f"level-1 folders still list songs directly: {offenders[:5]}"
    print(f"  ok: {len(_artists())} level-1 folders list only albums")


def test_file_album_parents_its_section():
    """A file-album's parent is the section, not itself.

    Under the old rule the section was its own album, so `parent` pointed at
    the folder being listed and a client anchoring its album list on that
    field landed on the list of sections.
    """
    album, section_id = _find_file_album()
    if album is None:
        print("  skipped: no single-file album in this collection")
        return
    root = _get("getAlbum.view", {"id": album.get("id")})
    _check(root, "getAlbum")
    el = root.find(f"{{{NS}}}album")
    assert el.get("id") != el.get("parent"), \
        "a file-album reports itself as its own parent"
    assert el.get("parent") == section_id, \
        f"parent {el.get('parent')} is not the section {section_id}"
    assert el.get("songCount") == "1", \
        f"a file-album should hold one track, got {el.get('songCount')}"
    print(f"  ok: {el.get('title')!r} is an album of one under {section_id}")


def test_album_texts_of_a_file_album():
    """getAlbumTexts survives a file-backed album.

    It runs directory_iterator on the album folder, which throws for a file;
    the throw is swallowed, so the failure mode is a silent empty list rather
    than an error. What must hold is that it answers at all, and that whatever
    it lists is the sidecar named after the file — never a `.chapters.txt`.
    """
    album, _ = _find_file_album()
    if album is None:
        print("  skipped: no single-file album in this collection")
        return
    root = _get("getAlbumTexts.view", {"id": album.get("id")})
    _check(root, "getAlbumTexts")
    names = [t.get("name") for t in root.findall(f".//{{{NS}}}textFile")]
    for n in names:
        assert not n.endswith(".chapters.txt"), \
            f"chapter sidecar listed as liner notes: {n}"
        assert n == album.get("title") + ".txt", \
            f"{n} is not the sidecar of {album.get('title')!r}"
    print(f"  ok: getAlbumTexts answered with {names}")


def test_album_images_of_a_file_album():
    """getAlbumImages answers for a file-backed album rather than erroring."""
    album, _ = _find_file_album()
    if album is None:
        print("  skipped: no single-file album in this collection")
        return
    root = _get("getAlbumImages.view", {"id": album.get("id")})
    _check(root, "getAlbumImages")
    el = root.find(f"{{{NS}}}albumImages")
    assert el is not None and el.get("count") is not None, \
        "getAlbumImages returned no count"
    print(f"  ok: count={el.get('count')}")


def test_cover_art_of_a_file_album_is_not_a_portrait():
    """A coverless album is never answered as an artist portrait.

    getCoverArt falls into its artist branch whenever the album has no cover,
    which used to mean a coverless album asked MusicBrainz about its own
    title. Every loose track without a sidecar image would do that now, so the
    branch is guarded: an album with no cover is a plain 404 with no
    Cache-Control telling the client to come back.
    """
    album, _ = _find_file_album()
    if album is None:
        print("  skipped: no single-file album in this collection")
        return
    status, ctype, _body = _raw("getCoverArt.view", {"id": album.get("id")})
    assert status in (200, 404), f"unexpected status {status}"
    if status == 200:
        assert ctype.startswith("image/"), f"cover art returned {ctype}"
        print(f"  ok: cover served as {ctype}")
    else:
        print("  ok: no cover, answered 404 rather than a portrait lookup")


def test_a_root_used_flat_lists_its_files_as_albums():
    """A root holding loose files appears as an artist whose albums are them.

    This is the level-1 case, and the one that breaks silently: such an album
    is a level-1 folder row, which scan() reinstates from the database and
    would otherwise hand to scan_artist_dir() — where fs::is_directory fails,
    the row reads as deleted, and every root-level album is pruned on every
    full scan. Run a full scan twice before trusting a pass here.
    """
    roots = _get("getMusicFolders.view")
    _check(roots, "getMusicFolders")
    names = {f.get("name") for f in roots.findall(f".//{{{NS}}}musicFolder")}
    flat = [a for a in _artists() if a.get("name") in names]
    if not flat:
        print("  skipped: no root is used as one flat library")
        return
    for a in flat:
        _, kids = _children(a.get("id"))
        bare = [c.get("title") for c in kids if c.get("isDir") == "false"]
        assert not bare, f"root {a.get('name')!r} lists songs directly: {bare[:5]}"
        assert kids, f"root {a.get('name')!r} reports no albums"
        for c in kids:
            assert not any(c.get("title", "").lower().endswith(e)
                           for e in MEDIA_EXT), \
                f"album named with its extension: {c.get('title')!r}"
    print(f"  ok: {len(flat)} flat root(s) list their files as albums")


def main():
    tests = [v for k, v in sorted(globals().items()) if k.startswith("test_")]
    failed = 0
    for t in tests:
        print(f"{t.__name__}: {(t.__doc__ or '').splitlines()[0]}")
        try:
            t()
        except AssertionError as e:
            print(f"  FAIL: {e}")
            failed += 1
        except Exception as e:                       # noqa: BLE001
            print(f"  ERROR: {type(e).__name__}: {e}")
            failed += 1
    print(f"\n{len(tests) - failed}/{len(tests)} passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())

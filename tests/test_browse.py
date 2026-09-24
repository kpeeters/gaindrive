#!/usr/bin/env python3
"""Browsing endpoint tests: getIndexes, getMusicDirectory, genres.

The genre tests skip rather than fail when nothing in the library carries a
genre tag: coverage is a property of the collection, not of the server, and a
library with none is a legitimate one.

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
    print(f"PASS  getIndexes - {len(index_list)} index letters, "
          f"{sum(len(i.findall(f'{{{NS}}}artist')) for i in index_list)} artists")


def test_get_indexes_ignoredArticles():
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    arts = indexes.get("ignoredArticles", "")
    assert "The" in arts, "ignoredArticles should contain 'The'"
    print("PASS  getIndexes - ignoredArticles present")


def test_get_music_directory_artist():
    """getMusicDirectory on an artist folder should return album sub-dirs."""
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    first_artist = indexes.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found - run scan first"

    artist_id = first_artist.get("id")
    droot = _get("getMusicDirectory.view", {"id": artist_id})
    _check(droot)
    directory = droot.find(f"{{{NS}}}directory")
    assert directory is not None, "No <directory> element"
    children = directory.findall(f"{{{NS}}}child")
    assert len(children) > 0, "Artist directory has no children"
    dirs = [c for c in children if c.get("isDir") == "true"]
    assert len(dirs) > 0, "Expected album sub-directories"
    print(f"PASS  getMusicDirectory (artist) - {len(dirs)} albums in "
          f"'{directory.get('name')}'")


def test_get_music_directory_album():
    """getMusicDirectory on an album folder should return song files."""
    root = _get("getIndexes.view")
    indexes = root.find(f"{{{NS}}}indexes")
    first_artist = indexes.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found - run scan first"

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
    print(f"PASS  getMusicDirectory (album) - {len(songs)} songs in "
          f"'{adirectory.get('name')}'")


def _artist_fields_ok(song, where):
    """Every song entry carries all three artist fields, and they agree
    with the rule the server applies to them."""
    for f in ("artist", "displayArtist", "displayAlbumArtist"):
        assert song.get(f) is not None, (
            f"{where}: song {song.get('id')} has no {f} attribute - "
            "OpenSubsonic requires a supported field to be sent even when empty"
        )
    assert song.get("displayArtist") == song.get("artist"), (
        f"{where}: displayArtist {song.get('displayArtist')!r} != "
        f"artist {song.get('artist')!r}"
    )
    # artist falls back to the folder's artist, so it is empty only when that
    # is too - never merely because the file carries no tag.
    if song.get("displayAlbumArtist"):
        assert song.get("artist"), (
            f"{where}: song {song.get('id')} has an album artist but no artist"
        )


def _first_album_songs():
    """The songs of the first album the library offers, however it is shaped."""
    root = _get("getIndexes.view")
    first_artist = root.find(f".//{{{NS}}}artist")
    assert first_artist is not None, "No artists found - run scan first"
    droot = _get("getMusicDirectory.view", {"id": first_artist.get("id")})
    directory = droot.find(f"{{{NS}}}directory")
    album = next(
        (c for c in directory.findall(f"{{{NS}}}child") if c.get("isDir") == "true"),
        None,
    )
    # Every album is a child directory now, a loose file's own album included,
    # so an artist with nothing under it has nothing to test.
    assert album is not None, "First artist holds no albums - run scan first"
    target = album.get("id")
    aroot = _get("getMusicDirectory.view", {"id": target})
    adir = aroot.find(f"{{{NS}}}directory")
    return target, [c for c in adir.findall(f"{{{NS}}}child")
                    if c.get("isDir") == "false"]


def test_song_artist_fields():
    """The three artist fields, across the endpoints that return songs.

    This is the contract of artist_of() seen from outside: displayAlbumArtist
    is the folder-derived artist, artist is the file's own when it names
    somebody else, and both OpenSubsonic fields are always present. Checked on
    more than one endpoint deliberately - the columns are selected by thirteen
    separate queries, and one of them forgetting is exactly the failure this
    catches.
    """
    album_id, songs = _first_album_songs()
    assert songs, "No songs found - run scan first"
    for s in songs:
        _artist_fields_ok(s, "getMusicDirectory")

    checked = ["getMusicDirectory"]

    # Reported only when it actually returned something: a silently empty check
    # is worse than a missing one.
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

    print(f"PASS  song artist fields - {len(songs)} songs, "
          f"endpoints: {', '.join(checked)}")


def test_album_video_count():
    """videoCount agrees with the album's own tracks, on all three endpoints.

    The field is emitted from three separate queries, so the failure this
    catches is one of them forgetting the column - which looks like "some
    albums have no icon" rather than like an error.

    Videos are a property of the collection, so a library with none skips
    rather than fails; the zero half is still checked everywhere.

    Assumes the default `flat_multi_disc`: the count is over every song of the
    album, which is what getAlbum returns only when it flattens disc subfolders
    into the listing.
    """
    checked = 0
    with_video = 0

    videos = _get("getVideos.view").findall(f".//{{{NS}}}video")

    # Whichever albums we can reach, video or not: getAlbum is the one endpoint
    # that returns the album and its songs together, so it is the only place the
    # count can be verified against what it counts.
    album_ids = []
    for v in videos[:5]:
        if v.get("parent"):
            album_ids.append(v.get("parent"))
    root = _get("getAlbumList.view", {"type": "alphabeticalByName", "size": "5"})
    album_ids += [a.get("id") for a in root.findall(f".//{{{NS}}}album")]

    for aid in dict.fromkeys(album_ids):
        al = _get("getAlbum.view", {"id": aid}).find(f"{{{NS}}}album")
        if al is None:
            continue
        got = al.get("videoCount")
        assert got is not None, f"getAlbum album {aid} has no videoCount"
        songs = al.findall(f"{{{NS}}}song")
        want = sum(1 for s in songs if s.get("isVideo") == "true")
        assert int(got) == want, \
            f"album {aid}: videoCount={got} but {want} of {len(songs)} songs are video"
        checked += 1
        if want:
            with_video += 1

        # The same album through the other two, which read it from their own
        # queries.
        parent = al.get("parent")
        if parent:
            arts = _get("getArtist.view", {"id": parent})
            for a in arts.findall(f".//{{{NS}}}album"):
                if a.get("id") == aid:
                    assert a.get("videoCount") == got, \
                        f"album {aid}: getArtist says {a.get('videoCount')!r}, " \
                        f"getAlbum says {got!r}"

    lst = _get("getAlbumList.view", {"type": "alphabeticalByName", "size": "20"})
    for a in lst.findall(f".//{{{NS}}}album"):
        assert a.get("videoCount") is not None, \
            f"getAlbumList album {a.get('id')} has no videoCount"

    if not checked:
        print("SKIP  album videoCount - no albums found")
        return
    print(f"PASS  album videoCount - {checked} albums checked, "
          f"{with_video} holding video")


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


def _genres():
    root = _get("getGenres.view")
    _check(root)
    el = root.find(f"{{{NS}}}genres")
    assert el is not None, "no <genres> element"
    return el.findall(f"{{{NS}}}genre")


def test_get_genres():
    genres = _genres()
    if not genres:
        print("SKIP  getGenres: no song in the library carries a genre tag")
        return
    for g in genres:
        # The name is the element's TEXT, not an attribute. Subsonic spells
        # this one differently from every other listing, and getting it wrong
        # yields a list of blanks rather than an error.
        assert (g.text or "").strip(), \
            f"genre with no text: {ET.tostring(g).decode()}"
        assert int(g.get("songCount") or 0) > 0, \
            f"songCount={g.get('songCount')!r} on genre {g.text!r}"
        # albumCount may legitimately be 0: it counts albums whose rolled-up
        # genre is this one, so a genre carried by a single track of an album
        # that is mostly something else has songs but no albums.
        assert int(g.get("albumCount") or -1) >= 0, \
            f"albumCount={g.get('albumCount')!r} on genre {g.text!r}"
    # Counts are ordered commonest first, which is what makes the list usable.
    counts = [int(g.get("songCount")) for g in genres]
    assert counts == sorted(counts, reverse=True), \
        f"getGenres not ordered by songCount: {counts[:10]}"
    # Folding means a genre can appear once only, case and padding ignored.
    folded = [(g.text or "").strip().lower() for g in genres]
    assert len(folded) == len(set(folded)), \
        "getGenres returned the same genre twice under different spellings"
    print(f"PASS  getGenres returned {len(genres)} folded genres, "
          f"commonest {genres[0].text!r} ({counts[0]} songs)")


def test_get_songs_by_genre():
    genres = _genres()
    if not genres:
        print("SKIP  getSongsByGenre: nothing in the library carries a genre")
        return
    name  = genres[0].text
    total = int(genres[0].get("songCount"))

    root = _get("getSongsByGenre.view", {"genre": name, "count": "10"})
    _check(root)
    el = root.find(f"{{{NS}}}songsByGenre")
    assert el is not None, "no <songsByGenre> element"
    songs = el.findall(f"{{{NS}}}song")
    assert songs, f"no songs for genre {name!r} which claims {total}"
    assert len(songs) <= 10, f"count=10 ignored, got {len(songs)}"

    # The match folds case and padding, so anything getGenres reported has to
    # come back whatever spelling the client sends it in.
    for variant in (name.upper(), name.lower(), f"  {name}  "):
        r2 = _get("getSongsByGenre.view", {"genre": variant, "count": "10"})
        _check(r2)
        n2 = len(r2.find(f"{{{NS}}}songsByGenre").findall(f"{{{NS}}}song"))
        assert n2 == len(songs), \
            f"genre={variant!r} gave {n2} songs, {name!r} gave {len(songs)}"

    # offset pages rather than repeating the first result.
    if len(songs) > 1:
        r3 = _get("getSongsByGenre.view",
                  {"genre": name, "count": "10", "offset": "1"})
        _check(r3)
        s3 = r3.find(f"{{{NS}}}songsByGenre").findall(f"{{{NS}}}song")
        if s3:
            assert s3[0].get("id") != songs[0].get("id"), \
                "offset=1 returned the same first song"
    print(f"PASS  getSongsByGenre {name!r} -> {len(songs)} songs, "
          "case/padding folded, offset pages")


def test_genre_multi_value():
    """A song may carry several genres and must be reachable under each.

    This is the property the song_genres table exists for: a film is normally
    two or three genres, and filing it under only the first is exactly the
    loss the single songs.genre column caused."""
    genres = _genres()
    if not genres:
        print("SKIP  multi-genre: nothing in the library carries a genre")
        return

    # Find a song listed under two different genres. Cheap enough: walk the
    # genres by size and collect song ids until one repeats.
    seen, shared = {}, None
    for g in genres[:25]:
        root = _get("getSongsByGenre.view", {"genre": g.text, "count": "200"})
        _check(root)
        el = root.find(f"{{{NS}}}songsByGenre")
        for s in (el.findall(f"{{{NS}}}song") if el is not None else []):
            sid = s.get("id")
            if sid in seen and seen[sid] != g.text:
                shared = (sid, seen[sid], g.text)
                break
            seen[sid] = g.text
        if shared:
            break

    if not shared:
        print("SKIP  multi-genre: no song in this library carries two genres "
              "(expected without a TMDB key, or before a rescan)")
        return

    sid, g1, g2 = shared
    # Both genres must also reach it through the album filter, which is the
    # half that used to lose it.
    for g in (g1, g2):
        r = _get("getAlbumList.view",
                 {"type": "byGenre", "genre": g, "size": "500"})
        _check(r)
        el = r.find(f"{{{NS}}}albumList")
        assert el is not None and el.findall(f"{{{NS}}}album"), \
            f"song {sid} is listed under {g!r} but byGenre returns no albums"
    print(f"PASS  song {sid} reachable under both {g1!r} and {g2!r}")


def test_get_songs_by_genre_missing_genre():
    root = _get("getSongsByGenre.view")
    _check(root, status="failed")
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10"
    print("PASS  getSongsByGenre with no genre returns error 10")


def test_album_list_by_genre():
    """albums.genre was empty for every album ever scanned, so this filter
    could match nothing but the empty string. Guard against the regression,
    and against getGenres and getAlbumList disagreeing about what a genre
    contains: albumCount is defined as exactly what byGenre returns."""
    genres = _genres()
    if not genres:
        print("SKIP  getAlbumList byGenre: nothing carries a genre")
        return
    withalbums = [g for g in genres if int(g.get("albumCount") or 0) > 0]
    if not withalbums:
        raise AssertionError(
            "every genre reports albumCount=0 -- albums.genre is probably "
            "empty again, which is what made type=byGenre match nothing")

    g     = withalbums[0]
    want  = int(g.get("albumCount"))
    root  = _get("getAlbumList.view",
                 {"type": "byGenre", "genre": g.text, "size": "500"})
    _check(root)
    el = root.find(f"{{{NS}}}albumList")
    got = len(el.findall(f"{{{NS}}}album")) if el is not None else 0
    assert got == min(want, 500), \
        f"getGenres says {g.text!r} has {want} albums, byGenre returned {got}"
    print(f"PASS  getAlbumList type=byGenre {g.text!r} returned "
          f"{got} albums, matching albumCount")


TESTS = [
    test_get_indexes,
    test_get_indexes_ignoredArticles,
    test_get_music_directory_artist,
    test_get_music_directory_album,
    test_song_artist_fields,
    test_album_video_count,
    test_get_music_directory_not_found,
    test_get_music_directory_missing_id,
    test_get_genres,
    test_get_songs_by_genre,
    test_genre_multi_value,
    test_get_songs_by_genre_missing_genre,
    test_album_list_by_genre,
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

#!/usr/bin/env python3
"""Authorization tests: what an ordinary account may *not* reach.

These are regressions for the gaps found reviewing the server before putting
it on a public address. Every one of them was reachable by any authenticated
user, so they are about a compromised or careless account rather than about a
stranger.

Needs an admin account and at least one album in a library root. The script
creates a throwaway non-admin user, uses it, and deletes it - except that
Subsonic has no deleteUser, so it disables the account instead and leaves it.

Start the server first, then:
    python3 tests/test_authz.py
"""

import sys
import urllib.error
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE   = "http://localhost:4040/rest"
ADMIN  = "admin"
APASS  = "secret"
VER    = "1.16.1"
CLIENT = "test"

# The throwaway account. Named so it is obvious in the Users list what it is.
PLAIN  = "authztest"
PPASS  = "authztest-password"

NS = "http://subsonic.org/restapi"


def _get(endpoint, extra=None, *, user=ADMIN, password=APASS):
    p = {"u": user, "p": password, "v": VER, "c": CLIENT, "f": "xml"}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    try:
        with urllib.request.urlopen(url) as r:
            return r.status, ET.fromstring(r.read())
    except urllib.error.HTTPError as e:
        return e.code, None


def _status(root):
    return root.get("status") if root is not None else None


def _error_code(root):
    if root is None:
        return None
    err = root.find(f"{{{NS}}}error")
    return err.get("code") if err is not None else None


def _first_song_and_folder():
    """An album folder id and a song id inside it, from the shared library."""
    _, root = _get("search3.view", {"query": "a", "songCount": "5",
                                    "albumCount": "5"})
    assert root is not None, "search3 failed"
    result = root.find(f"{{{NS}}}searchResult3")
    assert result is not None, "search3 returned no result element"
    song = result.find(f"{{{NS}}}song")
    album = result.find(f"{{{NS}}}album")
    assert song is not None, (
        "no songs matched 'a'; this test needs a non-empty library"
    )
    folder = album.get("id") if album is not None else song.get("parent")
    return song.get("id"), folder


# ---- Setup -------------------------------------------------------------


def setup_plain_user():
    """Create the non-admin account, or re-enable it from a previous run."""
    _get("createUser.view", {"username": PLAIN, "password": PPASS,
                             "adminRole": "false", "uploadRole": "false",
                             "castRole": "false"})
    # Whether it was just created or already existed, make sure it is enabled
    # and has no roles - a previous run disables it on the way out.
    _, root = _get("updateUser.view", {"username": PLAIN, "password": PPASS,
                                       "adminRole": "false",
                                       "uploadRole": "false",
                                       "castRole": "false",
                                       "disabled": "false"})
    assert _status(root) == "ok", (
        f"could not set up {PLAIN}: {ET.tostring(root).decode() if root else '?'}"
    )
    _, root = _get("ping.view", user=PLAIN, password=PPASS)
    assert _status(root) == "ok", f"{PLAIN} cannot authenticate"
    print(f"SETUP created/reset non-admin user {PLAIN!r}")


def teardown_plain_user():
    _get("updateUser.view", {"username": PLAIN, "disabled": "true"})
    print(f"SETUP disabled {PLAIN!r}")


# ---- Tests -------------------------------------------------------------


def test_update_song_needs_permission():
    """updateSong rewrites tags on disk and had no role check at all.

    Any account could retag any file in any root by guessing a song id, and
    TagLib rewrites the container, so it was a corruption vector as much as a
    metadata one.
    """
    song_id, _ = _first_song_and_folder()
    _, root = _get("updateSong.view", {"id": song_id, "title": "authz probe"},
                   user=PLAIN, password=PPASS)
    assert _status(root) == "failed" and _error_code(root) == "50", (
        "a non-admin was allowed to retag a shared-library file: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  updateSong refuses a non-admin on the shared library")


def test_update_song_still_works_for_admin():
    """The gate must not have locked out the people who are meant to use it."""
    song_id, _ = _first_song_and_folder()
    _, root = _get("getSong.view", {"id": song_id})
    original = root.find(f"{{{NS}}}song").get("title")
    _, root = _get("updateSong.view", {"id": song_id, "title": original})
    assert _status(root) == "ok", (
        "admin can no longer use updateSong: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  updateSong still works for an admin")


def test_set_cover_art_needs_permission():
    """setCoverArt writes cover.jpg into a folder and had no role check.

    It is a POST, so it is built by hand here rather than through _get.
    """
    _, folder_id = _first_song_and_folder()
    p = {"u": PLAIN, "p": PPASS, "v": VER, "c": CLIENT, "f": "xml",
         "id": folder_id, "url": "http://example.com/x.jpg"}
    url = f"{BASE}/setCoverArt.view?{urllib.parse.urlencode(p)}"
    req = urllib.request.Request(url, data=b"", method="POST")
    try:
        with urllib.request.urlopen(req) as r:
            root = ET.fromstring(r.read())
    except urllib.error.HTTPError as e:
        raise AssertionError(f"expected 200, got HTTP {e.code}") from None
    assert _status(root) == "failed" and _error_code(root) == "50", (
        "a non-admin was allowed to set cover art on the shared library: "
        + ET.tostring(root).decode()
    )
    print("PASS  setCoverArt refuses a non-admin on the shared library")


def test_get_album_reports_writability():
    """The Edit affordance is drawn from this flag, not from the caller's roles.

    Without it the web client offered Edit on every album to everybody and let
    the save fail - the same gap as updateSong above, one screen earlier. A
    role test cannot stand in for it: uploadRole makes an account's own
    uploads writable and the shared library not, so the answer differs per
    album rather than per account.
    """
    _, folder_id = _first_song_and_folder()
    _, root = _get("getAlbum.view", {"id": folder_id},
                   user=PLAIN, password=PPASS)
    album = root.find(f"{{{NS}}}album") if root is not None else None
    assert album is not None, (
        "getAlbum refused an ordinary user on the shared library: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    assert album.get("writable") == "false", (
        "getAlbum told a non-admin it may edit the shared library: "
        + ET.tostring(album).decode()
    )
    print("PASS  getAlbum reports the shared library unwritable to a non-admin")

    # And the gate must not have shut out the people it is for.
    _, root = _get("getAlbum.view", {"id": folder_id})
    album = root.find(f"{{{NS}}}album") if root is not None else None
    assert album is not None and album.get("writable") == "true", (
        "getAlbum denies an admin the Edit link: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  getAlbum reports the shared library writable to an admin")


def test_album_info_force_needs_admin():
    """force= re-queries MusicBrainz over a cache the whole server shares.

    It had no role check, so any account could spend the one paced provider
    gate every other pane waits behind, and overwrite cached notes nobody
    asked to have refreshed.
    """
    _, folder_id = _first_song_and_folder()
    _, root = _get("getAlbumInfo2.view", {"id": folder_id, "force": "1"},
                   user=PLAIN, password=PPASS)
    assert _status(root) == "failed" and _error_code(root) == "50", (
        "a non-admin was allowed to force an album lookup: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  getAlbumInfo2 refuses force= from a non-admin")

    # The read itself stays open - the gate is on the re-ask alone, and a
    # non-admin who can no longer see album notes would be a worse bug.
    _, root = _get("getAlbumInfo2.view", {"id": folder_id},
                   user=PLAIN, password=PPASS)
    assert _status(root) == "ok", (
        "the force gate also blocked an ordinary read: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  getAlbumInfo2 still reads for a non-admin without force=")


def test_artist_info_force_needs_admin():
    """The artist half of the same hole, reached from the artist pane."""
    _, root = _get("getIndexes.view")
    artist = root.find(f".//{{{NS}}}artist") if root is not None else None
    assert artist is not None, "no artists; this test needs a non-empty library"
    _, root = _get("getArtistInfo2.view", {"id": artist.get("id"),
                                           "force": "1"},
                   user=PLAIN, password=PPASS)
    assert _status(root) == "failed" and _error_code(root) == "50", (
        "a non-admin was allowed to force an artist lookup: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  getArtistInfo2 refuses force= from a non-admin")


def test_cover_art_url_refuses_private_addresses():
    """setCoverArt's url= made the server fetch anything it could reach.

    No host allow-list, no private-IP block, and redirects followed - so it
    reached 127.0.0.1, link-local metadata and the whole LAN, and its two
    distinct error messages made it a working port scanner. Run as admin,
    because the permission gate above would otherwise be what refuses it.
    """
    for target in ("http://127.0.0.1:4040/rest/ping.view",
                   "http://169.254.169.254/latest/meta-data/",
                   "http://[::1]:4040/",
                   "http://10.0.0.1/x.jpg"):
        _, folder_id = _first_song_and_folder()
        p = {"u": ADMIN, "p": APASS, "v": VER, "c": CLIENT, "f": "xml",
             "id": folder_id, "url": target}
        url = f"{BASE}/setCoverArt.view?{urllib.parse.urlencode(p)}"
        req = urllib.request.Request(url, data=b"", method="POST")
        with urllib.request.urlopen(req) as r:
            root = ET.fromstring(r.read())
        assert _status(root) == "failed", (
            f"the server was willing to fetch {target}: "
            + ET.tostring(root).decode()
        )
    print("PASS  setCoverArt url= refuses loopback, link-local and RFC1918")


def test_playlist_is_not_readable_by_id():
    """get_playlist's query was `WHERE p.id = ?` with no owner predicate.

    getPlaylists correctly hides other people's playlists from the listing,
    which made this an enumeration away rather than a link away - and playlist
    ids are small sequential integers.
    """
    _, root = _get("createPlaylist.view", {"name": "authz private playlist"})
    assert _status(root) == "ok", "could not create a playlist as admin"
    _, root = _get("getPlaylists.view")
    playlists = root.find(f"{{{NS}}}playlists")
    mine = [p for p in playlists
            if p.get("name") == "authz private playlist"]
    assert mine, "the playlist just created is not in the admin's listing"
    pid = mine[0].get("id")

    _, root = _get("getPlaylist.view", {"id": pid}, user=PLAIN, password=PPASS)
    assert _status(root) == "failed", (
        "a non-admin read another user's private playlist by id: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  getPlaylist refuses another user's private playlist")

    # And the owner must still be able to read it.
    _, root = _get("getPlaylist.view", {"id": pid})
    assert _status(root) == "ok", "the owner can no longer read their playlist"
    print("PASS  getPlaylist still works for the owner")
    _get("deletePlaylist.view", {"id": pid})


def test_shared_library_is_still_readable():
    """The uploads gate must not have made the shared library private."""
    song_id, folder_id = _first_song_and_folder()
    for endpoint, extra in (("getSong.view", {"id": song_id}),
                            ("getMusicDirectory.view", {"id": folder_id}),
                            ("getArtists.view", {})):
        _, root = _get(endpoint, extra, user=PLAIN, password=PPASS)
        assert _status(root) == "ok", (
            f"{endpoint} now refuses an ordinary user: "
            + (ET.tostring(root).decode() if root is not None else "no response")
        )
    print("PASS  an ordinary user can still browse the shared library")


def test_disabled_account_is_refused():
    """Unchanged behaviour, checked because the throttle rewrote this path."""
    _get("updateUser.view", {"username": PLAIN, "disabled": "true"})
    _, root = _get("ping.view", user=PLAIN, password=PPASS)
    assert _status(root) == "failed", "a disabled account authenticated"
    _get("updateUser.view", {"username": PLAIN, "disabled": "false"})
    print("PASS  a disabled account is refused")


def test_bad_username_refused():
    """A username is a directory component in the uploads tree.

    A '/' in one wrote outside the user's own area, with no
    path_is_within_root() on that join, and silently broke the ownership
    comparisons deleteUpload and moveAlbum make.
    """
    for bad in ("a/b", "..", "a\\b", "a b"):
        _, root = _get("createUser.view", {"username": bad,
                                           "password": "irrelevant"})
        assert _status(root) == "failed", (
            f"a user named {bad!r} was created: "
            + (ET.tostring(root).decode() if root is not None else "no response")
        )
    print("PASS  a username that is not a safe path component is refused")


def test_start_info_lookup_needs_admin():
    """startInfoLookup spends hours of the server's MusicBrainz budget.

    One account being able to saturate the single paced gate would make every
    other account's artist and album panes crawl for as long as the pass ran,
    and there is no way to stop it short of a restart.
    """
    _, root = _get("startInfoLookup.view", {"what": "artists"},
                   user=PLAIN, password=PPASS)
    assert _status(root) == "failed" and _error_code(root) == "50", (
        "a non-admin was allowed to start a library-wide metadata lookup: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  startInfoLookup refuses a non-admin")


def test_start_info_lookup_rejects_a_bad_what():
    """An unrecognised `what` must not be promoted to "everything".

    Run as admin, and deliberately *not* followed by a valid call: a passing
    run of this file must not leave an overnight pass queued behind it.
    """
    _, root = _get("startInfoLookup.view", {"what": "artits"})
    assert _status(root) == "failed" and _error_code(root) == "0", (
        "an invalid `what` was accepted: "
        + (ET.tostring(root).decode() if root is not None else "no response")
    )
    print("PASS  startInfoLookup rejects an unknown `what`")


TESTS = [
    test_update_song_needs_permission,
    test_update_song_still_works_for_admin,
    test_set_cover_art_needs_permission,
    test_get_album_reports_writability,
    test_album_info_force_needs_admin,
    test_artist_info_force_needs_admin,
    test_cover_art_url_refuses_private_addresses,
    test_playlist_is_not_readable_by_id,
    test_shared_library_is_still_readable,
    test_disabled_account_is_refused,
    test_bad_username_refused,
    test_start_info_lookup_needs_admin,
    test_start_info_lookup_rejects_a_bad_what,
]

if __name__ == "__main__":
    setup_plain_user()
    failed = 0
    for t in TESTS:
        try:
            t()
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1
    teardown_plain_user()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

#!/usr/bin/env python3
"""Visibility tests: tags on level-1 folders decide who sees what.

Needs an admin account and at least three artist (or category) folders that
each hold a song. The script picks three, A, B and C, and sets up

  * tag "vistest-explicit", default blocked, on A; adult's cell choice_on
  * tag "vistest-jazz",     default choice_off, on B; partner's cell shown
  * tag "vistest-kids",     default choice_on, on C
  * accounts vistest-adult, vistest-partner and vistest-kid, the last with
    blockUntagged

which should come out as

  * admin    sees A, B, C
  * adult    sees A and C; B is soft-hidden (gone from listings, still
             readable by id)
  * partner  sees B and C; A is hard-hidden (refused by id as well)
  * kid      sees C only; A and B are hard-hidden

Subsonic has no deleteUser, so the accounts are disabled on the way out, and
the tags deleted.

Start the server first, then:
    python3 tests/test_visibility.py
"""

import json
import sys
import urllib.parse
import urllib.request
from server_url import BASE

ADMIN  = "admin"
APASS  = "secret"
VER    = "1.16.1"
CLIENT = "test"
PASS   = "vistest-password"

ADULT, PARTNER, KID = "vistest-adult", "vistest-partner", "vistest-kid"


def _get(endpoint, extra=None, *, user=ADMIN, password=APASS):
    p = [("u", user), ("p", password), ("v", VER), ("c", CLIENT), ("f", "json")]
    if extra:
        p += list(extra.items()) if isinstance(extra, dict) else extra
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        return json.loads(r.read())["subsonic-response"]


def _ok(r, what):
    assert r.get("status") == "ok", f"{what}: {r}"
    return r


def _code(r):
    return r.get("error", {}).get("code") if r.get("status") != "ok" else None


# ---- Setup -------------------------------------------------------------

STATE = {}


def _song_in(folder_id):
    """Any song id at or below a folder, as admin, or None."""
    todo = [folder_id]
    while todo:
        d = _get("getMusicDirectory.view", {"id": todo.pop()})
        for c in d.get("directory", {}).get("child", []):
            if not c.get("isDir"):
                return c["id"]
            todo.append(c["id"])
    return None


def _pick_folders():
    arts = _ok(_get("getArtists.view"), "getArtists")["artists"].get("index", [])
    picked = []
    for idx in arts:
        for a in idx.get("artist", []):
            song = _song_in(a["id"])
            if song:
                picked.append({"id": a["id"], "name": a["name"], "song": song})
            if len(picked) == 3:
                return picked
    raise AssertionError("this test needs three level-1 folders holding a song")


def _tag(name, default):
    r = _get("createTag.view", {"name": name, "defaultState": default})
    if r.get("status") != "ok":   # left over from an interrupted run
        for t in _ok(_get("getTags.view"), "getTags")["tags"]["tag"]:
            if t["name"].lower() == name.lower():
                _ok(_get("updateTag.view", {"id": t["id"], "defaultState": default}),
                    "updateTag")
                return t["id"]
        raise AssertionError(f"createTag {name}: {r}")
    return r["tag"]["id"]


def setup():
    a, b, c = _pick_folders()
    STATE.update(A=a, B=b, C=c)
    for u in (ADULT, PARTNER, KID):
        _get("createUser.view", {"username": u, "password": PASS})
        _ok(_get("updateUser.view", {"username": u, "password": PASS,
                                     "adminRole": "false", "disabled": "false",
                                     "blockUntagged": "true" if u == KID else "false"}),
            f"updateUser {u}")
    explicit = _tag("vistest-explicit", "blocked")
    jazz     = _tag("vistest-jazz",     "choice_off")
    kids     = _tag("vistest-kids",     "choice_on")
    STATE.update(tags=[explicit, jazz, kids])
    _ok(_get("setFolderTags.view", [("id", a["id"]), ("tagId", explicit)]), "tag A")
    _ok(_get("setFolderTags.view", [("id", b["id"]), ("tagId", jazz)]),     "tag B")
    _ok(_get("setFolderTags.view", [("id", c["id"]), ("tagId", kids)]),     "tag C")
    _ok(_get("setUserTag.view", {"username": ADULT, "tagId": explicit,
                                 "state": "choice_on"}), "adult cell")
    _ok(_get("setUserTag.view", {"username": PARTNER, "tagId": jazz,
                                 "state": "shown"}), "partner cell")
    print(f"SETUP A={a['name']!r} B={b['name']!r} C={c['name']!r}")


def teardown():
    for t in STATE.get("tags", []):
        _get("deleteTag.view", {"id": t})
    for u in (ADULT, PARTNER, KID):
        _get("updateUser.view", {"username": u, "blockUntagged": "false",
                                 "disabled": "true"})
    print("SETUP deleted the vistest tags, disabled the vistest accounts")


# What each account should get: True visible, "soft", or "hard".
EXPECT = {
    ADMIN:   {"A": True,   "B": True,   "C": True},
    ADULT:   {"A": True,   "B": "soft", "C": True},
    PARTNER: {"A": "hard", "B": True,   "C": True},
    KID:     {"A": "hard", "B": "hard", "C": True},
}


def _as(user):
    return {"user": user, "password": APASS if user == ADMIN else PASS}


# ---- Tests -------------------------------------------------------------


def test_artist_listings():
    """getArtists and getIndexes list exactly the visible folders."""
    for user, want in EXPECT.items():
        for ep, key in (("getArtists.view", "artists"), ("getIndexes.view", "indexes")):
            r = _ok(_get(ep, **_as(user)), f"{ep} as {user}")
            ids = {a["id"] for i in r[key].get("index", []) for a in i.get("artist", [])}
            for k, v in want.items():
                assert (STATE[k]["id"] in ids) == (v is True), (
                    f"{ep} as {user}: folder {k} listed={STATE[k]['id'] in ids}, "
                    f"expected {v}")
    print("PASS  getArtists and getIndexes follow the tags")


def test_root_directory_listing():
    """getMusicDirectory on a root lists only the visible level-1 folders."""
    roots = _ok(_get("getMusicFolders.view"), "getMusicFolders")
    roots = roots["musicFolders"].get("musicFolder", [])
    for user, want in EXPECT.items():
        ids = set()
        for root in roots:
            d = _get("getMusicDirectory.view", {"id": root["id"]}, **_as(user))
            ids |= {c["id"] for c in d.get("directory", {}).get("child", [])}
        for k, v in want.items():
            assert (STATE[k]["id"] in ids) == (v is True), (
                f"root listing as {user}: folder {k} listed={STATE[k]['id'] in ids}, "
                f"expected {v}")
    print("PASS  a root's directory listing follows the tags")


def test_search():
    """search3 finds an artist only where it is visible."""
    for user, want in EXPECT.items():
        for k, v in want.items():
            r = _ok(_get("search3.view", {"query": STATE[k]["name"], "artistCount": 50,
                                          "albumCount": 0, "songCount": 0},
                         **_as(user)), f"search3 as {user}")
            ids = {a["id"] for a in r["searchResult3"].get("artist", [])}
            assert (STATE[k]["id"] in ids) == (v is True), (
                f"search3 as {user}: folder {k} found={STATE[k]['id'] in ids}, "
                f"expected {v}")
    print("PASS  search3 follows the tags")


def test_album_list():
    """getAlbumList2 leaves out albums of hidden folders."""
    for user, want in EXPECT.items():
        r = _ok(_get("getAlbumList2.view", {"type": "alphabeticalByName", "size": 500},
                     **_as(user)), f"getAlbumList2 as {user}")
        parents = {al.get("artistId") for al in r["albumList2"].get("album", [])}
        for k, v in want.items():
            if v is not True:
                assert STATE[k]["id"] not in parents, (
                    f"getAlbumList2 as {user} lists an album of hidden folder {k}")
    print("PASS  getAlbumList2 leaves out hidden folders")


def test_read_by_id():
    """Hard-hidden is refused by id with 70; soft-hidden still reads."""
    for user, want in EXPECT.items():
        for k, v in want.items():
            for ep, i in (("getMusicDirectory.view", STATE[k]["id"]),
                          ("getSong.view", STATE[k]["song"])):
                r = _get(ep, {"id": i}, **_as(user))
                if v == "hard":
                    assert _code(r) == 70, f"{ep} {k} as {user} should be 70: {r}"
                else:
                    assert r.get("status") == "ok", f"{ep} {k} as {user} refused: {r}"
    print("PASS  reads by id refuse hard-hidden and serve soft-hidden")


def test_tag_admin_is_admin_only():
    """An ordinary account may not see or change the grid."""
    for ep, extra in (("getTags.view", {}),
                      ("createTag.view", {"name": "vistest-x", "defaultState": "shown"}),
                      ("setUserTag.view", {"username": ADULT, "tagId": STATE["tags"][0],
                                           "state": "shown"})):
        r = _get(ep, extra, **_as(ADULT))
        assert _code(r) == 50, f"{ep} as a non-admin: {r}"
    print("PASS  the tag endpoints refuse a non-admin")


def test_personal_tags():
    """Every account has a [username] admin tag that cannot be deleted."""
    tags = _ok(_get("getTags.view"), "getTags")["tags"]["tag"]
    personal = {t.get("personalOf"): t for t in tags if t.get("personalOf")}
    for u in (ADMIN, ADULT, PARTNER, KID):
        assert u in personal, f"no personal tag for {u}"
        assert personal[u].get("defaultState") == "choice_off", personal[u]
    r = _get("deleteTag.view", {"id": personal[ADULT]["id"]})
    assert r.get("status") != "ok", "a personal tag was deleted"
    print("PASS  every account has a personal tag, and it stays")


TESTS = [
    test_artist_listings,
    test_root_directory_listing,
    test_search,
    test_album_list,
    test_read_by_id,
    test_tag_admin_is_admin_only,
    test_personal_tags,
]

if __name__ == "__main__":
    setup()
    failed = 0
    try:
        for t in TESTS:
            try:
                t()
            except Exception as e:
                print(f"FAIL  {t.__name__}: {e}")
                failed += 1
    finally:
        teardown()
    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

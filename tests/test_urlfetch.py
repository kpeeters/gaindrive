#!/usr/bin/env python3
"""URL-fetch endpoint tests: getUrlHandlers, fetchUrl, getFetchJobs,
cancelFetch.

Most of this needs no network and no yt-dlp: the refusals are the part worth
regression-testing, because each of them is the security boundary rather than a
convenience. A URL matching no handler must be refused, or an account with
upload rights can make the server issue requests to anywhere it can reach.

The server must have an uploads root configured, and the account below must
have upload rights (or be an admin).

    ./build/gaindrive --db /tmp/gd_test.db --artist-root music=/music \\
                      --upload-root personal=/tmp/gd_uploads --no-scan

    python3 tests/test_urlfetch.py

Set LIVE_URL to a real, short video to exercise the online half — an actual
fetch, a promote and a cancel. It is None by default because it downloads,
takes minutes and needs yt-dlp installed on the server.
"""

import io
import json
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
import zipfile

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"

# e.g. "https://www.youtube.com/watch?v=aqz-KE-bpKQ"
LIVE_URL = None

# A second account, created and removed by the permission test.
OTHER_USER = "urlfetch_nobody"
OTHER_PASS = "nobody"


class Skip(Exception):
    """Precondition absent — reported as a skip rather than a failure."""


def _get(endpoint, extra=None, *, user=USER, password=PASS):
    p = {"u": user, "p": password, "v": VER, "c": CLIENT, "f": "json"}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url) as r:
        return json.loads(r.read())["subsonic-response"]


def _ok(sr):
    assert sr["status"] == "ok", f"Expected ok, got {sr}"
    return sr


def _err(sr, code):
    assert sr["status"] == "failed", f"Expected failure, got {sr}"
    got = sr.get("error", {}).get("code")
    assert got == code, f"Expected error {code}, got {got}: {sr}"
    return sr["error"].get("message", "")


def _handlers():
    sr = _ok(_get("getUrlHandlers.view"))
    return sr.get("urlHandlers", {}).get("urlHandler", [])


def _require_handlers():
    hs = _handlers()
    if not hs:
        raise Skip("no URL handlers configured on this server")
    return hs


# ---- offline ----------------------------------------------------------

def test_handlers_well_formed():
    for h in _handlers():
        assert isinstance(h.get("name"), str) and h["name"], h
        assert isinstance(h.get("audio"), bool), h
        assert isinstance(h.get("video"), bool), h
        # A handler that can do neither should never have been kept.
        assert h["audio"] or h["video"], h
        # The pattern and the argv must not be reported: an argv can carry
        # --cookies, a proxy credential or an API key.
        assert "match" not in h and "audio_argv" not in h, h
    print("PASS  getUrlHandlers is well formed and leaks no configuration")


def test_missing_url():
    _require_handlers()
    _err(_get("fetchUrl.view"), 10)
    print("PASS  fetchUrl with no url returns error 10")


def test_scheme_refused():
    _require_handlers()
    msg = _err(_get("fetchUrl.view", {"url": "file:///etc/passwd"}), 0)
    # The message must name the scheme, not the table: a pattern is never
    # consulted for something refused outright.
    assert "http" in msg.lower(), f"Expected a scheme message, got {msg!r}"
    print("PASS  a file:// URL is refused by scheme")


def test_no_handler_refused():
    _require_handlers()
    # The SSRF regression test, and the reason the refusal exists at all.
    msg = _err(_get("fetchUrl.view",
                    {"url": "http://169.254.169.254/latest/meta-data/"}), 0)
    assert "handler" in msg.lower(), f"Expected a no-handler message, got {msg!r}"
    print("PASS  a URL matching no handler is refused")


def test_pattern_is_anchored():
    _require_handlers()
    # regex_search would accept this — the site name is in the fragment of a
    # URL pointing somewhere else entirely. regex_match must not.
    _err(_get("fetchUrl.view",
              {"url": "http://192.0.2.1/x#https://www.youtube.com/watch?v=a"}), 0)
    print("PASS  a site name in a fragment does not satisfy a pattern")


def test_blank_names_are_not_an_error():
    """Absent and all-whitespace must both mean "keep what the handler chose",
    or a client cannot send the two fields unconditionally.

    The assertion is that the request still fails for the *URL* — proving the
    names were accepted and that they are checked after the handler, not
    before."""
    _require_handlers()
    for extra in ({}, {"artist": "", "album": ""},
                  {"artist": "   ", "album": "\t"}):
        p = {"url": "http://192.0.2.1/nothing"}
        p.update(extra)
        msg = _err(_get("fetchUrl.view", p), 0)
        assert "handler" in msg.lower(), \
            f"{extra} was rejected for the names, not the URL: {msg!r}"
    print("PASS  blank names are accepted, and checked after the URL")


def test_unusable_names_refused():
    """A name that is not blank but sanitises away to nothing was typed and is
    wrong. moveAlbum takes the same line at the same question: filing it
    under "Unknown" would hide the mistake."""
    _require_handlers()
    for field in ("artist", "album"):
        for bad in ("...", "..", ".", "///", "   ...   ", '<>:*?"'):
            msg = _err(_get("fetchUrl.view",
                            {"url": "http://192.0.2.1/nothing", field: bad}), 10)
            assert field in msg.lower(), \
                f"error 10 for {field}={bad!r} did not name the field: {msg!r}"
    print("PASS  a name that sanitises to nothing is error 10, naming the field")


def test_control_characters_stripped():
    """A directory name is echoed into the log and written into an XML
    attribute, and nothing escapes a raw C0 byte in either. A name that is
    *only* control characters must therefore be refused like any other name
    that sanitises away."""
    _require_handlers()
    _err(_get("fetchUrl.view",
              {"url": "http://192.0.2.1/nothing", "artist": "\n\r\t"}), 10)
    # One that survives, to prove the stripping is not simply a rejection of
    # anything containing a control character.
    msg = _err(_get("fetchUrl.view",
                    {"url": "http://192.0.2.1/nothing",
                     "artist": "Clapton\nLive"}), 0)
    assert "handler" in msg.lower(), \
        f"a name with an embedded newline was rejected outright: {msg!r}"
    print("PASS  control characters are stripped, not treated as fatal")


def test_cancel_unknown_id():
    _require_handlers()
    _err(_get("cancelFetch.view", {"id": "not-a-real-job"}), 70)
    _err(_get("cancelFetch.view"), 10)
    print("PASS  cancelFetch reports an unknown id as 70")


def test_jobs_list_shape():
    _require_handlers()
    sr = _ok(_get("getFetchJobs.view"))
    jobs = sr.get("fetchJobs", {}).get("fetchJob", [])
    assert isinstance(jobs, list), sr
    print(f"PASS  getFetchJobs returns a list ({len(jobs)} job(s))")


def test_permission_and_isolation():
    _require_handlers()
    admin = _ok(_get("getUser.view", {"username": USER}))["user"]
    if not admin.get("adminRole"):
        raise Skip("the test account is not an admin, so it cannot create one")

    # Idempotent rather than create-and-remove: there is no deleteUser
    # endpoint, so the account survives the run and has to be reusable. It is
    # left disabled, which is why enabling it again is the first thing here.
    _get("createUser.view", {"username": OTHER_USER,
                             "password": OTHER_PASS,
                             "uploadRole": "false"})
    _ok(_get("updateUser.view", {"username": OTHER_USER,
                                 "password": OTHER_PASS,
                                 "uploadRole": "false",
                                 "adminRole": "false",
                                 "disabled": "false"}))
    try:
        for ep in ("getUrlHandlers.view", "fetchUrl.view",
                   "getFetchJobs.view", "cancelFetch.view"):
            _err(_get(ep, user=OTHER_USER, password=OTHER_PASS), 50)
        print("PASS  an account without upload rights is refused (50)")

        _ok(_get("updateUser.view", {"username": OTHER_USER,
                                     "uploadRole": "true"}))
        sr = _ok(_get("getFetchJobs.view",
                      user=OTHER_USER, password=OTHER_PASS))
        jobs = sr.get("fetchJobs", {}).get("fetchJob", [])
        assert jobs == [], f"Expected no jobs for a fresh user, got {jobs}"
        print("PASS  a user sees no other user's jobs")
    finally:
        try:
            _ok(_get("updateUser.view", {"username": OTHER_USER,
                                         "uploadRole": "false",
                                         "disabled": "true"}))
        except Exception as e:
            print(f"NOTE  could not disable {OTHER_USER}: {e}")


# ---- online -----------------------------------------------------------

def test_live_fetch():
    if LIVE_URL is None:
        raise Skip("LIVE_URL is not set")
    hs = _require_handlers()
    if not any(h["audio"] for h in hs):
        raise Skip("no handler offers audio")

    before = _ok(_get("getArtists.view", {"personal": "true"}))
    before_names = {a["name"]
                    for i in before.get("artists", {}).get("index", [])
                    for a in i.get("artist", [])}

    job = _ok(_get("fetchUrl.view", {"url": LIVE_URL, "mode": "audio"}))["fetchJob"]
    jid, batch = job["id"], job["batch"]
    assert job["state"] == "queued", job

    deadline = time.time() + 300
    last_pct = 0
    state = "queued"
    while time.time() < deadline:
        sr = _ok(_get("getFetchJobs.view"))
        jobs = sr.get("fetchJobs", {}).get("fetchJob", [])
        me = next((j for j in jobs if j["id"] == jid), None)
        assert me is not None, f"Job {jid} disappeared"
        state = me["state"]

        # The bar must never go backwards. A line carrying no percentage has to
        # keep the previous value, or the whole post-processing phase reads as
        # a restart.
        assert me["percent"] >= last_pct, \
            f"percent went backwards: {last_pct} -> {me['percent']}"
        last_pct = me["percent"]

        # A root path is never surfaced in an API response, and a tool's
        # progress line names the file it is writing.
        d = me.get("detail", "")
        if "/" in d:
            assert d.startswith(batch) or batch in d, \
                f"detail leaks a path outside the batch: {d!r}"

        if state in ("done", "error", "cancelled"):
            break
        time.sleep(2)

    assert state == "done", f"Fetch did not finish cleanly: {state}"
    print(f"PASS  live fetch completed ({last_pct}%)")

    # No sleep here on purpose: the worker scans before it reports done, so
    # the listing must already be right.
    after = _ok(_get("getArtists.view", {"personal": "true"}))
    after_names = {a["name"]
                   for i in after.get("artists", {}).get("index", [])
                   for a in i.get("artist", [])}
    assert after_names - before_names, \
        "done was reported but the personal listing did not change"
    print("PASS  the library is correct the moment the job says done")


def test_live_cancel():
    if LIVE_URL is None:
        raise Skip("LIVE_URL is not set")
    _require_handlers()

    job = _ok(_get("fetchUrl.view", {"url": LIVE_URL, "mode": "video"}))["fetchJob"]
    jid = job["id"]
    time.sleep(2)
    _ok(_get("cancelFetch.view", {"id": jid}))

    deadline = time.time() + 30
    while time.time() < deadline:
        sr = _ok(_get("getFetchJobs.view"))
        me = next((j for j in sr.get("fetchJobs", {}).get("fetchJob", [])
                   if j["id"] == jid), None)
        if me and me["state"] == "cancelled":
            print("PASS  a running fetch can be cancelled")
            return
        time.sleep(1)
    raise AssertionError("cancelled job never reached the cancelled state")


def test_live_fetch_with_names():
    """The whole point: a typed name is what the batch is filed under, and the
    title parsing is not consulted.

    A merge test proper — two directories at one level collapsing into the typed
    name — needs a playlist, and the built-in handler passes --no-playlist, so
    it cannot be exercised here without an operator-written handler."""
    if LIVE_URL is None:
        raise Skip("LIVE_URL is not set")
    _require_handlers()
    artist, album = "GD Test Artist", "GD Test Album"

    job = _ok(_get("fetchUrl.view", {"url": LIVE_URL, "mode": "audio",
                                     "artist": artist,
                                     "album": album}))["fetchJob"]
    assert job["artist"] == artist and job["album"] == album, job

    deadline, state = time.time() + 300, "queued"
    while time.time() < deadline:
        sr = _ok(_get("getFetchJobs.view"))
        me = next((j for j in sr.get("fetchJobs", {}).get("fetchJob", [])
                   if j["id"] == job["id"]), None)
        assert me is not None, f"job {job['id']} disappeared"
        state = me["state"]
        if state in ("done", "error", "cancelled"):
            break
        time.sleep(2)
    assert state == "done", f"fetch did not finish cleanly: {state}"

    # No sleep: the worker scans before it says done, so the listing is the
    # assertion and it must already be right.
    after = _ok(_get("getArtists.view", {"personal": "true"}))
    index = after.get("artists", {}).get("index", [])
    names = {a["name"] for i in index for a in i.get("artist", [])}
    assert artist in names, f"typed artist not in the personal listing: {names}"

    # Exactly one album, named as typed — and therefore still a five-component
    # path, which is the only thing that makes it promotable.
    aid = next(a["id"] for i in index for a in i.get("artist", [])
               if a["name"] == artist)
    d = _ok(_get("getMusicDirectory.view", {"id": aid}))["directory"]
    albums = [c["title"] for c in d.get("child", []) if c.get("isDir")]
    assert albums == [album], f"expected one album named {album!r}, got {albums}"
    print("PASS  typed names are what the batch is filed under")


def test_duplicate_refused():
    if LIVE_URL is None:
        raise Skip("LIVE_URL is not set")
    _require_handlers()
    job = _ok(_get("fetchUrl.view", {"url": LIVE_URL}))["fetchJob"]
    try:
        _err(_get("fetchUrl.view", {"url": LIVE_URL}), 0)
        print("PASS  the same URL twice is refused while one is in flight")
    finally:
        _get("cancelFetch.view", {"id": job["id"]})


# ---- Tool sidecars (no network) ---------------------------------------
#
# A fetch asks yt-dlp for a .info.json and the batch pipeline turns its
# `chapters` array into the sidecar gaindrive indexes. That conversion is keyed
# on the *file* rather than on which handler ran, so an uploaded archive takes
# the identical path — which is what makes it testable with no network, no
# yt-dlp and no waiting for a download.
#
# Not asserted here: that the .info.json is deleted afterwards. Nothing in the
# API lists arbitrary files in a folder, so that one is checked by hand.

UPLOAD_URL = BASE.rsplit("/rest", 1)[0] + "/upload"


def _upload_zip(entries):
    """POSTs a zip of {name: bytes|str} and returns the reply object."""
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name, data in entries.items():
            z.writestr(name, data)

    boundary = "----gaindrivetestboundary"
    body = (f"--{boundary}\r\n"
            'Content-Disposition: form-data; name="file"; filename="b.zip"\r\n'
            "Content-Type: application/zip\r\n\r\n").encode()
    body += buf.getvalue()
    body += f"\r\n--{boundary}--\r\n".encode()

    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    req = urllib.request.Request(
        f"{UPLOAD_URL}?{urllib.parse.urlencode(p)}", data=body)
    req.add_header("Content-Type",
                   f"multipart/form-data; boundary={boundary}")
    try:
        with urllib.request.urlopen(req) as r:
            return json.loads(r.read())
    except urllib.error.HTTPError as e:
        raise Skip(f"/upload returned HTTP {e.code} — is an uploads root "
                   f"configured and does {USER} have upload rights?") from None


def _await_song(title, timeout=30):
    """/upload answers before the scan finishes, so the listing lags."""
    for _ in range(timeout * 2):
        sr = _get("search3.view", {"query": title, "songCount": "20",
                                   "artistCount": "0", "albumCount": "0",
                                   "personal": "true"})
        for s in sr.get("searchResult3", {}).get("song", []):
            if s.get("title") == title:
                return s
        time.sleep(0.5)
    return None


def _chapter_pairs(song_id):
    sr = _get("getChapters.view", {"id": song_id})
    return ([(c["start"], c.get("name", ""))
             for c in sr.get("chapters", {}).get("chapter", [])],
            sr.get("chapters", {}).get("source"))


def _cleanup(song):
    """Best effort — the album folder, which is what deleteUpload accepts."""
    try:
        _get("deleteUpload.view", {"id": song["parent"]})
    except Exception:                                    # noqa: BLE001
        pass


def test_info_json_becomes_a_chapter_sidecar():
    """The whole point: a fetched set lists its songs with no manual step."""
    tag  = str(int(time.time()))
    name = f"Zqx Set {tag}"
    info = json.dumps({"title": name, "chapters": [
        {"start_time": 0.0,   "end_time": 815.0,  "title": "Opener"},
        {"start_time": 815.0, "end_time": 1142.0, "title": "Second Song"},
        {"start_time": 1142.0,                    "title": "Encore"}]})
    r = _upload_zip({f"Zqx Artist/{name}/{name}.opus": b"\0" * 4096,
                     f"Zqx Artist/{name}/{name}.info.json": info})
    assert r.get("status") == "ok", r

    song = _await_song(name)
    assert song, f"{name!r} never appeared in the personal listing"
    try:
        pairs, source = _chapter_pairs(song["id"])
        assert source == "sidecar", source
        assert pairs == [(0.0, "Opener"), (815.0, "Second Song"),
                         (1142.0, "Encore")], pairs

        # And it reached the browse index, which is what the album view reads.
        sr = _get("getAlbumChapters.view", {"id": song["parent"]})
        vids = sr.get("albumChapters", {}).get("song", [])
        assert any(v["id"] == song["id"] for v in vids), vids
        print(f"PASS  an info.json became {len(pairs)} chapters, indexed")
    finally:
        _cleanup(song)


def test_info_json_without_chapters_writes_nothing():
    """A video with none sends "chapters": null — the shape value() throws on,
    and an empty sidecar would be a tombstone the fetch has no right to set."""
    tag  = str(int(time.time())) + "b"
    name = f"Zqx Plain {tag}"
    info = json.dumps({"title": name, "chapters": None})
    r = _upload_zip({f"Zqx Artist/{name}/{name}.opus": b"\0" * 4096,
                     f"Zqx Artist/{name}/{name}.info.json": info})
    assert r.get("status") == "ok", r

    song = _await_song(name)
    assert song, f"{name!r} never appeared"
    try:
        pairs, source = _chapter_pairs(song["id"])
        assert pairs == [], pairs
        assert source == "none", f"source={source} — a tombstone was written"
        print("PASS  chapters:null writes no sidecar, not an empty one")
    finally:
        _cleanup(song)


def test_loose_sidecar_follows_its_media():
    """A sidecar's double extension must not strand it in an album of its own.

    std::filesystem splits on the last dot, so without split_sidecar_name()
    "Clip.chapters.txt" is filed under an album called "Clip.chapters" while
    "Clip.mp4" goes to "Clip", and the markers are never found. A video is used
    because reorganise_by_tags() claims loose *audio* before this runs.
    """
    tag  = str(int(time.time())) + "c"
    name = f"Zqx Clip {tag}"
    r = _upload_zip({f"{name}.mp4": b"\0" * 4096,
                     f"{name}.chapters.txt": "0:00 One\n5:00 Two\n"})
    assert r.get("status") == "ok", r

    song = _await_song(name)
    assert song, f"{name!r} never appeared"
    try:
        pairs, source = _chapter_pairs(song["id"])
        assert source == "sidecar", \
            f"source={source} — the sidecar did not follow its media"
        assert pairs == [(0.0, "One"), (300.0, "Two")], pairs
        print("PASS  a loose sidecar is filed with the media it describes")
    finally:
        _cleanup(song)


TESTS = [
    test_handlers_well_formed,
    test_missing_url,
    test_scheme_refused,
    test_no_handler_refused,
    test_pattern_is_anchored,
    test_blank_names_are_not_an_error,
    test_unusable_names_refused,
    test_control_characters_stripped,
    test_cancel_unknown_id,
    test_jobs_list_shape,
    test_permission_and_isolation,
    test_info_json_becomes_a_chapter_sidecar,
    test_info_json_without_chapters_writes_nothing,
    test_loose_sidecar_follows_its_media,
    test_live_fetch,
    test_live_fetch_with_names,
    test_duplicate_refused,
    test_live_cancel,
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
    ran = len(TESTS) - skipped
    print(f"\n{ran - failed}/{ran} passed, {skipped} skipped")
    sys.exit(failed)

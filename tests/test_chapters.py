#!/usr/bin/env python3
"""Chapter marker endpoints - getChapters and saveChapters.

A concert is one file, and these are the markers that say where each song
starts. They live in a sidecar `<stem>.chapters.txt` beside the video, never
inside the container, and nothing about them is held in either database.

Start the server first, against a collection containing at least one video:
    ./build/gaindrive --db /tmp/gd_test.db --artist-root music=/music

Then run:
    python3 tests/test_chapters.py

**This writes into your library.** It saves markers onto one video and puts the
original list back at the end. If that video had no sidecar to begin with, an
*empty* one is left behind, because an empty file is how "this video has no
chapters" is spelled and the API deliberately cannot delete it. That leftover
is harmless -- a video with no container chapters reads the same either way --
and the last line of the run names the video so it can be removed by hand.

The parser's own rules are covered separately, with no server, by
tests/test_chapters_parse.py.
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

NS = "http://subsonic.org/restapi"


def _url(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


def _raw(endpoint, extra=None, body=None, user=None, password=None):
    """Returns (status, body). Does not raise on 4xx/5xx."""
    p = {"u": user or USER, "p": password or PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    url = f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"
    data = body.encode("utf-8") if body is not None else None
    req = urllib.request.Request(url, data=data)
    if data is not None:
        req.add_header("Content-Type", "text/plain; charset=utf-8")
    try:
        with urllib.request.urlopen(req) as r:
            return r.status, r.read()
    except urllib.error.HTTPError as e:
        return e.code, e.read()


def _json(endpoint, extra=None, body=None, user=None, password=None):
    extra = dict(extra or {})
    extra["f"] = "json"
    status, raw = _raw(endpoint, extra, body, user, password)
    try:
        return json.loads(raw)["subsonic-response"]
    except (json.JSONDecodeError, KeyError):
        raise AssertionError(
            f"{endpoint}: HTTP {status}, not a Subsonic envelope: "
            f"{raw[:200]!r}") from None


def _save(song_id, text, **kw):
    return _json("saveChapters.view", {"id": song_id}, body=text, **kw)


def _get(song_id, **kw):
    return _json("getChapters.view", {"id": song_id}, **kw)


def _pairs(sr):
    """The (start, name) list out of a chapters response."""
    return [(c["start"], c.get("name", ""))
            for c in sr.get("chapters", {}).get("chapter", [])]


# Queried once on first use rather than at import: a server that is down, or
# running an older binary, should fail one test with a readable message instead
# of aborting the whole script.
_CACHE = {}


def _video():
    if "video" not in _CACHE:
        r = _json("getVideos.view")
        vs = r.get("videos", {}).get("video", [])
        _CACHE["video"] = vs[0] if vs else None
    return _CACHE["video"]


def _vid():
    v = _video()
    assert v, "no videos in the library - scan a collection with video first"
    return v["id"]


def _original():
    """The list the video started with, so every test can put it back."""
    if "original" not in _CACHE:
        _CACHE["original"] = _get(_vid())
    return _CACHE["original"]


def _restore():
    sr = _original()
    text = "".join(f"{c['start']} {c.get('name','')}\n".rstrip() + "\n"
                   for c in sr.get("chapters", {}).get("chapter", []))
    _save(_vid(), text)


# ---- getChapters ------------------------------------------------------

def test_get_chapters_shape():
    sr = _get(_vid())
    assert sr["status"] == "ok", sr
    c = sr.get("chapters")
    assert c is not None, sr
    assert c.get("source") in ("sidecar", "container", "none"), c.get("source")
    assert isinstance(c.get("writable"), bool), c.get("writable")
    assert isinstance(c.get("chapter", []), list), c
    print(f"PASS  getChapters: source={c['source']} writable={c['writable']} "
          f"{len(c.get('chapter', []))} marker(s)")


def test_get_chapters_accepts_audio():
    """Chapters are not a video feature.

    A DJ set or a mixtape is one file holding a dozen songs for exactly the
    reason a concert film is. An audio track with no sidecar answers with an
    empty list and source "none" -- and, importantly, without an ffprobe: the
    container fallback stays video-only, because a process spawn per call is
    affordable once per film and not once per audio track.
    """
    r = _json("getRandomSongs.view", {"size": "50"})
    songs = [s for s in r.get("randomSongs", {}).get("song", [])
             if not s.get("isVideo")]
    if not songs:
        print("SKIP  audio chapters: no audio songs found")
        return
    sr = _get(songs[0]["id"])
    assert sr["status"] == "ok", sr
    assert sr["chapters"]["source"] in ("sidecar", "none"), sr["chapters"]
    print(f"PASS  getChapters accepts an audio song "
          f"(source={sr['chapters']['source']})")


def test_get_chapters_rejects_unknown_id():
    sr = _get("999999999")
    assert sr["status"] == "failed" and sr["error"]["code"] == 70, sr
    print("PASS  getChapters on an unknown id is error 70")


def test_missing_id():
    sr = _json("getChapters.view")
    assert sr["status"] == "failed" and sr["error"]["code"] == 10, sr
    print("PASS  getChapters without id is error 10")


# ---- saveChapters -----------------------------------------------------

def test_save_and_read_back():
    if not _original()["chapters"]["writable"]:
        print("SKIP  save: not writable by this account")
        return
    body = ("0:00 Shine On You Crazy Diamond\n"
            "13:35 - Learning to Fly\n"
            "19:02 Pt. 1: High Hopes\n")
    sr = _save(_vid(), body)
    assert sr["status"] == "ok", sr
    got = _pairs(sr)
    want = [(0.0, "Shine On You Crazy Diamond"),
            (815.0, "Learning to Fly"),
            (1142.0, "Pt. 1: High Hopes")]
    assert got == want, got
    # The reply must describe the file, so a fresh read has to agree with it.
    assert _pairs(_get(_vid())) == want, _pairs(_get(_vid()))
    assert _get(_vid())["chapters"]["source"] == "sidecar"
    print("PASS  saveChapters writes, and getChapters agrees with the reply")


def test_round_trip_is_a_fixed_point():
    """Saving what was read must change nothing, or every save walks the file."""
    if not _original()["chapters"]["writable"]:
        print("SKIP  round trip: not writable")
        return
    _save(_vid(), "0:00 A\n13:35.250 B\n1:30:00.999 C\n")
    first = _pairs(_get(_vid()))
    text = "".join(f"{s} {n}\n" for s, n in first)
    _save(_vid(), text)
    second = _pairs(_get(_vid()))
    assert first == second, f"{first} -> {second}"
    print(f"PASS  round trip is a fixed point ({first})")


def test_duplicate_names_both_survive():
    """The case that decided the wire format.

    httplib's query parser silently drops an exact repeat of a whole key=value
    token, so repeated name= parameters would have lost one of these and
    renamed every marker after it. The body is the file itself precisely so
    that cannot happen.
    """
    if not _original()["chapters"]["writable"]:
        print("SKIP  duplicate names: not writable")
        return
    sr = _save(_vid(), "0:00 Encore\n13:35 Encore\n20:00 Encore\n")
    got = _pairs(sr)
    assert got == [(0.0, "Encore"), (815.0, "Encore"), (1200.0, "Encore")], got
    print("PASS  three markers all called Encore survive a save")


def test_awkward_titles():
    if not _original()["chapters"]["writable"]:
        print("SKIP  awkward titles: not writable")
        return
    sr = _save(_vid(), "0:00 a=b & c\n"
                       "1:00 Pt. 1: Two\n"
                       "2:00 Sigur Rós — Hoppípolla\n")
    got = [n for _, n in _pairs(sr)]
    assert got == ["a=b & c", "Pt. 1: Two", "Sigur Rós — Hoppípolla"], got
    print("PASS  titles carrying =, a colon and non-ASCII round-trip intact")


def test_unsorted_input_comes_back_sorted():
    if not _original()["chapters"]["writable"]:
        print("SKIP  sorting: not writable")
        return
    got = _pairs(_save(_vid(), "13:35 Second\n0:00 First\n"))
    assert got == [(0.0, "First"), (815.0, "Second")], got
    print("PASS  an out-of-order file comes back sorted")


def test_unparseable_line_loses_only_itself():
    if not _original()["chapters"]["writable"]:
        print("SKIP  skipped lines: not writable")
        return
    got = _pairs(_save(_vid(), "0:00 First\nnot a marker\n13:35 Second\n"))
    assert got == [(0.0, "First"), (815.0, "Second")], got
    print("PASS  a stray line is skipped without losing the file")


def test_duration_is_derived():
    """The gap to the next marker, and the video's own duration for the last."""
    if not _original()["chapters"]["writable"]:
        print("SKIP  duration: not writable")
        return
    sr = _save(_vid(), "0:00 A\n0:30 B\n")
    ch = sr["chapters"]["chapter"]
    assert ch[0]["duration"] == 30, ch[0]
    assert isinstance(ch[1]["duration"], int) and ch[1]["duration"] >= 0, ch[1]
    print(f"PASS  durations derived: {[c['duration'] for c in ch]}")


def test_too_many_is_refused():
    if not _original()["chapters"]["writable"]:
        print("SKIP  cap: not writable")
        return
    body = "".join(f"0:{i % 60:02d} M{i}\n" for i in range(1200))
    sr = _save(_vid(), body)
    assert sr["status"] == "failed", sr
    print(f"PASS  1200 markers refused: {sr['error']['message']}")


def test_empty_body_is_a_tombstone():
    """An empty save leaves an empty file, not no file.

    The sidecar is what overrules a rip's own container chapters, so deleting
    it on an empty save would make those reappear and leave no way at all to
    say a film has none.
    """
    if not _original()["chapters"]["writable"]:
        print("SKIP  tombstone: not writable")
        return
    sr = _save(_vid(), "")
    assert sr["status"] == "ok", sr
    assert _pairs(sr) == [], _pairs(sr)
    after = _get(_vid())
    assert _pairs(after) == [], _pairs(after)
    assert after["chapters"]["source"] == "sidecar", \
        f"source={after['chapters']['source']} - the tombstone was not kept"
    print("PASS  an empty save leaves an empty sidecar, not the container list")


# ---- permissions ------------------------------------------------------

def test_xml_and_json_agree():
    j = _pairs(_get(_vid()))
    status, raw = _raw("getChapters.view", {"id": _vid(), "f": "xml"})
    import xml.etree.ElementTree as ET
    root = ET.fromstring(raw)
    x = [(float(e.get("start")), e.get("name", ""))
         for e in root.iter(f"{{{NS}}}chapter")]
    assert x == j, f"xml {x} != json {j}"
    print(f"PASS  the XML and JSON responses agree ({len(x)} marker(s))")


def test_album_texts_does_not_list_the_sidecar():
    """getAlbumTexts lists every .txt in a folder as liner notes."""
    v = _video()
    if not v or not v.get("parent"):
        print("SKIP  getAlbumTexts: no parent folder on the video")
        return
    sr = _json("getAlbumTexts.view", {"id": v["parent"]})
    names = [f["name"] for f in
             sr.get("albumTexts", {}).get("textFile", [])]
    bad = [n for n in names if n.endswith(".chapters.txt")]
    assert not bad, f"chapter files listed as liner notes: {bad}"
    print(f"PASS  getAlbumTexts hides chapter sidecars ({len(names)} text file(s))")


# ---- getAlbumChapters and search (the index) --------------------------

def test_album_chapters_agrees_with_get_chapters():
    """The index and the file must describe the same markers.

    They are read from different places on purpose -- getChapters from the
    sidecar, this from the scan's table -- so a disagreement is exactly the
    failure that split can produce.
    """
    if not _original()["chapters"]["writable"]:
        print("SKIP  getAlbumChapters: not writable")
        return
    _save(_vid(), "0:00 First\n13:35 Second\n")
    v = _video()
    sr = _json("getAlbumChapters.view", {"id": v["parent"]})
    vids = sr.get("albumChapters", {}).get("video", [])
    mine = [x for x in vids if x["id"] == _vid()]
    assert mine, f"video {_vid()} absent from {[x['id'] for x in vids]}"
    got = [(c["start"], c.get("name", "")) for c in mine[0].get("chapter", [])]
    assert got == _pairs(_get(_vid())), f"{got} != {_pairs(_get(_vid()))}"
    print(f"PASS  getAlbumChapters agrees with getChapters ({len(got)} marker(s))")


def test_album_chapters_updates_without_a_rescan():
    """A save writes the index too, or the album view is a scan behind."""
    if not _original()["chapters"]["writable"]:
        print("SKIP  index freshness: not writable")
        return
    _save(_vid(), "0:00 Before\n")
    _save(_vid(), "0:00 After\n30:00 Later\n")
    sr = _json("getAlbumChapters.view", {"id": _video()["parent"]})
    mine = [x for x in sr.get("albumChapters", {}).get("video", [])
            if x["id"] == _vid()]
    got = [c.get("name", "") for c in mine[0].get("chapter", [])] if mine else []
    assert got == ["After", "Later"], got
    print("PASS  a save is visible to getAlbumChapters with no rescan")


def test_album_chapters_omits_videos_without_markers():
    sr = _json("getAlbumChapters.view", {"id": _video()["parent"]})
    for v in sr.get("albumChapters", {}).get("video", []):
        assert v.get("chapter"), f"{v['id']} listed with no chapters"
    print("PASS  getAlbumChapters lists only videos that have markers")


def test_album_chapters_rejects_missing_id():
    sr = _json("getAlbumChapters.view")
    assert sr["status"] == "failed" and sr["error"]["code"] == 10, sr
    print("PASS  getAlbumChapters without id is error 10")


def test_search_finds_a_chapter():
    if not _original()["chapters"]["writable"]:
        print("SKIP  search: not writable")
        return
    needle = "Zzqx Marker Test"
    _save(_vid(), f"7:00 {needle}\n")
    sr = _json("search3.view", {"query": needle, "chapterCount": "20",
                                "artistCount": "0", "albumCount": "0"})
    hits = sr.get("searchResult3", {}).get("chapter", [])
    assert any(h.get("name") == needle for h in hits), hits
    h = next(h for h in hits if h.get("name") == needle)
    assert h["songId"] == _vid(), h
    assert float(h["start"]) == 420.0, h
    assert h.get("parent"), h
    print(f"PASS  search finds a chapter: {h['name']!r} at {h['start']}s "
          f"in {h.get('video')!r}")


def test_search_does_not_report_chapters_as_songs():
    """A chapter has no id anything can stream, star or queue."""
    if not _original()["chapters"]["writable"]:
        print("SKIP  search shape: not writable")
        return
    needle = "Zzqx Marker Test"
    _save(_vid(), f"7:00 {needle}\n")
    sr = _json("search3.view", {"query": needle, "chapterCount": "20"})
    songs = sr.get("searchResult3", {}).get("song", [])
    assert not [s for s in songs if s.get("title") == needle], songs
    print("PASS  a chapter match never appears in song[]")


def test_search_omits_chapters_when_not_asked():
    """An older client must see the response it has always seen."""
    if not _original()["chapters"]["writable"]:
        print("SKIP  search default: not writable")
        return
    _save(_vid(), "7:00 Zzqx Marker Test\n")
    sr = _json("search3.view", {"query": "Zzqx Marker Test"})
    assert "chapter" not in sr.get("searchResult3", {}), sr["searchResult3"]
    print("PASS  chapterCount defaults to 0 and the array is absent")


TESTS = [
    test_get_chapters_shape,
    test_missing_id,
    test_get_chapters_accepts_audio,
    test_get_chapters_rejects_unknown_id,
    test_save_and_read_back,
    test_round_trip_is_a_fixed_point,
    test_duplicate_names_both_survive,
    test_awkward_titles,
    test_unsorted_input_comes_back_sorted,
    test_unparseable_line_loses_only_itself,
    test_duration_is_derived,
    test_too_many_is_refused,
    test_empty_body_is_a_tombstone,
    test_xml_and_json_agree,
    test_album_texts_does_not_list_the_sidecar,
    test_album_chapters_agrees_with_get_chapters,
    test_album_chapters_updates_without_a_rescan,
    test_album_chapters_omits_videos_without_markers,
    test_album_chapters_rejects_missing_id,
    test_search_finds_a_chapter,
    test_search_does_not_report_chapters_as_songs,
    test_search_omits_chapters_when_not_asked,
]


if __name__ == "__main__":
    failed = 0
    for t in TESTS:
        try:
            t()
        except Exception as e:
            print(f"FAIL  {t.__name__}: {e}")
            failed += 1

    try:
        _restore()
        v = _video()
        if _original()["chapters"]["source"] == "none":
            print(f"\nNOTE  an empty <stem>.chapters.txt is left beside "
                  f"{v['title']!r} - remove it by hand if you would rather "
                  f"it were not there.")
        else:
            print(f"\nRestored the original markers on {v['title']!r}.")
    except Exception as e:
        print(f"\nWARNING  could not restore the original chapters: {e}")

    print(f"\n{len(TESTS) - failed}/{len(TESTS)} passed")
    sys.exit(failed)

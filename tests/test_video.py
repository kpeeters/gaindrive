#!/usr/bin/env python3
"""Video endpoint tests.

Covers the tier ladder (direct / remux / re-encode), the Child fields that
mark an entry as video, and the stateless HLS playlist — every segment URL it
emits must resolve, because nothing materialises them in advance. The playlist
answers at three paths and, given a repeated bitRate, as a master playlist.

Start the server first, against a collection containing at least one video:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_video.py
"""

import json
import shutil
import subprocess
import sys
import time
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

# Containers the server serves untouched; anything else is remuxed or encoded.
DIRECT_SUFFIXES = {"mp4", "m4v", "webm"}

# Containers a client may declare it demuxes itself, which moves them from the
# remux tier to the direct one for that one request.  Not vob: a DVD titleset's
# stored path names only the first of its concatenated VOBs.
DECLARABLE_SUFFIXES = {"mkv", "mov", "avi"}

# What the server labels each of those when it hands it over untouched, and how
# to recognise the bytes.  A remux answers video/mp4 whatever it started as, so
# the type is most of the test; the magic number is what catches a direct serve
# that was somehow mislabelled.
CONTAINER_MIMES = {
    "mkv": "video/x-matroska",
    "mov": "video/quicktime",
    "avi": "video/x-msvideo",
}
CONTAINER_MAGIC = {
    "mkv": (0, b"\x1a\x45\xdf\xa3"),   # EBML
    "avi": (0, b"RIFF"),
}


def _url(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


def _raw(endpoint, extra=None, headers=None):
    """Returns (status, headers, body). Does not raise on 4xx/5xx."""
    req = urllib.request.Request(_url(endpoint, extra))
    for k, v in (headers or {}).items():
        req.add_header(k, v)
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
        # An XML body here almost always means the endpoint fell through to
        # the catch-all, which ignores f=json — i.e. the running server
        # predates the build. Look for "NOT IMPLEMENTED" in its log.
        raise AssertionError(
            f"{endpoint} returned HTTP {status} with a non-JSON body: "
            f"{body[:200]!r}") from None


def _xml(endpoint, extra=None):
    extra = dict(extra or {})
    extra["f"] = "xml"
    _, _, body = _raw(endpoint, extra)
    return ET.fromstring(body)


# Queried once on first use rather than at import: a server that is down, or
# running an older binary, should fail one test with a readable message
# instead of aborting the whole script with a traceback.
_CACHE = {}


def _videos():
    if "videos" not in _CACHE:
        r = _json("getVideos.view")
        _CACHE["videos"] = r.get("videos", {}).get("video", [])
    return _CACHE["videos"]


def _video():
    vs = _videos()
    return vs[0] if vs else None


def _need_video():
    assert _video(), \
        "no videos in the library — scan a collection with video first"


# ---- getVideos --------------------------------------------------------

def test_get_videos_marks_entries_as_video():
    _need_video()
    for v in _videos():
        assert v.get("isVideo") is True, f"{v.get('title')}: isVideo={v.get('isVideo')}"
        assert v.get("type") == "video", f"{v.get('title')}: type={v.get('type')}"
        assert isinstance(v.get("id"), str), "ids must be strings"
    print(f"PASS  getVideos returned {len(_videos())} entries, all marked as video")


def test_get_videos_reports_dimensions():
    _need_video()
    sized = [v for v in _videos()
             if v.get("originalWidth") and v.get("originalHeight")]
    assert sized, "no video reported originalWidth/originalHeight — ffprobe " \
                  "did not run during the scan, or ffprobe is missing"
    print(f"PASS  {len(sized)}/{len(_videos())} videos report their dimensions")


def test_get_videos_xml_agrees():
    _need_video()
    root = _xml("getVideos.view")
    entries = list(root.iter(f"{{{NS}}}video"))
    assert len(entries) == len(_videos()), \
        f"xml has {len(entries)} videos, json has {len(_videos())}"
    assert entries[0].get("isVideo") == "true", entries[0].get("isVideo")
    print("PASS  the XML and JSON representations agree")


def test_audio_is_not_marked_as_video():
    """The isVideo/type fields used to be hardcoded; make sure audio kept them."""
    r = _json("search3.view", {"query": "", "songCount": "20",
                               "artistCount": "0", "albumCount": "0"})
    songs = r.get("searchResult3", {}).get("song", [])
    audio = [s for s in songs if s.get("suffix") not in
             {"mkv", "mp4", "m4v", "avi", "mpg", "mpeg", "mov", "webm", "wmv"}]
    assert audio, "no audio tracks found to check"
    for s in audio:
        assert s.get("isVideo") is False, f"{s.get('title')} claims to be video"
        assert s.get("type") == "music", s.get("type")
    print(f"PASS  {len(audio)} audio tracks still report type=music")


# ---- streaming tiers --------------------------------------------------

def test_direct_tier_honours_ranges():
    """Tier 0: an already-playable container is served as the raw file."""
    direct = [v for v in _videos() if v.get("suffix") in DIRECT_SUFFIXES]
    if not direct:
        print("SKIP  no mp4/m4v/webm video in the library (tier 0 untested)")
        return
    status, hdrs, body = _raw("stream.view", {"id": direct[0]["id"]},
                              {"Range": "bytes=0-1023"})
    assert status == 206, f"expected 206, got {status}"
    assert len(body) == 1024, len(body)
    assert "Content-Range" in hdrs, hdrs
    assert hdrs.get("Content-Type", "").startswith("video/"), \
        hdrs.get("Content-Type")
    print("PASS  tier 0 serves the raw file with byte ranges")


def test_transcoded_tier_returns_video():
    """A size constraint forces an encode, which must still be video/mp4."""
    _need_video()
    status, hdrs, body = _raw("stream.view",
                              {"id": _video()["id"], "size": "320x240"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", hdrs.get("Content-Type")
    assert len(body) > 0, "empty body — ffmpeg produced nothing"
    print("PASS  a constrained request re-encodes to fragmented mp4")


def test_segment_request_returns_mpegts():
    """A duration bound is what makes a request an HLS segment."""
    _need_video()
    status, hdrs, body = _raw("stream.view",
                              {"id": _video()["id"], "timeOffset": "0",
                               "duration": "10"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp2t", hdrs.get("Content-Type")
    # Every MPEG-TS packet starts with the sync byte 0x47.
    assert body[:1] == b"\x47", f"not a transport stream: {body[:4]!r}"
    print("PASS  a bounded request returns one MPEG-TS segment")


# ---- declared containers ----------------------------------------------
#
# `playable` lets a client say it demuxes a container itself, so the server can
# skip a remux it would otherwise pay.  The audio half of the same parameter is
# covered by tests/test_playable.py.  What is worth testing here is
# almost entirely the boundaries: that it never widens the codec test, never
# beats a constraint, never admits `vob`, and — the one with the worst blast
# radius — never changes what the browse endpoints advertise, because that is
# what a Cast receiver is told it is about to fetch.


def _remuxable():
    """A video the server would remux: right codecs, wrong container."""
    return [v for v in _videos()
            if v.get("nativeSeek") is True
            and v.get("suffix") not in DIRECT_SUFFIXES
            and v.get("suffix") in DECLARABLE_SUFFIXES]


def test_declared_container_is_served_untouched():
    """The point of the feature: a remux becomes a direct serve."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no remuxable mkv/mov/avi in the library")
        return
    v = vs[0]
    status, hdrs, body = _raw("stream.view",
                              {"id": v["id"],
                               "playable": v["suffix"]},
                              {"Range": "bytes=0-1023"})
    assert status == 206, f"expected 206, got {status}"
    assert "Content-Range" in hdrs, hdrs
    assert "X-Gaindrive-Transcode" not in hdrs, \
        "still went through the transcode cache: " + str(hdrs)
    ctype = hdrs.get("Content-Type", "")
    assert ctype == CONTAINER_MIMES[v["suffix"]], \
        f"expected {CONTAINER_MIMES[v['suffix']]}, got {ctype!r}"
    # The headers alone cannot tell a served container from a mislabelled
    # remux, so check the bytes where the container has a magic number worth
    # checking.  Not .mov: it is MP4-family, so its header and a remux's are
    # the same shape and only the Content-Type above separates them.
    magic = CONTAINER_MAGIC.get(v["suffix"])
    if magic:
        off, want = magic
        assert body[off:off + len(want)] == want, \
            f"not a .{v['suffix']}: {body[:12]!r}"
    print(f"PASS  a declared .{v['suffix']} is served untouched")


def test_declared_container_does_not_change_metadata():
    """The advertised fields describe what *any* client is sent, and must.

    A Cast receiver picks its decode pipeline from transcodedContentType, so
    the day this starts varying per client is the day casting an .mkv breaks.
    """
    vs = _remuxable()
    if not vs:
        print("SKIP  no remuxable mkv/mov/avi in the library")
        return
    v = vs[0]
    plain = _json("getSong.view", {"id": v["id"]})["song"]
    declared = _json("getSong.view",
                     {"id": v["id"],
                      "playable": v["suffix"]})["song"]
    for field in ("transcodedContentType", "transcodedSuffix", "nativeSeek"):
        assert plain.get(field) == declared.get(field), \
            f"{field} moved: {plain.get(field)!r} -> {declared.get(field)!r}"
    assert plain.get("transcodedSuffix") == "mp4", plain.get("transcodedSuffix")
    print("PASS  browse metadata is unchanged by a declaration")


def test_vob_is_never_declarable():
    """A DVD row's path names only the first VOB of a concatenated titleset."""
    vobs = [v for v in _videos() if v.get("suffix") == "vob"]
    if not vobs:
        print("SKIP  no DVD rip in the library")
        return
    status, hdrs, _ = _raw("stream.view",
                           {"id": vobs[0]["id"], "playable": "vob"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", \
        "a declared vob was served raw: " + str(hdrs.get("Content-Type"))
    print("PASS  vob is refused as a declaration")


def test_declared_container_still_honours_constraints():
    """A declaration skips a remux; it never overrides a real constraint."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no remuxable mkv/mov/avi in the library")
        return
    v = vs[0]
    status, hdrs, body = _raw("stream.view",
                              {"id": v["id"], "size": "320x240",
                               "playable": v["suffix"]})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", hdrs.get("Content-Type")
    assert len(body) > 0, "empty body — ffmpeg produced nothing"
    print("PASS  a declaration does not beat size=")


def test_garbage_declaration_is_ignored():
    """Unrecognised tokens are dropped, not refused, and never 500."""
    _need_video()
    vid = _video()["id"]
    for value in ("../../etc/passwd", "a" * 500, ",,,", "nonsense",
                  "mkv,,vob,,nonsense"):
        status, _, _ = _raw("stream.view", {"id": vid,
                                            "playable": value},
                            {"Range": "bytes=0-1023"})
        assert status in (200, 206), f"{value!r} gave HTTP {status}"
    vs = _remuxable()
    if vs and vs[0]["suffix"] == "mkv":
        # Case is folded: songs.codec is stored lowercased.
        status, hdrs, _ = _raw("stream.view",
                               {"id": vs[0]["id"],
                                "playable": "MKV"},
                               {"Range": "bytes=0-1023"})
        assert "X-Gaindrive-Transcode" not in hdrs, \
            "MKV was not folded to mkv: " + str(hdrs)
    print("PASS  a malformed declaration is ignored rather than fatal")


# ---- streaming while the remux builds ---------------------------------
#
# `startImmediately` asks the server not to wait out a whole-file `-c copy`
# before sending anything: it answers from a fragmented pipe and builds the
# seekable cache entry beside it.  What is worth testing is the boundaries —
# that it touches no other tier, overrides no constraint, changes nothing the
# API advertises, and is refused for a cast token, which is the one whose
# failure would only ever show up on somebody's television.


def test_start_immediately_streams_and_then_caches():
    """The feature, end to end: piped now, seekable afterwards."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no remuxable mkv/mov/avi in the library")
        return
    v = vs[0]
    status, hdrs, body = _raw("stream.view",
                              {"id": v["id"], "startImmediately": "true"})
    assert status == 200, status
    assert len(body) > 0, "empty body"
    # Two well-formed answers, and which one arrives depends on whether the
    # cache happened to be warm — so accept either rather than demanding a
    # cold cache the test cannot arrange.
    state = hdrs.get("X-Gaindrive-Transcode")
    assert state in ("building", "hit", "miss"), state
    if state == "building":
        assert "Content-Length" not in hdrs, hdrs
        assert hdrs.get("Accept-Ranges") == "none", hdrs.get("Accept-Ranges")
        # A fragmented MP4: ftyp first, and a moof rather than a moov, which
        # is what distinguishes it from the cache entry's layout.
        assert body[4:8] == b"ftyp", f"not MP4: {body[:12]!r}"
        assert b"moof" in body[:65536], "no moof — not fragmented"
    else:
        assert "Content-Length" in hdrs, hdrs

    # Whatever happened above, the entry must exist shortly afterwards: the
    # background build is the half that makes later plays cheap.
    for _ in range(120):
        _, h2, _ = _raw("stream.view", {"id": v["id"]},
                        {"Range": "bytes=0-1023"})
        if h2.get("X-Gaindrive-Transcode") == "hit":
            print("PASS  streamed at once, and the cache entry landed")
            return
        time.sleep(1)
    raise AssertionError("the background build never produced an entry")


def test_start_immediately_does_not_change_metadata():
    """The advertised fields describe what *any* client is sent, and must.

    A Cast receiver picks its decode pipeline from transcodedContentType, so
    the day these start varying per request is the day casting breaks.
    """
    _need_video()
    vid = _video()["id"]
    plain = _json("getSong.view", {"id": vid})["song"]
    asked = _json("getSong.view",
                  {"id": vid, "startImmediately": "true"})["song"]
    for field in ("transcodedContentType", "transcodedSuffix", "nativeSeek"):
        assert plain.get(field) == asked.get(field), \
            f"{field} moved: {plain.get(field)!r} -> {asked.get(field)!r}"
    print("PASS  browse metadata is unchanged by startImmediately")


def test_start_immediately_leaves_the_direct_tier_alone():
    """Nothing to skip when the file is already served off disk."""
    direct = [v for v in _videos() if v.get("suffix") in DIRECT_SUFFIXES]
    if not direct:
        print("SKIP  no mp4/m4v/webm video in the library")
        return
    status, hdrs, _ = _raw("stream.view",
                           {"id": direct[0]["id"],
                            "startImmediately": "true"},
                           {"Range": "bytes=0-1023"})
    assert status == 206, status
    assert "Content-Range" in hdrs, hdrs
    assert "X-Gaindrive-Transcode" not in hdrs, hdrs
    print("PASS  startImmediately does not touch the direct tier")


def test_start_immediately_does_not_beat_a_constraint():
    """It skips a remux.  It does not make a re-encode seekable."""
    _need_video()
    status, hdrs, body = _raw("stream.view",
                              {"id": _video()["id"], "size": "320x240",
                               "startImmediately": "true"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", hdrs.get("Content-Type")
    assert len(body) > 0, "empty body — ffmpeg produced nothing"
    print("PASS  startImmediately does not override size=")


def test_start_immediately_only_accepts_true():
    """Pinned to the literal, as pace and estimateContentLength are."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no remuxable mkv/mov/avi in the library")
        return
    v = vs[0]
    for value in ("yes", "1", "TRUE", "", "../../etc/passwd"):
        status, hdrs, _ = _raw("stream.view",
                               {"id": v["id"], "startImmediately": value},
                               {"Range": "bytes=0-1023"})
        assert status in (200, 206), f"{value!r} gave HTTP {status}"
        assert hdrs.get("X-Gaindrive-Transcode") != "building", \
            f"{value!r} was read as true"
    print("PASS  only the literal 'true' asks for the fast path")


# ---- audio only -------------------------------------------------------
#
# Naming an audio format for a video asks for its soundtrack alone.  This is
# not an extension: `format` is an ordinary Subsonic parameter, and VIDEO.md
# specified the behaviour from the start — the `-vn` the audio path already
# passes *is* the extraction.  What makes it worth having is everything that
# follows from being ordinary audio: the transcode cache materialises it, so
# it carries a real Content-Length, answers Range requests, and is a fraction
# of the bytes.


def test_audio_format_returns_the_soundtrack():
    _need_video()
    status, hdrs, body = _raw("stream.view",
                              {"id": _video()["id"], "format": "opus",
                               "maxBitRate": "128"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "audio/ogg", hdrs.get("Content-Type")
    # The cache path is the whole point: a piped transcode has no length.
    assert hdrs.get("Content-Length"), \
        "no Content-Length — the transcode cache did not produce a file"
    assert body[:4] == b"OggS", f"not an Ogg stream: {body[:8]!r}"
    print("PASS  an audio format on a video returns its soundtrack, with a length")


def test_audio_only_stream_is_seekable():
    """The property the video path cannot offer, and the reason to cache."""
    _need_video()
    status, hdrs, body = _raw("stream.view",
                              {"id": _video()["id"], "format": "opus",
                               "maxBitRate": "128"},
                              {"Range": "bytes=1024-2047"})
    assert status == 206, f"expected 206, got {status}"
    assert len(body) == 1024, len(body)
    assert "Content-Range" in hdrs, hdrs
    print("PASS  the extracted soundtrack answers byte ranges")


def test_audio_only_covers_the_whole_video():
    """A DVD rip is several VOBs that are one stream; extracting only the
    first gives a film that reports and stops after twenty minutes."""
    _need_video()
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        print("SKIP  ffprobe not on PATH")
        return

    video = _video()
    expected = video.get("duration")
    if not expected:
        print("SKIP  the server reports no duration for this video")
        return

    _, _, body = _raw("stream.view", {"id": video["id"], "format": "opus",
                                      "maxBitRate": "128"})
    out = subprocess.run(
        [ffprobe, "-v", "error", "-show_entries", "format=duration",
         "-of", "csv=p=0", "-"],
        input=body, capture_output=True)
    got = float(out.stdout.decode().strip() or 0)
    # Generous: a keyframe-aligned container and the server's own rounding
    # disagree by a second or two on a long film.
    assert abs(got - expected) <= max(5, expected * 0.02), \
        f"soundtrack is {got:.0f}s, video is {expected}s"
    print(f"PASS  the soundtrack covers the whole video ({got:.0f}s)")


def test_raw_and_absent_format_still_serve_video():
    """The switch is `format`, so neither of the other two spellings may
    accidentally become an audio request."""
    _need_video()
    for extra in ({"id": _video()["id"]},
                  {"id": _video()["id"], "format": "raw"}):
        status, hdrs, _ = _raw("stream.view", extra, {"Range": "bytes=0-1023"})
        assert status in (200, 206), f"{extra}: HTTP {status}"
        ctype = hdrs.get("Content-Type", "")
        assert ctype.startswith("video/"), f"{extra}: Content-Type {ctype}"
    print("PASS  no format, and format=raw, both still serve the video")


# ---- HLS --------------------------------------------------------------

def test_hls_playlist_is_well_formed():
    _need_video()
    status, hdrs, body = _raw("hls.m3u8", {"id": _video()["id"]})
    assert status == 200, status
    text = body.decode()
    assert text.startswith("#EXTM3U"), text[:40]
    assert "#EXT-X-ENDLIST" in text, "playlist is not terminated"
    segs = [l for l in text.splitlines() if l.startswith("stream.view")]
    expected = (int(_video()["duration"]) + 9) // 10
    assert len(segs) == expected, f"{len(segs)} segments, expected {expected}"
    print(f"PASS  hls.m3u8 lists {len(segs)} segments for a "
          f"{_video()['duration']}s video")


def test_hls_segments_resolve():
    """Nothing pre-materialises these, so each one has to transcode on demand."""
    _need_video()
    _, _, body = _raw("hls.m3u8", {"id": _video()["id"]})
    segs = [l for l in body.decode().splitlines()
            if l.startswith("stream.view")]
    assert segs, "playlist had no segments"
    # First, middle and last: enough to catch an offset that runs past the end.
    for i in {0, len(segs) // 2, len(segs) - 1}:
        req = urllib.request.Request(f"{BASE}/{segs[i]}")
        with urllib.request.urlopen(req) as r:
            data = r.read()
        assert r.status == 200, f"segment {i}: {r.status}"
        assert data[:1] == b"\x47", f"segment {i} is not MPEG-TS"
    print("PASS  first, middle and last HLS segments all transcode")


def test_hls_segments_carry_absolute_timestamps():
    """Each segment must be stamped where the playlist says it belongs.

    -ss before -i rebases the output, so without -output_ts_offset every
    segment starts at PTS 0.  A player seeds its timestamp adjuster from the
    first segment it loads and reuses it, so the second maps to the same
    instant as the first and the timeline stops advancing — playback stalls
    with no error at all, because bytes keep arriving and nothing has failed.
    """
    _need_video()
    ffprobe = shutil.which("ffprobe")
    if not ffprobe:
        print("SKIP  ffprobe not on PATH")
        return

    _, _, body = _raw("hls.m3u8", {"id": _video()["id"]})
    segs = [l for l in body.decode().splitlines()
            if l.startswith("stream.view")]
    if len(segs) < 3:
        print("SKIP  video too short to have three segments")
        return

    def first_pts(seg):
        with urllib.request.urlopen(f"{BASE}/{seg}") as r:
            data = r.read()
        out = subprocess.run(
            [ffprobe, "-v", "error", "-select_streams", "v:0",
             "-show_entries", "packet=pts_time", "-of", "csv=p=0", "-"],
            input=data, capture_output=True)
        for line in out.stdout.decode().splitlines():
            value = line.strip().rstrip(",")
            if value:
                return float(value)
        raise AssertionError(f"no video packets in segment: {seg}")

    # The first and the third, so the gap is two whole segments and a keyframe
    # snap of a second or two cannot be mistaken for the real thing.
    start, later = first_pts(segs[0]), first_pts(segs[2])
    gap = later - start
    assert gap > 10.0, (
        f"segments start {gap:.2f}s apart; two segments should be ~20s. "
        "Timestamps are being rebased to zero — see -output_ts_offset in "
        "video_ffmpeg_argv.")
    print(f"PASS  HLS segments advance ({gap:.1f}s across two segments)")


def test_hls_is_served_at_every_spelling():
    """The spec spells it .m3u8; a client composing <name>.view needs both.

    One handler is registered at hls.m3u8 and hls.view, and bare `hls` reaches
    the latter through the pre-routing rewrite. All three must produce the same
    playlist, which they can only do because every URI in the body is relative.
    """
    _need_video()
    vid = _video()["id"]
    bodies = {}
    for path in ("hls.m3u8", "hls.view", "hls"):
        status, hdrs, body = _raw(path, {"id": vid})
        assert status == 200, f"{path}: HTTP {status}"
        assert body.startswith(b"#EXTM3U"), f"{path}: {body[:40]!r}"
        assert hdrs.get("Content-Type", "") == "application/vnd.apple.mpegurl", \
            f"{path}: {hdrs.get('Content-Type')!r}"
        assert "no-store" in hdrs.get("Cache-Control", ""), \
            f"{path}: {hdrs.get('Cache-Control')!r}"
        bodies[path] = body
    assert bodies["hls.m3u8"] == bodies["hls.view"] == bodies["hls"], \
        "the three paths disagree about the playlist"
    print("PASS  hls.m3u8, hls.view and hls all serve the same playlist")


def test_hls_variant_playlist():
    """A repeated bitRate is the spec's request for a master playlist."""
    _need_video()
    vid = _video()["id"]
    for path in ("hls.m3u8", "hls.view"):
        url = _url(path, {"id": vid}) + "&bitRate=2000@1280x720&bitRate=800"
        with urllib.request.urlopen(url) as r:
            text = r.read().decode()
            cc = r.headers.get("Cache-Control", "")
        assert text.startswith("#EXTM3U"), text[:40]
        assert "no-store" in cc, f"{path}: master is cacheable ({cc!r})"
        # A master holds no segments, so it must not be terminated like one.
        assert "#EXT-X-ENDLIST" not in text, "master carries EXT-X-ENDLIST"
        assert "#EXTINF" not in text, "master carries segments"

        infs = [l for l in text.splitlines() if l.startswith("#EXT-X-STREAM-INF")]
        uris = [l for l in text.splitlines() if not l.startswith("#")]
        assert len(infs) == 2, f"{len(infs)} variants, expected 2"
        assert len(uris) == 2, f"{len(uris)} variant URIs, expected 2"

        bws = [int(l.split("BANDWIDTH=")[1].split(",")[0]) for l in infs]
        assert bws == [800_000, 2_000_000], f"bandwidths {bws}, expected ascending"
        # RESOLUTION belongs only to the variant that supplied @WxH.
        assert "RESOLUTION" not in infs[0], infs[0]
        assert "RESOLUTION=1280x720" in infs[1], infs[1]

        # Each URI names this endpoint again, on the spelling that was fetched,
        # carrying exactly one bitRate.
        for u, want in zip(uris, ("bitRate=800", "bitRate=2000@1280x720")):
            assert u.startswith(f"{path}?id="), u
            assert u.count("bitRate=") == 1, u
            assert f"&{want}&" in u or u.endswith(f"&{want}"), u

        # And a variant URI resolves to an ordinary media playlist.
        with urllib.request.urlopen(f"{BASE}/{uris[0]}") as r:
            media = r.read().decode()
        assert "#EXT-X-ENDLIST" in media, "variant is not a media playlist"
        segs = [l for l in media.splitlines() if l.startswith("stream.view")]
        assert segs, "variant playlist had no segments"
        assert all("maxBitRate=800" in s for s in segs), segs[0]
    print("PASS  a repeated bitRate yields a master playlist on both paths")


def test_hls_rejects_audio():
    r = _json("search3.view", {"query": "", "songCount": "5",
                               "artistCount": "0", "albumCount": "0"})
    songs = r.get("searchResult3", {}).get("song", [])
    audio = [s for s in songs if not s.get("isVideo")]
    if not audio:
        print("SKIP  no audio track to check")
        return
    _, _, body = _raw("hls.m3u8", {"id": audio[0]["id"]})
    assert b"error" in body.lower(), body[:120]
    print("PASS  hls.m3u8 refuses a non-video id")


# ---- getVideoInfo / getCaptions ---------------------------------------

def test_video_info_lists_tracks():
    _need_video()
    r = _json("getVideoInfo.view", {"id": _video()["id"]})
    assert "videoInfo" in r, r
    info = r["videoInfo"]
    assert info["id"] == _video()["id"], info["id"]
    assert isinstance(info.get("audioTrack"), list), info
    assert isinstance(info.get("captions"), list), info
    print(f"PASS  getVideoInfo: {len(info['audioTrack'])} audio track(s), "
          f"{len(info['captions'])} caption track(s)")


def test_video_info_rejects_audio():
    r = _json("search3.view", {"query": "", "songCount": "5",
                               "artistCount": "0", "albumCount": "0"})
    songs = r.get("searchResult3", {}).get("song", [])
    audio = [s for s in songs if not s.get("isVideo")]
    if not audio:
        print("SKIP  no audio track to check")
        return
    r = _json("getVideoInfo.view", {"id": audio[0]["id"]})
    assert r.get("status") == "failed", r
    print("PASS  getVideoInfo refuses a non-video id")


def test_captions_are_webvtt_or_absent():
    _need_video()
    r = _json("getVideoInfo.view", {"id": _video()["id"]})
    caps = r.get("videoInfo", {}).get("captions", [])
    extra = {"id": _video()["id"]}
    if caps:
        extra["captionId"] = caps[0]["id"]
    status, hdrs, body = _raw("getCaptions.view", extra)
    if status == 404:
        print("SKIP  this video has no captions (sidecar or embedded)")
        return
    assert status == 200, status
    assert hdrs.get("Content-Type", "").startswith("text/vtt"), \
        hdrs.get("Content-Type")
    assert body.startswith(b"WEBVTT"), body[:40]
    print("PASS  getCaptions returns WebVTT")


def test_missing_id_is_a_subsonic_error():
    """A bare 500 from an unguarded stoi would fail this."""
    for ep in ("getVideoInfo.view", "getCaptions.view", "hls.m3u8", "hls.view"):
        status, _, body = _raw(ep)
        assert status == 200, f"{ep}: HTTP {status}"
        assert b"error" in body.lower(), f"{ep}: {body[:120]}"
    for ep in ("getVideoInfo.view", "hls.m3u8", "hls.view"):
        status, _, body = _raw(ep, {"id": "not-a-number"})
        assert status == 200, f"{ep}: HTTP {status}"
        assert b"error" in body.lower(), f"{ep}: {body[:120]}"
    print("PASS  missing and malformed ids produce Subsonic errors, not 500s")


# ---- The fields must not depend on which endpoint was asked ----------

def _find_entry(entries, vid):
    """A song entry by id, out of whatever list shape an endpoint returned."""
    if isinstance(entries, dict):
        entries = [entries]
    for e in entries or []:
        if e.get("id") == vid:
            return e
    return None


# The fields that come from the codec pair and its two neighbours.  Compared
# as a set rather than one at a time so a failure names every disagreement.
_TIER_FIELDS = ("nativeSeek", "season", "coverArt",
                "transcodedContentType", "transcodedSuffix")


def test_video_fields_agree_across_endpoints():
    """The same file must answer the same way whichever endpoint was asked.

    Only four queries used to select video_codec/audio_codec/season, so a video
    reached through a playlist or a search hit came back with empty codecs —
    reporting nativeSeek: false however seekable it was, advertising a
    transcode that would not happen, and (for a loose file) its section's cover
    instead of its own. The Android app hides its cast button on that flag, so
    the same film was castable from its album and not from a playlist.

    getVideos is the reference: it is one of the four that always selected them.
    """
    _need_video()
    v   = _video()
    vid = v["id"]
    ref = {k: v.get(k) for k in _TIER_FIELDS}

    seen = {}

    # search3 — no mutation needed.
    r = _json("search3.view", {"query": v.get("title", ""),
                               "songCount": 500, "artistCount": 0,
                               "albumCount": 0})
    hit = _find_entry(r.get("searchResult3", {}).get("song"), vid)
    if hit is not None:
        seen["search3"] = hit

    # getStarred2 — star it, then leave the star exactly as it was found.
    was_starred = bool(v.get("starred"))
    if not was_starred:
        _json("star.view", {"id": vid})
    try:
        r = _json("getStarred2.view")
        hit = _find_entry(r.get("starred2", {}).get("song"), vid)
        assert hit is not None, "the video did not come back from getStarred2"
        seen["getStarred2"] = hit
    finally:
        if not was_starred:
            _json("unstar.view", {"id": vid})

    # createPlaylist reads its songs back through a near-duplicate of
    # get_playlist's query, so both are worth checking: it is the copy a fix
    # applied by hand is most likely to miss.
    r = _json("createPlaylist.view",
              {"name": "gd-test-native-seek", "songId": vid})
    pid = str(r.get("playlist", {}).get("id", ""))
    assert pid, f"createPlaylist returned no id: {r}"
    try:
        hit = _find_entry(r.get("playlist", {}).get("entry"), vid)
        if hit is not None:
            seen["createPlaylist"] = hit
        r = _json("getPlaylist.view", {"id": pid})
        hit = _find_entry(r.get("playlist", {}).get("entry"), vid)
        assert hit is not None, "the video did not come back from getPlaylist"
        seen["getPlaylist"] = hit
    finally:
        _json("deletePlaylist.view", {"id": pid})

    assert seen, "the video could not be reached through any other endpoint"

    bad = []
    for name, e in sorted(seen.items()):
        got = {k: e.get(k) for k in _TIER_FIELDS}
        if got != ref:
            bad.append(f"  {name}: {got}")
    assert not bad, (
        "endpoints disagree about %r\n  getVideos: %s\n%s"
        % (v.get("title"), ref, "\n".join(bad)))

    print(f"PASS  nativeSeek/season/coverArt/transcoded* agree across "
          f"{', '.join(sorted(seen))}")


def test_direct_video_advertises_no_transcode_from_a_playlist():
    """transcode_target() reads the same codec pair, so it had the same gap.

    A directly-playable file advertises no transcodedContentType at all. With
    the codecs empty it advertised video/mp4 — and the Android app hands that
    value to a Cast receiver as the LOAD's contentType, so it is not cosmetic.
    """
    _need_video()
    direct = [v for v in _videos()
              if v.get("suffix") in DIRECT_SUFFIXES
              and v.get("nativeSeek") is True
              and not v.get("transcodedContentType")]
    if not direct:
        print("SKIP  no directly-playable video in the library")
        return
    v   = direct[0]
    vid = v["id"]

    r = _json("createPlaylist.view",
              {"name": "gd-test-transcode-target", "songId": vid})
    pid = str(r.get("playlist", {}).get("id", ""))
    assert pid, f"createPlaylist returned no id: {r}"
    try:
        r = _json("getPlaylist.view", {"id": pid})
        e = _find_entry(r.get("playlist", {}).get("entry"), vid)
        assert e is not None, "the video did not come back from getPlaylist"
        assert not e.get("transcodedContentType"), \
            (f"{v.get('title')!r} is served untouched but getPlaylist "
             f"advertises transcodedContentType="
             f"{e.get('transcodedContentType')!r}")
        assert not e.get("transcodedSuffix"), \
            f"{v.get('title')!r}: transcodedSuffix={e.get('transcodedSuffix')!r}"
    finally:
        _json("deletePlaylist.view", {"id": pid})
    print(f"PASS  a directly-played video advertises no transcode from a playlist")


TESTS = [
    test_get_videos_marks_entries_as_video,
    test_get_videos_reports_dimensions,
    test_get_videos_xml_agrees,
    test_audio_is_not_marked_as_video,
    test_direct_tier_honours_ranges,
    test_transcoded_tier_returns_video,
    test_segment_request_returns_mpegts,
    test_declared_container_is_served_untouched,
    test_declared_container_does_not_change_metadata,
    test_vob_is_never_declarable,
    test_declared_container_still_honours_constraints,
    test_garbage_declaration_is_ignored,
    test_start_immediately_streams_and_then_caches,
    test_start_immediately_does_not_change_metadata,
    test_start_immediately_leaves_the_direct_tier_alone,
    test_start_immediately_does_not_beat_a_constraint,
    test_start_immediately_only_accepts_true,
    test_audio_format_returns_the_soundtrack,
    test_audio_only_stream_is_seekable,
    test_audio_only_covers_the_whole_video,
    test_raw_and_absent_format_still_serve_video,
    test_hls_playlist_is_well_formed,
    test_hls_segments_resolve,
    test_hls_segments_carry_absolute_timestamps,
    test_hls_rejects_audio,
    test_hls_is_served_at_every_spelling,
    test_hls_variant_playlist,
    test_video_info_lists_tracks,
    test_video_info_rejects_audio,
    test_captions_are_webvtt_or_absent,
    test_missing_id_is_a_subsonic_error,
    test_video_fields_agree_across_endpoints,
    test_direct_video_advertises_no_transcode_from_a_playlist,
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

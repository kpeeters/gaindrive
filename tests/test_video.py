#!/usr/bin/env python3
"""Video endpoint tests.

Covers stream.view's tiers (direct / copy / re-encode), the Child fields that
mark an entry as video, and HLS: a playlist of fragmented-MP4 segments, every
one of which must resolve, carry the film's own timestamps, and join the next
without a gap. The playlist answers at three paths and, given a repeated
bitRate, as a master playlist.

Start the server first, against a collection containing at least one video:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_video.py
"""

import json
import re
import shutil
import subprocess
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

# Containers the server serves untouched; anything else goes through HLS.
DIRECT_SUFFIXES = {"mp4", "m4v", "webm"}

# Containers a client may declare it demuxes itself, which moves them from the
# copy tier to the direct one for that one request.  Not vob: a DVD titleset's
# stored path names only the first of its concatenated VOBs.
DECLARABLE_SUFFIXES = {"mkv", "mov", "avi"}

# What the server labels each of those when it hands it over untouched, and how
# to recognise the bytes.  A copy answers video/mp4 whatever it started as, so
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
        # the catch-all, which ignores f=json - i.e. the running server
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
        "no videos in the library - scan a collection with video first"


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
    assert sized, "no video reported originalWidth/originalHeight - ffprobe " \
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
    assert len(body) > 0, "empty body - ffmpeg produced nothing"
    print("PASS  a constrained request re-encodes to fragmented mp4")


def test_offset_request_on_copyable_video_is_fragmented_mp4():
    """A third-party client's seek on a file it cannot take as it is.

    The codecs allow a copy, so a timeOffset must not demote it to a re-encode:
    the answer is -c copy from the keyframe before the offset, as fragmented
    MP4 with no Content-Length.
    """
    vs = _remuxable()
    if not vs:
        print("SKIP  no copyable mkv/mov/avi in the library")
        return
    status, hdrs, body = _raw("stream.view",
                              {"id": vs[0]["id"], "timeOffset": "30"})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", hdrs.get("Content-Type")
    assert body[4:8] == b"ftyp", f"not MP4: {body[:12]!r}"
    assert b"moof" in body[:1 << 20], "no moof - not fragmented"
    print("PASS  a seek on a copyable video is a fragmented -c copy")


# ---- declared containers ----------------------------------------------
#
# `playable` lets a client say it demuxes a container itself, so the server can
# skip a remux it would otherwise pay.  The audio half of the same parameter is
# covered by tests/test_playable.py.  What is worth testing here is
# almost entirely the boundaries: that it never widens the codec test, never
# beats a constraint, never admits `vob`, and - the one with the worst blast
# radius - never changes what the browse endpoints advertise, because that is
# what a Cast receiver is told it is about to fetch.


def _remuxable():
    """A video the server would copy: right codecs, wrong container."""
    return [v for v in _videos()
            if v.get("nativeSeek") is True
            and v.get("suffix") not in DIRECT_SUFFIXES
            and v.get("suffix") in DECLARABLE_SUFFIXES]


def test_declared_container_is_served_untouched():
    """The point of the feature: a copy becomes a direct serve."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no copyable mkv/mov/avi in the library")
        return
    v = vs[0]
    status, hdrs, body = _raw("stream.view",
                              {"id": v["id"],
                               "playable": v["suffix"]},
                              {"Range": "bytes=0-1023"})
    assert status == 206, f"expected 206, got {status}"
    assert "Content-Range" in hdrs, hdrs
    ctype = hdrs.get("Content-Type", "")
    assert ctype == CONTAINER_MIMES[v["suffix"]], \
        f"expected {CONTAINER_MIMES[v['suffix']]}, got {ctype!r}"
    # The headers alone cannot tell a served container from a mislabelled
    # copy, so check the bytes where the container has a magic number worth
    # checking.  Not .mov: it is MP4-family, so its header and a copy's are
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
        print("SKIP  no copyable mkv/mov/avi in the library")
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
    """A declaration skips a copy; it never overrides a real constraint."""
    vs = _remuxable()
    if not vs:
        print("SKIP  no copyable mkv/mov/avi in the library")
        return
    v = vs[0]
    status, hdrs, body = _raw("stream.view",
                              {"id": v["id"], "size": "320x240",
                               "playable": v["suffix"]})
    assert status == 200, status
    assert hdrs.get("Content-Type") == "video/mp4", hdrs.get("Content-Type")
    assert len(body) > 0, "empty body - ffmpeg produced nothing"
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
        assert status == 206, "MKV was not folded to mkv: HTTP " + str(status)
    print("PASS  a malformed declaration is ignored rather than fatal")


# ---- audio only -------------------------------------------------------
#
# Naming an audio format for a video asks for its soundtrack alone.  This is
# not an extension: `format` is an ordinary Subsonic parameter, and the
# behaviour was specified from the start - the `-vn` the audio path already
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
        "no Content-Length - the transcode cache did not produce a file"
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

def _playlist(vid, extra=None):
    """(init URI, [(start, end, URI)]) from a media playlist."""
    params = {"id": vid}
    params.update(extra or {})
    _, _, body = _raw("hls.m3u8", params)
    text = body.decode()
    m = re.search(r'#EXT-X-MAP:URI="([^"]*)"', text)
    assert m, f"no EXT-X-MAP in the playlist: {text[:300]!r}"
    segs, t, dur = [], 0.0, None
    for line in text.splitlines():
        if line.startswith("#EXTINF:"):
            dur = float(line[len("#EXTINF:"):].split(",")[0])
        elif line and not line.startswith("#") and dur is not None:
            segs.append((t, t + dur, line))
            t  += dur
            dur = None
    return m.group(1), segs


def _get(uri):
    with urllib.request.urlopen(f"{BASE}/{uri}") as r:
        return r.status, r.headers.get("Content-Type", ""), r.read()


def _top_boxes(data):
    """Types of the top-level MP4 boxes."""
    out, off = [], 0
    while off + 8 <= len(data):
        size = int.from_bytes(data[off:off + 4], "big")
        kind = data[off + 4:off + 8].decode("latin1")
        if size == 1:
            size = int.from_bytes(data[off + 8:off + 16], "big")
        if size < 8:
            break
        out.append(kind)
        off += size
    return out


def _packets(data, selector):
    """[(pts, duration)] of one stream in an init+media concatenation."""
    out = subprocess.run(
        [shutil.which("ffprobe"), "-v", "error", "-select_streams", selector,
         "-show_entries", "packet=pts_time,duration_time", "-of", "csv=p=0",
         "-"],
        input=data, capture_output=True)
    rows = []
    for line in out.stdout.decode().splitlines():
        parts = line.strip().rstrip(",").split(",")
        try:
            rows.append((float(parts[0]), float(parts[1])))
        except (ValueError, IndexError):
            continue
    return sorted(rows)


def test_hls_playlist_is_well_formed():
    _need_video()
    v = _video()
    status, hdrs, body = _raw("hls.m3u8", {"id": v["id"]})
    assert status == 200, status
    text = body.decode()
    assert text.startswith("#EXTM3U"), text[:40]
    assert "#EXT-X-VERSION:7" in text, "fragmented MP4 needs version 7"
    assert "#EXT-X-ENDLIST" in text, "playlist is not terminated"
    _, segs = _playlist(v["id"])
    assert segs, "playlist has no segments"
    assert all(u.startswith("hlsSegment.view?") for _, _, u in segs), segs[0]
    total = segs[-1][1]
    assert abs(total - float(v["duration"])) < 1.5, \
        f"segments add up to {total:.1f}s, the video is {v['duration']}s"
    print(f"PASS  hls.m3u8 lists {len(segs)} segments covering "
          f"{total:.0f}s")


def test_hls_segments_resolve():
    """The init segment is ftyp+moov, and each media segment is moof+mdat."""
    _need_video()
    init, segs = _playlist(_video()["id"])
    status, ctype, data = _get(init)
    assert status == 200 and ctype == "video/mp4", (status, ctype)
    boxes = _top_boxes(data)
    assert boxes[:2] == ["ftyp", "moov"], boxes
    assert "moof" not in boxes, "init segment carries media"
    # First, middle and last: enough to catch a cut that runs past the end.
    for i in sorted({0, len(segs) // 2, len(segs) - 1}):
        status, ctype, data = _get(segs[i][2])
        assert status == 200 and ctype == "video/mp4", (i, status, ctype)
        boxes = _top_boxes(data)
        assert boxes and boxes[0] == "moof", f"segment {i}: {boxes[:4]}"
        assert "moov" not in boxes, f"segment {i} repeats the init segment"
    print("PASS  init, first, middle and last HLS segments are fragmented MP4")


def test_hls_segments_carry_absolute_timestamps():
    """Each segment is stamped where the playlist says it begins.

    Every ffmpeg run starts its own timeline at zero; the server moves each
    segment's tfdt to the film's time.  A player places segments by those
    timestamps, so one left at zero lands on top of the first.
    """
    _need_video()
    if not shutil.which("ffprobe"):
        print("SKIP  ffprobe not on PATH")
        return
    init, segs = _playlist(_video()["id"])
    _, _, head = _get(init)
    for i in sorted({0, len(segs) // 2, len(segs) - 1}):
        start = segs[i][0]
        _, _, data = _get(segs[i][2])
        video = _packets(head + data, "v:0")
        assert video, f"no video packets in segment {i}"
        assert abs(video[0][0] - start) < 0.25, (
            f"segment {i} starts at {video[0][0]:.3f}s, "
            f"the playlist says {start:.3f}s")
    print("PASS  HLS segments carry the film's own timestamps")


def _check_joins(vid, extra, label):
    init, segs = _playlist(vid, extra)
    if len(segs) < 3:
        print(f"SKIP  {label}: too short to have three segments")
        return
    _, _, data = _get(init)
    for _, _, uri in segs[:3]:
        data += _get(uri)[2]
    for sel, slack in (("v:0", 0.1), ("a:0", 0.03)):
        rows = _packets(data, sel)
        if not rows:
            continue
        gaps = [(b[0] - (a[0] + a[1]), a[0])
                for a, b in zip(rows, rows[1:])]
        worst = max(gaps)
        assert worst[0] < slack, (
            f"{label}: {sel} jumps {worst[0] * 1000:.0f} ms after "
            f"{worst[1]:.3f}s")
    print(f"PASS  {label}: three segments join without gaps")


def test_hls_segments_join_without_gaps():
    """No gap in picture or sound across a boundary - the old HLS's flaw.

    Encoding each segment on its own starts a fresh AAC encoder every time,
    whose priming frame is a short silence at every boundary.  The default
    plan and a re-encoded variant are both checked: the second is the one a
    continuous per-viewer encoder has to get right.
    """
    _need_video()
    if not shutil.which("ffprobe"):
        print("SKIP  ffprobe not on PATH")
        return
    vid = _video()["id"]
    _check_joins(vid, None, "default plan")
    _check_joins(vid, {"bitRate": "0@320x240"}, "re-encoded variant")


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
        segs = [l for l in media.splitlines() if l.startswith("hlsSegment.view")]
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
    reached through a playlist or a search hit came back with empty codecs -
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

    # search3 - no mutation needed.
    r = _json("search3.view", {"query": v.get("title", ""),
                               "songCount": 500, "artistCount": 0,
                               "albumCount": 0})
    hit = _find_entry(r.get("searchResult3", {}).get("song"), vid)
    if hit is not None:
        seen["search3"] = hit

    # getStarred2 - star it, then leave the star exactly as it was found.
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
    the codecs empty it advertised video/mp4 - and the Android app hands that
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
    test_offset_request_on_copyable_video_is_fragmented_mp4,
    test_declared_container_is_served_untouched,
    test_declared_container_does_not_change_metadata,
    test_vob_is_never_declarable,
    test_declared_container_still_honours_constraints,
    test_garbage_declaration_is_ignored,
    test_audio_format_returns_the_soundtrack,
    test_audio_only_stream_is_seekable,
    test_audio_only_covers_the_whole_video,
    test_raw_and_absent_format_still_serve_video,
    test_hls_playlist_is_well_formed,
    test_hls_segments_resolve,
    test_hls_segments_carry_absolute_timestamps,
    test_hls_segments_join_without_gaps,
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

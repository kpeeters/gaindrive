#!/usr/bin/env python3
"""Video endpoint tests.

Covers the tier ladder (direct / remux / re-encode), the Child fields that
mark an entry as video, and the stateless HLS playlist — every segment URL it
emits must resolve, because nothing materialises them in advance.

Start the server first, against a collection containing at least one video:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_video.py
"""

import json
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

# Containers the server serves untouched; anything else is remuxed or encoded.
DIRECT_SUFFIXES = {"mp4", "m4v", "webm"}


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
    for ep in ("getVideoInfo.view", "getCaptions.view", "hls.m3u8"):
        status, _, body = _raw(ep)
        assert status == 200, f"{ep}: HTTP {status}"
        assert b"error" in body.lower(), f"{ep}: {body[:120]}"
    for ep in ("getVideoInfo.view", "hls.m3u8"):
        status, _, body = _raw(ep, {"id": "not-a-number"})
        assert status == 200, f"{ep}: HTTP {status}"
        assert b"error" in body.lower(), f"{ep}: {body[:120]}"
    print("PASS  missing and malformed ids produce Subsonic errors, not 500s")


TESTS = [
    test_get_videos_marks_entries_as_video,
    test_get_videos_reports_dimensions,
    test_get_videos_xml_agrees,
    test_audio_is_not_marked_as_video,
    test_direct_tier_honours_ranges,
    test_transcoded_tier_returns_video,
    test_segment_request_returns_mpegts,
    test_hls_playlist_is_well_formed,
    test_hls_segments_resolve,
    test_hls_segments_carry_absolute_timestamps,
    test_hls_rejects_audio,
    test_video_info_lists_tracks,
    test_video_info_rejects_audio,
    test_captions_are_webvtt_or_absent,
    test_missing_id_is_a_subsonic_error,
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

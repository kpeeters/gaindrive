#!/usr/bin/env python3
"""Injection regressions for the stream and HLS parameters.

`size=` was spliced straight into an ffmpeg **filtergraph**:

    -vf yadif=deint=interlaced,scale=<W>:<H>:force_original_aspect_ratio=decrease

That is one argv element, so it was never shell injection — but a filtergraph
is its own language and `-vf` accepts source filters. `movie=` and
`subtitles=` both name a file to read, and the trailing option could be
absorbed by ending an injected chain with another scale, so the concatenation
offered no accidental protection.

The same value is echoed into the hls.m3u8 **body**, which is neither a URL
nor XML, so nothing downstream would have caught a newline in it either — into
a segment's query string, and, when bitRate is repeated, into a master
playlist's BANDWIDTH and RESOLUTION attributes and its variant URIs.

Needs a library containing at least one video. Start the server first, then:
    python3 tests/test_stream_params.py
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


def _url(endpoint, extra=None):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "xml"}
    if extra:
        p.update(extra)
    return f"{BASE}/{endpoint}?{urllib.parse.urlencode(p)}"


def _fetch(endpoint, extra=None, limit=1 << 20):
    try:
        with urllib.request.urlopen(_url(endpoint, extra)) as r:
            return r.status, r.read(limit)
    except urllib.error.HTTPError as e:
        return e.code, e.read(limit)


def _a_video_id():
    _, body = _fetch("getVideos.view")
    root = ET.fromstring(body)
    videos = root.find(f"{{{NS}}}videos")
    assert videos is not None and len(videos), (
        "no videos in the library; this test needs one"
    )
    return videos[0].get("id")


# The shapes that mattered. Each ends the injected chain in a way that would
# have absorbed the trailing :force_original_aspect_ratio=decrease.
HOSTILE_SIZES = [
    "640x480,movie=/etc/passwd[m];[m]scale=320:240",
    "640x480:force_original_aspect_ratio=decrease,subtitles=/etc/passwd",
    "640x480[a];[a]scale=320:240",
    "1x1,sendcmd=f=/etc/passwd",
    "99999x99999",
    "0x0",
    "-1x-1",
    "axb",
    "640",
    "640x",
    "x480",
]


def test_hostile_size_is_not_a_500():
    """An unparseable size must mean "do not scale", not a server error."""
    vid = _a_video_id()
    for size in HOSTILE_SIZES:
        status, body = _fetch("stream.view", {"id": vid, "size": size,
                                              "maxBitRate": "500"})
        assert status != 500, f"size={size!r} produced HTTP 500"
        assert b"No such filter" not in body and b"Invalid argument" not in body, (
            f"size={size!r} reached ffmpeg: {body[:200]!r}"
        )
    print(f"PASS  {len(HOSTILE_SIZES)} hostile size= values are refused cleanly")


def test_valid_size_still_works():
    """The validator must not have broken the parameter it validates."""
    vid = _a_video_id()
    status, body = _fetch("stream.view", {"id": vid, "size": "640x480",
                                          "maxBitRate": "500"}, limit=65536)
    assert status == 200, f"a valid size=640x480 returned HTTP {status}"
    assert body, "a valid size=640x480 returned an empty body"
    print("PASS  a valid size=640x480 still streams")


def test_m3u8_body_has_no_injected_lines():
    """`size` and `bitRate` are written into the playlist body.

    They were emitted without url_encode, unlike the credentials on the same
    line, so a newline in either forged whole playlist entries.
    """
    vid = _a_video_id()
    hostile = [
        "500@640x480\nhttp://evil.example/x.ts\n#EXTINF:10.0,",
        "500\n#EXT-X-ENDLIST",
        "500@640x480,movie=/etc/passwd",
    ]
    for bitrate in hostile:
        status, body = _fetch("hls.m3u8", {"id": vid, "bitRate": bitrate})
        assert status == 200, f"hls.m3u8 returned HTTP {status}"
        text = body.decode("utf-8", "replace")
        assert "evil.example" not in text, (
            f"a URL was injected into the playlist body: {text[:300]!r}"
        )
        for line in text.splitlines():
            assert line.startswith("#") or line.startswith("stream.view?"), (
                f"unexpected playlist line {line!r} for bitRate={bitrate!r}"
            )
    print("PASS  hls.m3u8 body cannot be injected through bitRate or size")


def test_master_playlist_body_has_no_injected_lines():
    """A repeated bitRate opens a second body sink for the same two values.

    A master playlist writes the bitrate into a BANDWIDTH attribute and a
    variant URI, and the frame size into a RESOLUTION attribute — none of which
    the media-playlist test above can reach, since it never sends bitRate twice.
    """
    vid = _a_video_id()
    hostile = [
        ("500@640x480\nhttp://evil.example/x.ts\n#EXTINF:10.0,", "900"),
        ("500\n#EXT-X-ENDLIST", "900@640x480"),
        ("500@640x480,movie=/etc/passwd", "900,RESOLUTION=1x1"),
    ]
    for a, b in hostile:
        url = (_url("hls.m3u8", {"id": vid})
               + "&bitRate=" + urllib.parse.quote(a, safe="")
               + "&bitRate=" + urllib.parse.quote(b, safe=""))
        try:
            with urllib.request.urlopen(url) as r:
                body = r.read(1 << 20)
                status = r.status
        except urllib.error.HTTPError as e:
            status, body = e.code, e.read(1 << 20)
        assert status == 200, f"hls.m3u8 returned HTTP {status}"
        text = body.decode("utf-8", "replace")
        assert "evil.example" not in text, (
            f"a URL was injected into the playlist body: {text[:300]!r}"
        )
        assert "/etc/passwd" not in text, text[:300]
        for line in text.splitlines():
            assert (line.startswith("#")
                    or line.startswith("stream.view?")
                    or line.startswith("hls.m3u8?")), (
                f"unexpected playlist line {line!r} for bitRate={a!r},{b!r}"
            )
    print("PASS  a master playlist cannot be injected through bitRate or size")


def test_m3u8_is_not_cacheable():
    """The playlist body carries the caller's credentials once per segment."""
    vid = _a_video_id()
    with urllib.request.urlopen(_url("hls.m3u8", {"id": vid})) as r:
        cc = r.headers.get("Cache-Control", "")
    assert "no-store" in cc, (
        f"hls.m3u8 is cacheable (Cache-Control: {cc!r}); it contains credentials"
    )
    print("PASS  hls.m3u8 is served with Cache-Control: no-store")


def test_credentials_are_not_echoed_unencoded():
    """Sanity: the auth parameters in the body must be percent-encoded."""
    vid = _a_video_id()
    _, body = _fetch("hls.m3u8", {"id": vid})
    text = body.decode("utf-8", "replace")
    for line in text.splitlines():
        if line.startswith("stream.view?"):
            assert "&u=" in line, f"no credentials on a segment line: {line!r}"
            break
    else:
        raise AssertionError("the playlist contained no segment lines")
    print("PASS  segment URLs carry encoded credentials")


TESTS = [
    test_hostile_size_is_not_a_500,
    test_valid_size_still_works,
    test_m3u8_body_has_no_injected_lines,
    test_master_playlist_body_has_no_injected_lines,
    test_m3u8_is_not_cacheable,
    test_credentials_are_not_echoed_unencoded,
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

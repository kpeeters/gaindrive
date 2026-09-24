#!/usr/bin/env python3
"""Streaming, transcoding and download tests.

Covers the transcode cache: a transcoded stream must carry a real
Content-Length and answer byte ranges, which is what makes it seekable for
third-party clients and resumable for offline downloads.

Start the server first (with a scanned music collection):
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_stream.py
"""

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


def _xml(endpoint, extra=None):
    extra = dict(extra or {})
    extra["f"] = "xml"
    _, _, body = _raw(endpoint, extra)
    return ET.fromstring(body)


def _first_song(suffix=None):
    """Finds a song id, optionally with a specific suffix. None if absent."""
    root = _xml("search3.view", {"query": "", "songCount": "200",
                                 "artistCount": "0", "albumCount": "0"})
    for s in root.iter(f"{{{NS}}}song"):
        if suffix is None or s.get("suffix") == suffix:
            return s.get("id"), s
    return None, None


SONG_ID, SONG = _first_song()


def _long_song(min_duration=150):
    """A track long enough that a 45 s buffer is visibly less than the file."""
    root = _xml("search3.view", {"query": "", "songCount": "200",
                                 "artistCount": "0", "albumCount": "0"})
    for s in root.iter(f"{{{NS}}}song"):
        if int(s.get("duration") or 0) >= min_duration:
            return s.get("id"), s
    return None, None


def _bytes_per_sec(song):
    br = int(song.get("bitRate") or 0)
    if br:
        return br * 125.0
    return int(song.get("size")) / float(song.get("duration"))


def _read_for(endpoint, extra, secs):
    """Reads a response for `secs` wall-clock seconds, then hangs up.

    Returns (bytes read, whether the body ended on its own).  The server logs
    a write failure for the hang-up, which is expected and not a fault.
    """
    req = urllib.request.Request(_url(endpoint, extra))
    got, done = 0, False
    deadline = time.monotonic() + secs
    with urllib.request.urlopen(req, timeout=10) as r:
        while time.monotonic() < deadline:
            chunk = r.read(65536)
            if not chunk:
                done = True
                break
            got += len(chunk)
    return got, done


def _need_song():
    assert SONG_ID, "no songs in the library - scan a collection first"


# ---- raw streaming ----------------------------------------------------

def test_raw_has_length():
    _need_song()
    status, hdrs, body = _raw("stream.view", {"id": SONG_ID})
    assert status == 200, status
    clen = hdrs.get("Content-Length")
    assert clen and int(clen) == len(body), f"Content-Length {clen} vs {len(body)}"
    print("PASS  raw stream carries a correct Content-Length")


def test_raw_range():
    _need_song()
    status, hdrs, body = _raw("stream.view", {"id": SONG_ID},
                              {"Range": "bytes=0-99"})
    assert status == 206, f"expected 206, got {status}"
    assert len(body) == 100, len(body)
    assert "Content-Range" in hdrs, hdrs
    print("PASS  raw stream honours a byte range")


# ---- transcoding ------------------------------------------------------

def test_opus_transcode_has_length():
    _need_song()
    status, hdrs, body = _raw("stream.view",
                              {"id": SONG_ID, "format": "opus",
                               "maxBitRate": "128"})
    assert status == 200, status
    assert hdrs.get("Transfer-Encoding") != "chunked", \
        "transcoded response is still chunked - the cache did not engage"
    clen = hdrs.get("Content-Length")
    assert clen and int(clen) == len(body), f"Content-Length {clen} vs {len(body)}"
    assert body.startswith(b"OggS"), f"not an Ogg stream: {body[:8]!r}"
    assert "ogg" in hdrs.get("Content-Type", ""), hdrs.get("Content-Type")
    print("PASS  opus transcode carries a correct Content-Length")


def test_transcode_range():
    """The compatibility fix: without this, third-party clients cannot seek."""
    _need_song()
    status, hdrs, body = _raw("stream.view",
                              {"id": SONG_ID, "format": "opus",
                               "maxBitRate": "128"},
                              {"Range": "bytes=0-99"})
    assert status == 206, f"expected 206, got {status}"
    assert len(body) == 100, len(body)
    print("PASS  transcoded stream honours a byte range")


def test_transcode_cache_hit():
    _need_song()
    args = {"id": SONG_ID, "format": "opus", "maxBitRate": "96"}
    t0 = time.monotonic()
    _, hdrs1, body1 = _raw("stream.view", args)
    first = time.monotonic() - t0

    t0 = time.monotonic()
    _, hdrs2, body2 = _raw("stream.view", args)
    second = time.monotonic() - t0

    assert body1 == body2, "cache returned different bytes"
    assert hdrs1.get("X-Gaindrive-Transcode") in ("hit", "miss"), \
        "first request did not go through the transcode cache"
    assert hdrs2.get("X-Gaindrive-Transcode") == "hit", \
        "second request re-ran ffmpeg instead of hitting the cache"
    print(f"PASS  transcode cache hit ({first:.2f}s cold, {second:.2f}s warm)")


def test_bogus_format_is_an_error():
    """Regression: this used to be a 200 with an empty body."""
    _need_song()
    root = _xml("stream.view", {"id": SONG_ID, "format": "bogus"})
    assert root.get("status") == "failed", ET.tostring(root)
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", ET.tostring(root)
    print("PASS  unsupported format returns error 10")


def test_m4a_target_produces_audio():
    """Regression: -f m4a is not a muxer, so ffmpeg exited writing nothing."""
    _need_song()
    status, hdrs, body = _raw("stream.view",
                              {"id": SONG_ID, "format": "m4a",
                               "maxBitRate": "128"})
    assert status == 200, status
    assert len(body) > 1024, f"empty or truncated body: {len(body)} bytes"
    assert "mp4" in hdrs.get("Content-Type", ""), hdrs.get("Content-Type")
    print("PASS  m4a transcode produces a non-empty audio/mp4 body")


def test_raw_format_is_passthrough():
    _need_song()
    _, _, plain = _raw("stream.view", {"id": SONG_ID})
    _, _, raw = _raw("stream.view", {"id": SONG_ID, "format": "raw"})
    assert plain == raw, "format=raw changed the bytes"
    print("PASS  format=raw is a pass-through")


def test_malformed_params_do_not_break_the_handler():
    _need_song()
    status, _, _ = _raw("stream.view", {"id": SONG_ID, "maxBitRate": "abc",
                                        "timeOffset": "x"})
    assert status == 200, f"malformed params gave {status}"
    print("PASS  malformed numeric params fall back to defaults")


# ---- pacing -----------------------------------------------------------
#
# pace=true asks the server to deliver at roughly 1x playback rate.  It exists
# for a client that hands the URL to something else - a Cast receiver fetching
# for itself - which the server cannot recognise from the request alone.  The
# property being asserted is that the server *stops*: unpaced it writes the
# whole track into the socket at once, blocks, and the idle connection is then
# torn down by the receiver's own no-data timeout or by a reverse proxy's.

READ_SECS = 8


def test_pace_throttles():
    sid, song = _long_song()
    if not sid:
        print("SKIP  pace (no track long enough in the library)")
        return
    bps  = _bytes_per_sec(song)
    got, done = _read_for("stream.view", {"id": sid, "pace": "true"}, READ_SECS)
    # 30 s of prebuffer, then TARGET_BUF (15 s) ahead of elapsed time.  The
    # bound is deliberately loose: this fails on a server that is not pacing at
    # all, which is the regression worth catching, not on one pacing slightly
    # differently.
    bound = (30 + 15 + READ_SECS) * bps * 1.5
    assert not done, "the whole track arrived within the read window"
    assert got < bound, f"{got} bytes in {READ_SECS}s, expected under {bound:.0f}"
    print(f"PASS  pace=true throttles ({got} bytes in {READ_SECS}s)")


def test_no_pace_is_unthrottled():
    """The default must stay fast: a pin or an offline download depends on it."""
    sid, song = _long_song()
    if not sid:
        print("SKIP  unpaced stream (no track long enough in the library)")
        return
    got, done = _read_for("stream.view", {"id": sid}, READ_SECS)
    assert done, f"unpaced stream did not finish in {READ_SECS}s ({got} bytes)"
    assert got == int(song.get("size")), f"{got} vs {song.get('size')}"
    print("PASS  a stream with no pace= arrives as fast as the socket takes it")


# ---- download ---------------------------------------------------------

def test_download_returns_original():
    _need_song()
    _, _, streamed = _raw("stream.view", {"id": SONG_ID})
    status, hdrs, downloaded = _raw("download.view", {"id": SONG_ID})
    assert status == 200, status
    assert downloaded == streamed, "download differs from the raw stream"
    assert int(SONG.get("size")) == len(downloaded), \
        f"size attribute {SONG.get('size')} vs {len(downloaded)} bytes"
    cd = hdrs.get("Content-Disposition", "")
    assert cd.startswith("attachment;"), cd
    assert "filename*=UTF-8''" in cd, cd
    print("PASS  download returns the original file with Content-Disposition")


def test_download_ignores_transcode_params():
    """download is defined as the original media, whatever else is asked for."""
    _need_song()
    _, _, plain = _raw("download.view", {"id": SONG_ID})
    _, _, asked = _raw("download.view", {"id": SONG_ID, "format": "opus",
                                          "maxBitRate": "64"})
    assert plain == asked, "download honoured a transcode parameter"
    print("PASS  download ignores format/maxBitRate")


def test_download_ignores_pace():
    """download.view is the original media data, and never paced.

    Guards serve_raw's hardcoded false against a later refactor: a paced
    download of a forty-minute FLAC would take forty minutes.
    """
    sid, song = _long_song()
    if not sid:
        print("SKIP  download pacing (no track long enough in the library)")
        return
    got, done = _read_for("download.view", {"id": sid, "pace": "true"}, READ_SECS)
    assert done, f"download.view honoured pace=true ({got} bytes)"
    assert got == int(song.get("size")), f"{got} vs {song.get('size')}"
    print("PASS  download ignores pace=true")


def test_download_missing_id():
    root = _xml("download.view")
    assert root.get("status") == "failed"
    err = root.find(f"{{{NS}}}error")
    assert err is not None and err.get("code") == "10", ET.tostring(root)
    print("PASS  download with no id returns error 10")


# ---- metadata ---------------------------------------------------------

def test_transcoded_fields_follow_the_cap():
    _need_song()
    root = _xml("getSong.view", {"id": SONG_ID, "maxBitRate": "64"})
    song = root.find(f"{{{NS}}}song")
    assert song is not None, ET.tostring(root)
    # Only advertised when the source actually exceeds the cap.
    if int(song.get("bitRate") or 0) > 64:
        assert song.get("transcodedSuffix") == "mp3", ET.tostring(song)
        assert song.get("transcodedBitRate") == "64", ET.tostring(song)
        print("PASS  transcoded* fields reflect the bitrate cap")
    else:
        print("SKIP  transcoded* fields (source is already below the cap)")


TESTS = [
    test_raw_has_length,
    test_raw_range,
    test_opus_transcode_has_length,
    test_transcode_range,
    test_transcode_cache_hit,
    test_bogus_format_is_an_error,
    test_m4a_target_produces_audio,
    test_raw_format_is_passthrough,
    test_malformed_params_do_not_break_the_handler,
    test_pace_throttles,
    test_no_pace_is_unthrottled,
    test_download_returns_original,
    test_download_ignores_transcode_params,
    test_download_ignores_pace,
    test_download_missing_id,
    test_transcoded_fields_follow_the_cap,
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

#!/usr/bin/env python3
"""`playable` on the audio path: what a client says it can be sent untouched.

The video half of this parameter is covered by tests/test_video.py.  What is
worth testing here is almost entirely the boundaries, because the failure the
feature can cause is not a slow stream but a wrong one:

  * a declared source is served as itself, with no ffmpeg between
  * an *undeclared* source of the same kind still transcodes -- the control,
    without which the first test passes on a server that ignores `format`
  * an ambiguous container is refused bare and honoured only as a pair
  * a pair naming the wrong codec does nothing
  * a declaration never beats maxBitRate or timeOffset
  * it never changes what the browse endpoints advertise, because that is what
    a Cast receiver is told it is about to fetch

Needs a library with at least one MP3.  The .m4a and .ogg cases skip when the
library has none.  Start the server first, then:
    python3 tests/test_playable.py
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

# The form the server stores for each extension whose codec its name settles,
# from implied_audio_form() in src/codecs.hh.  These need no file opened, so
# they are the ones that prove the whole library was filled rather than only
# the containers that had to be read.
IMPLIED_FORMS = {
    "mp3":  "mpeg/mp3",
    "flac": "flac/flac",
    "opus": "ogg/opus",
    "aac":  "adts/aac",
}

# What the scan has to open a file to learn, by extension.  The container it
# will have recorded is the *real* one, so `.oga` and `.ogg` are both `ogg` -
# which is the whole point of the pair being observed rather than derived.
READ_CONTAINERS = {"m4a": "mp4", "ogg": "ogg", "oga": "ogg"}

# What a passthrough of each must be labelled, from TARGETS in src/codecs.hh.
SOURCE_MIMES = {
    "mp3":  "audio/mpeg",
    "flac": "audio/flac",
    "opus": "audio/ogg",
    "aac":  "audio/aac",
    "m4a":  "audio/mp4",
    "ogg":  "audio/ogg",
    "oga":  "audio/ogg",
}

# Enough of a header to prove the bytes are the container they claim, for the
# ones that say so in their first bytes.  (offset, magic)
SOURCE_MAGIC = {
    "flac": (0, b"fLaC"),
    "opus": (0, b"OggS"),
    "ogg":  (0, b"OggS"),
    "oga":  (0, b"OggS"),
    "m4a":  (4, b"ftyp"),
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


def _xml(endpoint, extra=None):
    extra = dict(extra or {})
    extra["f"] = "xml"
    _, _, body = _raw(endpoint, extra)
    return ET.fromstring(body)


def _songs():
    root = _xml("search3.view", {"query": "", "songCount": "500",
                                 "artistCount": "0", "albumCount": "0"})
    return [s for s in root.iter(f"{{{NS}}}song")
            if s.get("isVideo") != "true"]


SONGS = _songs()


def _song_with(suffix):
    for s in SONGS:
        if s.get("suffix") == suffix:
            return s
    return None


def _transcoded(hdrs):
    """Whether this response came through the transcode cache."""
    return "X-Gaindrive-Transcode" in hdrs


# ---- the point of the feature ------------------------------------------


def test_declared_source_is_served_untouched():
    """format=opus plus a matching declaration means no ffmpeg at all.

    Uses the implied forms, which is also what proves the back-fill reached
    every audio row rather than only the containers it had to open.
    """
    done = 0
    for suffix, form in sorted(IMPLIED_FORMS.items()):
        s = _song_with(suffix)
        if s is None:
            continue
        status, hdrs, body = _raw("stream.view",
                                  {"id": s.get("id"), "format": "opus",
                                   "maxBitRate": "160",
                                   "playable": form},
                                  {"Range": "bytes=0-1023"})
        assert status == 206, f".{suffix}: expected 206, got {status}"
        assert "Content-Range" in hdrs, hdrs
        assert not _transcoded(hdrs), \
            f".{suffix} still went through the transcode cache: {hdrs}"
        assert hdrs.get("Content-Type") == SOURCE_MIMES[suffix], \
            f".{suffix}: got {hdrs.get('Content-Type')!r}"
        magic = SOURCE_MAGIC.get(suffix)
        if magic:
            off, want = magic
            assert body[off:off + len(want)] == want, \
                f"not a .{suffix}: {body[:12]!r}"
        done += 1
    if not done:
        print("SKIP  no mp3/flac/opus/aac in the library")
        return
    print(f"PASS  {done} declared source(s) served untouched")


def test_undeclared_source_still_transcodes():
    """The control: without the declaration the same request converts.

    Without this, the test above would pass against a server that had simply
    stopped honouring `format`.
    """
    s = _song_with("mp3")
    if s is None:
        print("SKIP  no mp3 in the library")
        return
    _, hdrs, _ = _raw("stream.view",
                      {"id": s.get("id"), "format": "opus",
                       "maxBitRate": "160"})
    assert hdrs.get("Content-Type") == "audio/ogg", \
        f"expected opus, got {hdrs.get('Content-Type')!r}"
    print("PASS  the same request without a declaration still transcodes")


# ---- ambiguous containers ---------------------------------------------


def _ambiguous():
    for suffix in ("m4a", "ogg", "oga"):
        s = _song_with(suffix)
        if s is not None:
            return suffix, s
    return None, None


def test_a_bare_audio_token_is_not_a_declaration():
    """A bare token names a *video* container, so it must match no audio file.

    The one that would go wrong quietly: a client still sending the old
    extension-shaped tokens gets transcodes, not the wrong bytes.
    """
    checked = 0
    for suffix in ("mp3", "flac", "opus", "aac", "m4a", "ogg", "oga"):
        s = _song_with(suffix)
        if s is None:
            continue
        _, hdrs, _ = _raw("stream.view",
                          {"id": s.get("id"), "format": "opus",
                           "maxBitRate": "160", "playable": suffix})
        assert _transcoded(hdrs), \
            f"a bare .{suffix} declaration was honoured: {hdrs}"
        checked += 1
    if not checked:
        print("SKIP  no audio in the library")
        return
    print(f"PASS  {checked} bare audio token(s) declare nothing")


def test_ambiguous_container_is_honoured_as_a_pair():
    """The codec half is what the server matches against songs.audio_codec.

    Skips rather than fails when the library has one of these but the scan has
    not recorded a codec for it -- an upgraded install back-fills on the next
    scan, and until then there is nothing to match.
    """
    suffix, s = _ambiguous()
    if s is None:
        print("SKIP  no m4a/ogg/oga in the library")
        return
    # Try every codec the container can hold; exactly one can match.  Note the
    # container named is the stored one, so a `.oga` is reached by `ogg/...`.
    container = READ_CONTAINERS[suffix]
    codecs = (["aac", "alac"] if container == "mp4"
              else ["vorbis", "opus", "flac", "speex"])
    hit = None
    for c in codecs:
        _, hdrs, body = _raw("stream.view",
                             {"id": s.get("id"), "format": "opus",
                              "maxBitRate": "160",
                              "playable": f"{container}/{c}"},
                             {"Range": "bytes=0-1023"})
        if not _transcoded(hdrs):
            hit = (c, hdrs, body)
            break
    if hit is None:
        print(f"SKIP  .{suffix} has no audio_codec recorded yet (rescan)")
        return
    c, hdrs, body = hit
    assert hdrs.get("Content-Type") == SOURCE_MIMES[suffix], \
        f"{container}/{c}: got {hdrs.get('Content-Type')!r}"
    off, want = SOURCE_MAGIC[suffix]
    assert body[off:off + len(want)] == want, \
        f"not a .{suffix}: {body[:12]!r}"
    print(f"PASS  a .{suffix} is served untouched as {container}/{c}")


def test_wrong_codec_in_a_pair_does_nothing():
    """A pair is matched verbatim, so naming the other codec must not hit."""
    suffix, s = _ambiguous()
    if s is None:
        print("SKIP  no m4a/ogg/oga in the library")
        return
    # A codec that container can hold but this file certainly is not, and one
    # that no container holds at all.
    container = READ_CONTAINERS[suffix]
    for bogus in ("mp3", "nonsense"):
        _, hdrs, _ = _raw("stream.view",
                          {"id": s.get("id"), "format": "opus",
                           "maxBitRate": "160",
                           "playable": f"{container}/{bogus}"})
        assert _transcoded(hdrs), \
            f"{container}/{bogus} was honoured: {hdrs}"
    # And the *extension* is not a container: whatever codec is paired with it,
    # nothing was ever stored under that name. This is what catches a `.oga`
    # being reachable as `oga/...` again.
    for c in ("aac", "alac", "vorbis", "opus", "flac"):
        _, hdrs, _ = _raw("stream.view",
                          {"id": s.get("id"), "format": "opus",
                           "maxBitRate": "160", "playable": f"{suffix}/{c}"})
        assert _transcoded(hdrs), f"{suffix}/{c} was honoured: {hdrs}"
    print(f"PASS  a mismatched pair on .{suffix} is ignored")


# ---- boundaries --------------------------------------------------------


def test_declaration_does_not_beat_max_bitrate():
    """A cap is an explicit constraint; a declaration is a preference."""
    s = None
    for c in SONGS:
        if c.get("suffix") == "mp3" and int(c.get("bitRate") or 0) > 128:
            s = c
            break
    if s is None:
        print("SKIP  no mp3 above 128 kbps in the library")
        return
    status, hdrs, _ = _raw("stream.view",
                           {"id": s.get("id"), "format": "opus",
                            "maxBitRate": "64", "playable": "mpeg/mp3"},
                           {"Range": "bytes=0-1023"})
    assert _transcoded(hdrs) or status != 206, \
        "a declaration beat maxBitRate: " + str(hdrs)
    print("PASS  a declaration does not beat maxBitRate")


def test_declaration_does_not_beat_time_offset():
    """A seek cannot be served from the head of the file."""
    s = _song_with("mp3")
    if s is None:
        print("SKIP  no mp3 in the library")
        return
    _, hdrs, _ = _raw("stream.view",
                      {"id": s.get("id"), "timeOffset": "5",
                       "format": "opus", "playable": "mpeg/mp3"})
    assert _transcoded(hdrs) or "Content-Length" not in hdrs, \
        "a declaration beat timeOffset: " + str(hdrs)
    print("PASS  a declaration does not beat timeOffset")


def test_declaration_does_not_change_metadata():
    """The advertised fields describe what *any* client is sent, and must.

    A Cast receiver picks its decode pipeline from transcodedContentType, so
    the day this starts varying per client is the day casting breaks.
    """
    s = _song_with("mp3")
    if s is None:
        print("SKIP  no mp3 in the library")
        return
    plain = _xml("getSong.view", {"id": s.get("id")})
    decl  = _xml("getSong.view", {"id": s.get("id"), "playable": "mpeg/mp3"})
    a = plain.find(f"{{{NS}}}song")
    b = decl.find(f"{{{NS}}}song")
    for field in ("transcodedContentType", "transcodedSuffix",
                  "transcodedBitRate", "contentType", "suffix"):
        assert a.get(field) == b.get(field), \
            f"{field} moved: {a.get(field)!r} -> {b.get(field)!r}"
    print("PASS  browse metadata is unchanged by a declaration")


def test_garbage_declaration_is_ignored():
    """Unrecognised tokens are dropped, not refused, and never 500."""
    s = _song_with("mp3")
    if s is None:
        print("SKIP  no mp3 in the library")
        return
    for value in ("../../etc/passwd", "a" * 500, ",,,", "nonsense",
                  "mp4/", "/aac", "mp4/a/b", "mpeg/", "wav", "riff/pcm",
                  "mpeg/mp3,,vob,,nonsense", "mp4/aac;drop"):
        status, _, _ = _raw("stream.view",
                            {"id": s.get("id"), "format": "opus",
                             "playable": value},
                            {"Range": "bytes=0-1023"})
        assert status in (200, 206), f"{value!r} gave HTTP {status}"
    # Case is folded: songs.codec is stored lowercased.
    _, hdrs, _ = _raw("stream.view",
                      {"id": s.get("id"), "format": "opus",
                       "playable": "MPEG/MP3"},
                      {"Range": "bytes=0-1023"})
    assert not _transcoded(hdrs), "MPEG/MP3 was not folded: " + str(hdrs)
    print("PASS  a malformed declaration is ignored rather than fatal")


def test_declaration_is_not_needed_for_raw():
    """Asking for the original never needed this, and still does not."""
    s = _song_with("mp3")
    if s is None:
        print("SKIP  no mp3 in the library")
        return
    _, hdrs, _ = _raw("stream.view", {"id": s.get("id")},
                      {"Range": "bytes=0-1023"})
    assert not _transcoded(hdrs), hdrs
    assert hdrs.get("Content-Type") == "audio/mpeg", hdrs.get("Content-Type")
    print("PASS  no format at all is still a passthrough")


TESTS = [
    test_declared_source_is_served_untouched,
    test_undeclared_source_still_transcodes,
    test_a_bare_audio_token_is_not_a_declaration,
    test_ambiguous_container_is_honoured_as_a_pair,
    test_wrong_codec_in_a_pair_does_nothing,
    test_declaration_does_not_beat_max_bitrate,
    test_declaration_does_not_beat_time_offset,
    test_declaration_does_not_change_metadata,
    test_garbage_declaration_is_ignored,
    test_declaration_is_not_needed_for_raw,
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

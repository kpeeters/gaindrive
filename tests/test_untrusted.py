#!/usr/bin/env python3
"""What the provider-string guards make of a hostile value.

Every field gaindrive takes from MusicBrainz, Wikidata, Wikipedia, TheAudioDB,
Discogs or TMDB is stored in a cache table and served to every client there is
- the web client, the iOS app, and any third-party Subsonic app. The server is
therefore the only place a guarantee about one can be made, and `src/untrusted.hh`
is where it is made. This is the table of cases it has to keep answering the
same way.

Like test_video_names.py and test_chapters_parse.py, this needs **no running
server** and no network:

    python3 tests/test_untrusted.py [path/to/gaindrive]

Values carry \\xNN, \\r, \\n, \\t and \\\\ escapes in both directions, because a
test whose whole subject is control characters cannot put them on a line raw.
"""

import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"

# (kind, value) -> expected output, both sides escaped.
#
# "" means the guard rejected the value outright. For a URL that is the whole
# design: the field is empty rather than present-and-hostile, which is what
# every consumer already reads as "no link".
CASES = [
    # --- URLs: an allow-list of two schemes ----------------------------
    # A scheme cannot be escaped, so it has to be validated. These are the
    # ones a client would turn into a working href.
    ("url", "javascript:alert(1)",                    ""),
    ("url", "JavaScript:alert(1)",                    ""),
    ("url", "data:text/html,<script>alert(1)</script>", ""),
    ("url", "vbscript:msgbox(1)",                     ""),
    ("url", "file:///etc/passwd",                     ""),
    # Scheme-relative resolves against whatever page holds it, so it inherits
    # the viewer's origin rather than naming one.
    ("url", "//evil.example/x",                       ""),
    ("url", "/wiki/Pink_Floyd",                       ""),
    ("url", "",                                       ""),
    ("url", "https://www.allmusic.com/artist/mn0000346336",
            "https://www.allmusic.com/artist/mn0000346336"),
    ("url", "http://a.example/x",                     "http://a.example/x"),
    # The scheme is compared case-insensitively; the rest is left alone,
    # because a path's case is the provider's business.
    ("url", "HTTPS://a.example/X",                    "HTTPS://a.example/X"),

    # A URL is split into host and path before it is fetched, so a control
    # character in one is a request-splitting attempt rather than a typo.
    ("url", "https://a.example/\\r\\nX-Injected: 1",  ""),
    ("url", "https://a.example/\\x00",                ""),
    ("url", "https://a.example/a b",                  ""),
    # Refused because a URL is the one provider string that reaches an HTML
    # attribute in every client that renders one.
    ("url", 'https://a.example/\\x22onload=x',        ""),
    ("url", "https://a.example/<img>",                ""),
    ("url", "https://a.example/\\xff",                ""),

    # --- Prose: paragraphs survive, everything else does not -----------
    # 0x09, 0x0A and 0x0D are the only C0 characters XML 1.0 permits, and the
    # newlines are the only structure these fields have.
    ("prose", "One.\\r\\n\\r\\nTwo.",                 "One.\\n\\nTwo."),
    ("prose", "One.\\rTwo.",                          "One.\\nTwo."),
    ("prose", "a\\tb",                                "a\\tb"),
    # One of these in a biography made the whole subsonic-response
    # unparseable for every conformant XML client, not merely that field.
    ("prose", "a\\x01b",                              "ab"),
    ("prose", "a\\x0bb",                              "ab"),
    ("prose", "a\\x1fb",                              "ab"),
    ("prose", "a\\x7fb",                              "ab"),
    # A NUL is worse than illegal: everything reaches tinyxml2 as .c_str(),
    # so it silently truncated the value instead.
    ("prose", "a\\x00b",                              "ab"),
    # Invalid UTF-8 is what made nlohmann's dump() throw, turning one artist
    # into a permanent 500 because the bytes were in a cache with no TTL.
    ("prose", "a\\xffb",                              "ab"),
    ("prose", "a\\xc3b",                              "ab"),
    # Valid multi-byte UTF-8 survives, and comes back as the character rather
    # than as its bytes - the output escape covers control characters only.
    ("prose", "Ry\\xc5\\xabichi Sakamoto",            "Ry\u016bichi Sakamoto"),
    # Markup is *not* stripped: the guards make a value safe to serialise, and
    # rendering it as text is the client's job. Stripping here would silently
    # rewrite prose that legitimately mentions a tag.
    ("prose", "<img src=x onerror=alert(1)>",         "<img src=x onerror=alert(1)>"),
    ("prose", "5 < 6 & 7 > 6",                        "5 < 6 & 7 > 6"),

    # --- Genres: a label, not prose ------------------------------------
    # video_meta.genre is a pipe-joined list, so a name carrying one would not
    # corrupt a row, it would silently become two genres - in a table that is
    # aggregated across the whole library.
    ("genre", "Science|Fiction",                      ""),
    ("genre", "|",                                    ""),
    ("genre", "Action & Adventure",                   "Action & Adventure"),
    ("genre", "  Drama\\t",                           "Drama"),
    # A line break in a label is damage rather than structure, which is the
    # one place this differs from prose.
    ("genre", "Dra\\nma",                             "Drama"),
    ("genre", "   ",                                  ""),
    ("genre", "Science\\x01Fiction",                  "ScienceFiction"),

    # --- MusicBrainz ids: concatenated into a URL path -----------------
    # The tag path has checked this shape since it existed, on the grounds
    # that a tag is arbitrary bytes somebody else wrote. So is a search hit.
    ("uuid", "f27ec8db-af05-4f36-916e-3d57f91ecf5e",  "yes"),
    ("uuid", "F27EC8DB-AF05-4F36-916E-3D57F91ECF5E",  "yes"),
    ("uuid", "f27ec8db-af05-4f36-916e-3d57f91ecf5",   "no"),
    ("uuid", "f27ec8dbaf054f36916e3d57f91ecf5e",      "no"),
    ("uuid", "../../../etc/passwd",                   "no"),
    ("uuid", "f27ec8db-af05-4f36-916e-3d57f91ecf5e/x", "no"),
    ("uuid", "",                                      "no"),
]


def run():
    stdin = "".join(f"{kind}:{value}\n" for kind, value, _ in CASES)
    p = subprocess.run([BINARY, "--untrusted-test"], input=stdin,
                       capture_output=True, text=True)
    if p.returncode != 0:
        print(f"FAIL  {BINARY} --untrusted-test exited {p.returncode}")
        if p.stderr:
            print(p.stderr.strip())
        return 1

    lines = p.stdout.split("\n")
    if lines and lines[-1] == "":
        lines.pop()
    if len(lines) != len(CASES):
        print(f"FAIL  got {len(lines)} output lines for {len(CASES)} cases")
        return 1

    failed = 0
    for (kind, value, want), got in zip(CASES, lines):
        if got != want:
            print(f"FAIL  {kind}:{value!r}\n      got {got!r}, expected {want!r}")
            failed += 1

    print(f"\n{len(CASES) - failed}/{len(CASES)} cases passed")
    return failed


if __name__ == "__main__":
    sys.exit(min(run(), 1))

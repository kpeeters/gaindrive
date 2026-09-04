#!/usr/bin/env python3
"""Chapter sidecar parsing rules.

A concert video's markers live in a sidecar `<stem>.chapters.txt` beside it, in
the format mp4chaps(1) exports. The parse is deliberately liberal, because the
other thing that lands in that file is a block pasted out of a YouTube
description -- so these are the rules that decide what a hand-edited file means.

Like tests/test_video_names.py and unlike everything else in tests/, this needs
**no running server**: it feeds files straight through --chapters-test.

    python3 tests/test_chapters_parse.py [path/to/gaindrive]

Each case is (file text) -> list of (formatted time, title). A case naming an
empty list asserts the file yields nothing at all.
"""

import os
import subprocess
import sys
import tempfile

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"

BOM = "﻿"

# text -> [(HH:MM:SS.mmm, title), ...]
CASES = [
    # --- what we write, and what mp4chaps writes -----------------------
    ("00:00:00.000 Shine On You Crazy Diamond\n"
     "00:13:35.000 Learning to Fly\n",
     [("00:00:00.000", "Shine On You Crazy Diamond"),
      ("00:13:35.000", "Learning to Fly")]),

    # --- what someone pastes out of a YouTube description ---------------
    ("0:00 Shine On You Crazy Diamond\n"
     "13:35 Learning to Fly\n"
     "19:02 High Hopes\n",
     [("00:00:00.000", "Shine On You Crazy Diamond"),
      ("00:13:35.000", "Learning to Fly"),
      ("00:19:02.000", "High Hopes")]),

    # A separator after the time is as common as the bare form. Without
    # stripping it, every title in half these files begins with "- ".
    ("13:35 - Learning to Fly\n"
     "19:02 | High Hopes\n"
     "20:00 – En Dash\n"
     "21:00 — Em Dash\n",
     [("00:13:35.000", "Learning to Fly"),
      ("00:19:02.000", "High Hopes"),
      ("00:20:00.000", "En Dash"),
      ("00:21:00.000", "Em Dash")]),

    # ...but a title that genuinely starts with a dash keeps it, because the
    # separator only counts when whitespace follows it.
    ("13:35 -Bleach\n", [("00:13:35.000", "-Bleach")]),

    # --- the rules that are decisions, not arithmetic -------------------
    # Minutes may exceed 59 in the two-field form: a hand-typed list of a long
    # concert routinely says 90:00, and rejecting it breaks the commonest file.
    ("90:00 Encore\n", [("01:30:00.000", "Encore")]),
    # But not in the three-field form, where it is a typo.
    ("13:99:00 Typo\n", []),
    # Seconds are always < 60.
    ("00:60 Typo\n", []),

    # The fraction is a decimal fraction, not a count of milliseconds:
    # 00:00.5 is half a second, not five thousandths of one.
    ("00:00.5 Half\n", [("00:00:00.500", "Half")]),
    # A comma is accepted as the decimal point, because SRT uses one and this
    # file is edited by the same people.
    ("12:34,500 Comma\n", [("00:12:34.500", "Comma")]),

    # The timestamp must be terminated by whitespace, or "13:35abc" would parse
    # as a time followed by a title.
    ("13:35abc Nope\n", []),
    # A bare number is a track index, not a time.
    ("5 Nope\n", []),
    # Three colons is not a time either.
    ("1:2:3:4 Nope\n", []),

    # --- the title is everything after the whitespace run, verbatim -----
    # This anchoring is what makes all three of these come out right with no
    # special cases at all.
    ("19:02 Pt. 1: High Hopes\n", [("00:19:02.000", "Pt. 1: High Hopes")]),
    ("45:00 2 Minutes To Midnight\n",
     [("00:45:00.000", "2 Minutes To Midnight")]),
    ("45:00 12:34 Reprise\n", [("00:45:00.000", "12:34 Reprise")]),

    # --- file-level shapes ----------------------------------------------
    # A BOM is stripped once. Without it exactly one chapter -- the first --
    # fails to parse while every other line is fine, which reads as a missing
    # marker rather than as an encoding problem. Notepad writes one.
    (BOM + "0:00 First\n13:35 Second\n",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),
    # CRLF.
    ("0:00 First\r\n13:35 Second\r\n",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),
    # Blank lines, comments, leading indentation.
    ("\n# a comment\n\n   0:00 First\n\n\t13:35 Second\n",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),
    # No trailing newline on the last line.
    ("0:00 First\n13:35 Second",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),
    # An empty file is the tombstone: no chapters, and that is not an error.
    ("", []),

    # One unparseable line loses that line, never the file -- a stray editor
    # line must not take every marker with it.
    ("0:00 First\nnot a line at all\n13:35 Second\n",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),

    # Sorted by start, so a hand-edited file need not be in order.
    ("13:35 Second\n0:00 First\n",
     [("00:00:00.000", "First"), ("00:13:35.000", "Second")]),
    # Equal starts keep file order rather than being merged: dropping one is
    # data loss on a file somebody typed.
    ("5:00 A\n5:00 B\n", [("00:05:00.000", "A"), ("00:05:00.000", "B")]),

    # A marker with no title is legal and stays empty -- the API boundary is
    # what fills in "Chapter N", so that saving a hand-edited file does not
    # rewrite lines left deliberately bare.
    ("13:35\n", [("00:13:35.000", "")]),

    # Two markers with the same name both survive. This is the case that
    # decided the wire format: httplib's query parser silently drops an exact
    # repeat of a whole key=value token, so repeated name= parameters would
    # have lost one and renamed every marker after it.
    ("0:00 Encore\n13:35 Encore\n",
     [("00:00:00.000", "Encore"), ("00:13:35.000", "Encore")]),

    # Hours are not truncated to two digits.
    ("100:00:00 Long\n", [("100:00:00.000", "Long")]),

    # A control character inside a title is dropped rather than escaped: the
    # format is line-based and has no escape syntax, so a stray one must not be
    # able to become another marker on the way back out.
    ("13:35 Bad\x01Title\n", [("00:13:35.000", "BadTitle")]),

    # The OGM/mkvmerge form is out of scope by decision, not by oversight.
    ("CHAPTER01=00:00:00.000\nCHAPTER01NAME=Intro\n", []),
]


def parse(text):
    """Runs one file through --chapters-test, returning [(time, title)]."""
    fd, path = tempfile.mkstemp(suffix=".chapters.txt")
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="") as f:
            f.write(text)
        r = subprocess.run([BINARY, "--chapters-test", path],
                           capture_output=True, text=True)
        if r.returncode != 0:
            raise AssertionError(f"exit {r.returncode}: {r.stderr.strip()}")
        out = []
        for line in r.stdout.splitlines():
            if not line:
                continue
            # index, formatted time, title
            parts = line.split("\t")
            out.append((parts[1], parts[2] if len(parts) > 2 else ""))
        return out
    finally:
        os.unlink(path)


def run():
    failed = 0
    for text, expect in CASES:
        label = repr(text if len(text) <= 46 else text[:43] + "...")
        try:
            got = parse(text)
        except Exception as e:                        # noqa: BLE001
            print(f"FAIL  {label}: {e}")
            failed += 1
            continue
        if got != expect:
            print(f"FAIL  {label}:\n        got      {got}\n"
                  f"        expected {expect}")
            failed += 1

    # A round trip through the writer must be a fixed point, or every save
    # walks the file. This is where a truncating llround would show up.
    canonical = "".join(f"{t} {n}\n" for t, n in
                        [("00:00:00.000", "First"),
                         ("00:13:35.250", "Second"),
                         ("01:30:00.999", "Third")])
    got = parse(canonical)
    want = [("00:00:00.000", "First"), ("00:13:35.250", "Second"),
            ("01:30:00.999", "Third")]
    if got != want:
        print(f"FAIL  round trip:\n        got      {got}\n"
              f"        expected {want}")
        failed += 1

    total = len(CASES) + 1
    print(f"\n{total - failed}/{total} cases passed")
    return failed


if __name__ == "__main__":
    sys.exit(min(run(), 1))

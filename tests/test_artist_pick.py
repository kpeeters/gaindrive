#!/usr/bin/env python3
"""Which MusicBrainz artist a folder name resolves to.

An artist folder's name is all the scanner has, and whichever MusicBrainz
artist it picks gets to supply that folder's biography, portrait and Last.fm
link -- cached, with nothing downstream able to tell a wrong answer from a
right one. So these are the rules that decide it.

Like tests/test_video_names.py and tests/test_chapters_parse.py, and unlike
everything else in tests/, this needs **no running server** and no network: it
feeds canned search responses through --artist-pick-test.

    python3 tests/test_artist_pick.py [path/to/gaindrive]

Every body here is trimmed from a real response, and the id and score in each
are MusicBrainz's own -- a case whose numbers were invented would be a test of
nothing.
"""

import json
import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"


def artist(mbid, name, score, aliases=None):
    a = {"id": mbid, "name": name, "score": score}
    if aliases is not None:
        a["aliases"] = [{"name": x} for x in aliases]
    return a


def body(*artists):
    return json.dumps({"created": "x", "count": len(artists),
                       "offset": 0, "artists": list(artists)})


HIROMI = "8472f0ce-c57d-46f2-93db-d4a6f6e6473a"
SAKAMOTO = "a7f7df4a-77d8-4f12-8acd-5c60c93f4de8"
ALVA_NOTO = "6edc70bb-c340-4ae6-bdd4-d5fb0c7411de"
PINK_FLOYD = "83d91898-7763-47d7-b03b-b92132375c47"
ZIPPY_KID = "6b656576-9e05-46b5-b9d0-16b1d9f0ff4f"
MILES = "561d854a-6a28-4aa7-8c99-323e6ce46c2a"
ACDC = "66c662b6-6e2f-4930-8610-912e24c63ed1"
SIGUR_ROS = "f6f2326f-6b25-4170-b89d-e235b25508e8"

# (label, folder name, response body, expected mbid or None)
CASES = [
    # --- the bug this file was written for -----------------------------
    # MusicBrainz's `artist` field holds the primary name only. Hers is
    # 上原ひろみ, so the romanization everyone files her under is an alias and
    # nothing else -- and searching the name field alone returned count 0,
    # which was then cached as "this artist does not exist".
    ("alias is the only spelling that matches",
     "Hiromi Uehara",
     body(artist(HIROMI, "上原ひろみ", 100,
                 ["Hiromi's Sonicwonder", "Hiromi Uehara", "Hiromi",
                  "上原 ひろみ", "UEHARA Hiromi"])),
     HIROMI),

    # --- exactness outranks score --------------------------------------
    # The live ordering for this query. A search ranks a name *containing* the
    # query above one that only lists it as an alias, so first-hit-wins gives
    # this folder a duo's biography. It is the case that proves the rule is
    # about more than the widened query: widening alone still returns the duo
    # first.
    ("a collaboration outscores the artist himself",
     "Ryuichi Sakamoto",
     body(artist(ALVA_NOTO, "Alva Noto + Ryuichi Sakamoto", 100,
                 ["Alva Noto and Ryuichi Sakamoto"]),
          artist(SAKAMOTO, "坂本龍一", 80,
                 ["Ryūichi Sakamoto", "Ryûichi Sakamoto", "坂本 龍一",
                  "Sakamoto Ryūichi", "R.S."]),
          artist("130ef082-0000-0000-0000-000000000000",
                 "Ryuichi Sakamoto & Hildur Guðnadóttir", 60)),
     SAKAMOTO),

    # ...and the only reason that case resolves is the diacritic fold: there
    # is no plain-ASCII "Ryuichi Sakamoto" among his aliases, only Ryūichi and
    # Ryûichi. Strip the fold and this reverts to the duo.
    ("a macron is not a different artist",
     "Ryuichi Sakamoto",
     body(artist(ALVA_NOTO, "Alva Noto + Ryuichi Sakamoto", 100),
          artist(SAKAMOTO, "坂本龍一", 80, ["Ryūichi Sakamoto"])),
     SAKAMOTO),

    # The fold works on the primary name too, not only on aliases.
    ("an unaccented folder finds an accented name",
     "Sigur Ros",
     body(artist(SIGUR_ROS, "Sigur Rós", 100, ["Sigur Ros"])),
     SIGUR_ROS),

    # --- score still breaks ties among exact matches --------------------
    # Widening the query to the alias field drags in junk: an unrelated artist
    # really does carry "Pink Floyd" as an alias. Both are exact, so the score
    # is what separates them -- which is why exactness is a first key and not
    # the only one.
    ("score decides between two exact matches",
     "Pink Floyd",
     body(artist(PINK_FLOYD, "Pink Floyd", 100, ["The Pink Floyd", "Floyd"]),
          artist(ZIPPY_KID, "Zippy Kid", 63, ["Pink Floyd", "sHEcrIEZ"])),
     PINK_FLOYD),

    # An exact match wins from behind, whatever the order of the array.
    ("an exact match wins from any position",
     "Miles Davis",
     body(artist("fe7245e7-0000-0000-0000-000000000000",
                 "Miles Davis Quintet", 100),
          artist(MILES, "Miles Davis", 96, ["Miles Dewey Davis III"])),
     MILES),

    # --- names that are punctuation ------------------------------------
    # artist_key() treats a folder as a filename, so "AC-DC" on disk and
    # "AC/DC" at MusicBrainz are the same artist. The slash also needs no
    # Lucene escape, being literal inside a quoted phrase.
    ("punctuation does not separate two spellings",
     "AC-DC",
     body(artist(ACDC, "AC/DC", 100), artist("74a1057a-x", "AC/DC UK", 63)),
     ACDC),

    # --- a guess is refused --------------------------------------------
    # Nothing matched exactly and the best hit is weak, so no id is recorded.
    # A wrong one is worse than none: everything downstream is attributed to
    # whoever it named, and cached.
    ("a weak inexact hit is no answer",
     "Some Band That Is Not There",
     body(artist("11111111-x", "Some Other Band", 42),
          artist("22222222-x", "Third Band", 30)),
     None),

    # ...but a strong inexact hit is still taken, which is what keeps a folder
    # carrying a release tag or a stray article resolving as it did before.
    ("a strong inexact hit is still taken",
     "Portishead 1994",
     body(artist("33333333-x", "Portishead", 88)),
     "33333333-x"),

    # --- bodies that are not results -----------------------------------
    ("an empty result set", "Anybody", body(), None),
    ("MusicBrainz's busy 503 body", "Anybody",
     '{"error": "The MusicBrainz web server is currently busy."}', None),
    ("malformed JSON", "Anybody", "<!DOCTYPE html><html>not json", None),
    ("an empty body", "Anybody", "", None),
    # `artists` arriving as something other than an array must not be walked.
    ("artists is not an array", "Anybody", '{"artists": 3}', None),
    # A candidate with no id is no candidate, whatever its score.
    ("a hit with no id", "Anybody",
     '{"artists":[{"name":"Anybody","score":100}]}', None),
    # A name that keys to nothing must not match an alias that also keys to
    # nothing -- two empty keys are not the same artist.
    ("a folder named only in punctuation", "!!!",
     body(artist("44444444-x", "Some Band", 100, [""])), None),
]


def pick(name, response):
    """Runs one response through --artist-pick-test, returning the mbid."""
    r = subprocess.run([BINARY, "--artist-pick-test", name],
                       input=response, capture_output=True, text=True)
    if r.returncode != 0:
        raise AssertionError(f"exit {r.returncode}: {r.stderr.strip()}")
    out = r.stdout.strip()
    return None if out == "(no match)" else out.split("\t")[0]


def query(name):
    """The query line --artist-pick-test prints on stderr."""
    r = subprocess.run([BINARY, "--artist-pick-test", name],
                       input=body(), capture_output=True, text=True)
    for line in r.stderr.splitlines():
        if line.startswith("query: "):
            return line[len("query: "):]
    raise AssertionError("no query line on stderr")


# The query has to name both fields, or the alias half of every case above is
# unreachable however good the matching rule is. And the name is escaped,
# because it lands in a Lucene query: a folder holding a double quote would
# otherwise close the phrase and have its own name parsed as query syntax.
QUERIES = [
    ("Hiromi Uehara", 'artist:"Hiromi Uehara" OR alias:"Hiromi Uehara"'),
    ("AC/DC", 'artist:"AC/DC" OR alias:"AC/DC"'),
    ('Quote" OR alias:"x',
     'artist:"Quote\\" OR alias:\\"x" OR alias:"Quote\\" OR alias:\\"x"'),
    ("Back\\slash", 'artist:"Back\\\\slash" OR alias:"Back\\\\slash"'),
]


def run():
    failed = 0
    for label, name, response, expect in CASES:
        try:
            got = pick(name, response)
        except Exception as e:                        # noqa: BLE001
            print(f"FAIL  {label}: {e}")
            failed += 1
            continue
        if got != expect:
            print(f"FAIL  {label} [{name}]:\n        got      {got}\n"
                  f"        expected {expect}")
            failed += 1

    for name, expect in QUERIES:
        try:
            got = query(name)
        except Exception as e:                        # noqa: BLE001
            print(f"FAIL  query [{name}]: {e}")
            failed += 1
            continue
        if got != expect:
            print(f"FAIL  query [{name}]:\n        got      {got}\n"
                  f"        expected {expect}")
            failed += 1

    total = len(CASES) + len(QUERIES)
    print(f"\n{total - failed}/{total} cases passed")
    return failed


if __name__ == "__main__":
    sys.exit(1 if run() else 0)

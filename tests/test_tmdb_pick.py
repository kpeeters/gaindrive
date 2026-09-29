#!/usr/bin/env python3
"""Which TMDB search result a title and year resolve to.

The pick is where a wrong poster comes from: whichever result it returns gets
to overwrite the album title and supply the plot, and nothing downstream can
tell a wrong answer from a right one. Like tests/test_artist_pick.py this
needs **no running server**, no network and no API key: it feeds canned search
response bodies through --tmdb-pick-test.

    python3 tests/test_tmdb_pick.py [path/to/gaindrive]

The first case is the bug this file was written for: "A_Complete_Unknown_2024"
parsed down to title "A", and the year branch used to take the first popular
result within one year of 2024 without ever comparing titles, which was
"A Desert" (2025).
"""

import json
import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"


def movie(mid, title, date, overview="x"):
    return {"id": mid, "title": title, "release_date": date,
            "overview": overview, "poster_path": "/x.jpg", "genre_ids": []}


def body(*movies):
    return json.dumps({"page": 1, "results": list(movies),
                       "total_pages": 1, "total_results": len(movies)})


# (label, query title, query year, response body, expected id or None)
CASES = [
    # --- the year alone is not a match ----------------------------------
    ("a mangled title does not take the year's word for it",
     "A", 2024,
     body(movie(1156593, "A Desert", "2025-01-31")),
     None),

    # --- exact fold outranks popularity order ---------------------------
    ("the right film beats a more popular year-mate",
     "A Complete Unknown", 2024,
     body(movie(1156593, "A Desert", "2025-01-31"),
          movie(661539, "A Complete Unknown", "2024-12-25")),
     661539),

    # --- the year still disambiguates same-named films ------------------
    ("Zulu 2013 is not Zulu 1964",
     "Zulu", 2013,
     body(movie(19910, "Zulu", "1964-01-22"),
          movie(171372, "Zulu", "2013-05-19")),
     171372),

    # --- containment accepts a dropped article --------------------------
    ("Intouchables finds The Intouchables",
     "Intouchables", 2011,
     body(movie(77338, "The Intouchables", "2011-11-02")),
     77338),

    # --- ...but only when the shorter side is substantial ----------------
    ("a two-letter title gets no containment credit",
     "It", 2017,
     body(movie(371638, "It Ends With Us", "2016-08-07")),
     None),
    ("a two-letter title still matches exactly",
     "It", 2017,
     body(movie(371638, "It Ends With Us", "2016-08-07"),
          movie(346364, "It", "2017-09-06")),
     346364),

    # --- no year: the bar is exact, folded -------------------------------
    ("folding bridges punctuation TMDB keeps",
     "Wall E", 0,
     body(movie(10681, "WALL·E", "2008-06-22")),
     10681),
    ("no year and no exact title is no match",
     "A", 0,
     body(movie(1156593, "A Desert", "2025-01-31")),
     None),
]

# Under a music root the performer is known, and a candidate must show it.
# (label, query title, query year, artist, response body, expected id or None)
ARTIST_CASES = [
    ("the artist-prefixed title is accepted outright",
     "Live at Wembley", 0, "Queen",
     body(movie(1, "Queen: Live at Wembley", "1986-07-12")),
     1),
    ("a bare title with the artist in the overview",
     "Live at Wembley", 1986, "Queen",
     body(movie(2, "Live at Wembley", "1986-07-12",
                "Queen play Wembley Stadium in 1986.")),
     2),
    ("a bare title with no sign of the artist is another band's film",
     "Live at Wembley", 1986, "Queen",
     body(movie(3, "Live at Wembley", "1986-06-01",
                "The other band's stadium show.")),
     None),
    ("a short artist name is matched as a word, not inside one",
     "Close to the Edge", 0, "Yes",
     body(movie(4, "Close to the Edge", "",
                "Before their eyes the stage fills.")),
     None),
    ("the artist-prefixed title rejects a year far off",
     "Live at Wembley", 1986, "Queen",
     body(movie(5, "Queen: Live at Wembley", "2003-01-01")),
     None),
]


def run():
    failed = 0
    cases = [(l, t, y, "", r, w) for (l, t, y, r, w) in CASES] + ARTIST_CASES
    for label, title, year, artist, resp, want in cases:
        try:
            p = subprocess.run(
                [BINARY, "--tmdb-pick-test", title, "--tmdb-year", str(year)]
                + (["--tmdb-artist", artist] if artist else []),
                input=resp, capture_output=True, text=True, timeout=30)
        except FileNotFoundError:
            print(f"FAIL  no binary at {BINARY}; pass its path as the first "
                  f"argument")
            return 1
        if p.returncode != 0:
            print(f"FAIL  {label!r}: exited {p.returncode}: "
                  f"{p.stderr.strip()}")
            failed += 1
            continue
        # The pick logs its reasoning through the ordinary stamped log, so
        # the verdict is the last line: "title\tyear\tid" or "(no match)".
        lines = [l for l in p.stdout.splitlines() if l.strip()]
        verdict = lines[-1] if lines else ""
        if want is None:
            if verdict != "(no match)":
                print(f"FAIL  {label!r}: expected no match, got {verdict!r}")
                failed += 1
            continue
        parts = verdict.split("\t")
        if len(parts) != 3 or parts[2] != str(want):
            print(f"FAIL  {label!r}: expected id {want}, got {verdict!r}")
            failed += 1

    print(f"\n{len(cases) - failed}/{len(cases)} cases passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(run())

#!/usr/bin/env python3
"""Filename → title/year/episode rules.

Video files carry no tag anything writes, so the filename is the only metadata
there is, and it is usually a name meant for a tracker rather than for a
person. These are the rules that turn it back into something a client can show
and a metadata provider can be asked with.

Unlike every other script in tests/, this one needs **no running server** — it
pipes names through the parser directly:

    python3 tests/test_video_names.py [path/to/gaindrive]

Inputs may carry folder context, written as "Parent/Folder/Name", because the
title is as often on the folder as on the file. Each case asserts only the
fields it is actually about; anything unstated is not checked, so tuning the
stop-word list does not break unrelated cases.
"""

import subprocess
import sys

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"

# name -> expected fields. Keys: title, year, ep, source, cleaned, series.
CASES = {
    # --- scene-style, dotted -------------------------------------------
    "The.Third.Man.1949.1080p.BluRay.x264-GRP":
        {"title": "The Third Man", "year": "1949"},
    "Some.Doc.2019.WEBRip.XviD.MP3-XVID":
        {"title": "Some Doc", "year": "2019"},
    "www.Torrenting.com - Big.Film.2019.720p":
        {"title": "Big Film", "year": "2019"},
    "Arrival.2016.2160p.UHD.BluRay.x265.HDR10.DTS-HD.MA.7.1-TERMiNAL":
        {"title": "Arrival", "year": "2016"},

    # --- Kodi / Jellyfin -----------------------------------------------
    "The Third Man (1949)":
        {"title": "The Third Man", "year": "1949"},
    "Star Trek II The Wrath of Khan (1982)":
        {"title": "Star Trek II The Wrath of Khan", "year": "1982"},

    # --- plain descriptive, nothing to strip ---------------------------
    # The safety property: nothing matched, so nothing changed.
    "cheese making documentary":
        {"title": "cheese making documentary", "year": "", "cleaned": "as-is"},
    "Spider-Man":
        {"title": "Spider-Man", "cleaned": "as-is"},

    # --- the three cases that decide the rules -------------------------
    # Last year wins, so the 2049 in the title survives and 2017 is the year.
    "Blade.Runner.2049.2017.1080p":
        {"title": "Blade Runner 2049", "year": "2017"},
    # A year followed by a real word is not a year.
    "holiday 2019 crete":
        {"title": "holiday 2019 crete", "year": "", "cleaned": "as-is"},
    # The file says nothing; the folder does.
    "The Third Man (1949)/title00":
        {"title": "The Third Man", "year": "1949", "source": "folder"},

    # A title that is *only* a year, disambiguated by a bracketed one. The
    # bracketed year is authoritative, so no token in the title can be a
    # second one — without that rule this loses its whole title.
    "1917 (2019)":
        {"title": "1917", "year": "2019"},
    "Blade Runner 2049 (2017)":
        {"title": "Blade Runner 2049", "year": "2017"},
    # A title opening with a word the stop-word table calls release junk.
    # Cutting at the first token would empty it, and no name is improved by
    # being emptied.
    "4K Nature Scenes":
        {"title": "4K Nature Scenes", "cleaned": "as-is"},
    "HD Home Video 2015":
        {"title": "HD Home Video", "year": "2015"},
    # Digits and a hyphen inside the title itself.
    "Se7en.1995.REMASTERED.1080p.BluRay.x264-AMIABLE":
        {"title": "Se7en", "year": "1995"},
    "WALL-E (2008)":
        {"title": "WALL-E", "year": "2008"},

    # A year token that is part of the title, with nothing after it.
    "2001 A Space Odyssey":
        {"title": "2001 A Space Odyssey", "year": "", "cleaned": "as-is"},
    # ...and the same title with junk after it, where the year is real.
    "2001.A.Space.Odyssey.1968.1080p.BluRay":
        {"title": "2001 A Space Odyssey", "year": "1968"},

    # A leading number orders episodes but must never claim the title:
    # "12 Angry Men" is a film.
    "12 Angry Men":
        {"title": "12 Angry Men", "ep": "E12", "cleaned": "as-is"},

    # --- series ---------------------------------------------------------
    "Planet Earth II/Season 01/Planet.Earth.II.S01E03.Jungles.1080p":
        {"title": "Jungles", "ep": "S1E3", "series": "Planet Earth II"},
    # No episode name anywhere: the show comes from two levels up, since the
    # folder is a season.
    "Planet Earth II/Season 01/S01E03":
        {"title": "Episode 3", "ep": "S1E3", "series": "Planet Earth II"},
    "Breaking Bad/Season 02/2x05 - Breakage":
        {"title": "Breakage", "ep": "S2E5", "series": "Breaking Bad"},

    # --- the season from the folder --------------------------------------
    # Episodes regularly do not repeat the season the folder already states,
    # and the season is what groups them in a client, so the folder has to
    # supply it. The leading number still orders them.
    "Planet Earth II/Season 02/03 Jungles":
        {"title": "03 Jungles", "ep": "S2E3", "series": "Planet Earth II"},
    "Blackadder/Series 4/title00":
        {"title": "Blackadder", "ep": "S4E0", "source": "folder"},
    "Wallander/Seizoen 3/aflevering":
        {"ep": "S3E0", "series": "Wallander"},
    # The file is the more specific claim: an S03E01 sitting in "Season 02" is
    # a misfiled episode, not a season 2 one.
    "Show/Season 02/Show.S03E01":
        {"ep": "S3E1", "series": "Show"},
    # A disc is not a season. This is the whole reason the folder rules use a
    # narrower pattern than the one that finds the show's name: reading "2"
    # here would label a two-disc film as a series.
    "Some Film/Disc 2/title00":
        {"title": "Some Film", "ep": "", "source": "folder"},
    "Some Film/CD1/part":
        {"ep": ""},
    # An unnumbered folder says nothing, and must not: a Specials folder read
    # as a season would collide with season 1.
    "Planet Earth II/Specials/making of":
        {"ep": ""},

    # --- uninformative filenames ---------------------------------------
    "Some Documentary/title00":
        {"title": "Some Documentary", "source": "folder"},
    "Movies/The Third Man (1949)/movie":
        {"title": "The Third Man", "year": "1949", "source": "folder"},
    "Big Film 2019/VIDEO_TS":
        {"title": "Big Film", "year": "2019", "source": "folder"},
    # The vts arm of UNINFORMATIVE, both halves.  A short one still matches;
    # the long one is the ReDoS regression — the old nested-quantifier
    # spelling took exponential time on exactly this shape (many digits, then
    # one letter that forces every partition to be tried), so the assertion
    # here is as much "the parser returns at all" as what it returns.
    "Big Film 2019/vts_01_2":
        {"title": "Big Film", "year": "2019", "source": "folder"},
    # Asserts nothing about the parse — only that a line comes back, which
    # the old spelling did not do within the age of the universe.
    "Junk 2020/vts111111111111111111111111111111111111111111111111111111111111111111111111x":
        {},
    # A part marker ends the title, and the folder supplies the year.
    "Big Film 2019/BigFilm.CD1":
        {"title": "BigFilm", "year": "2019"},

    # --- explicit overrides ---------------------------------------------
    "The Matrix [tmdbid=603]":
        {"title": "The Matrix"},
    "Some Obscure Film [imdbid=tt0090605]":
        {"title": "Some Obscure Film"},

    # --- real extensions are stripped, fake ones are not ----------------
    "The Third Man (1949).mkv":
        {"title": "The Third Man", "year": "1949"},

    # --- language words are title words, junk only in junk company -------
    # The reported file: "french" is a release tag in the stop-word table
    # and an ordinary adjective here. Followed by the year it stays title
    # text, and the year then anchors the cut at 720p — which also recovers
    # the year the lowercase group tag ("titler") used to defeat.
    "La French [The Connection] 2014 720p BRRip x264 titler":
        {"title": "La French", "year": "2014"},
    "The.English.Patient.1996.720p.BluRay.x264-GRP":
        {"title": "The English Patient", "year": "1996"},
    # A language tag in the company of junk still cuts...
    "Amelie.2001.FRENCH.1080p.BluRay":
        {"title": "Amelie", "year": "2001"},
    # ...with or without a year to anchor on.
    "Un.Film.FRENCH.DVDRip":
        {"title": "Un Film"},

    # --- a bracketed year marks where the title ends ---------------------
    # "Title (Year)" is a human convention, so what precedes the bracket is
    # the title verbatim — the only rule that can save a title-*final*
    # language word, which is shape-identical to a tag.
    "The.Girl.Who.Was.French.(2037).DVDRip":
        {"title": "The Girl Who Was French", "year": "2037"},
    # ...and it outranks a strong stop word sitting inside the title.
    "Charlottes Web (2006) 720p BluRay x264":
        {"title": "Charlottes Web", "year": "2006"},
    # Year-first naming: an empty prefix falls through to the token rules.
    "(2014) Some Movie":
        {"title": "Some Movie", "year": "2014"},
}

FIELDS = ["label", "title", "year", "ep", "source", "cleaned", "series"]


def run():
    names = "\n".join(CASES) + "\n"
    try:
        # The timeout is part of the test: the ReDoS case above regresses as
        # a hang, and a hung test reports nothing.
        p = subprocess.run([BINARY, "--video-name-test", "-"],
                           input=names, capture_output=True, text=True,
                           timeout=30)
    except FileNotFoundError:
        print(f"FAIL  no binary at {BINARY} — pass its path as the first "
              f"argument")
        return 1
    except subprocess.TimeoutExpired:
        print("FAIL  parser did not finish in 30 s — a name in the table "
              "backtracks; see the ReDoS case")
        return 1
    if p.returncode != 0:
        print(f"FAIL  {BINARY} exited {p.returncode}: {p.stderr.strip()}")
        return 1

    got = {}
    for line in p.stdout.splitlines():
        if not line.strip():
            continue
        parts = line.split("\t")
        # Trailing empty fields may be dropped by nothing here, but be lenient
        # so a future extra column does not break every case at once.
        parts += [""] * (len(FIELDS) - len(parts))
        row = dict(zip(FIELDS, parts))
        got[row["label"]] = row

    failed = 0
    for name, expect in CASES.items():
        row = got.get(name)
        if row is None:
            print(f"FAIL  {name!r}: no output line")
            failed += 1
            continue
        for field, want in expect.items():
            if row[field] != want:
                print(f"FAIL  {name!r}: {field} = {row[field]!r}, "
                      f"expected {want!r}")
                failed += 1

    checks = sum(len(e) for e in CASES.values())
    print(f"\n{checks - failed}/{checks} assertions passed "
          f"over {len(CASES)} names")
    return failed


if __name__ == "__main__":
    sys.exit(min(run(), 1))

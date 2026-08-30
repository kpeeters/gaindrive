#!/usr/bin/env python3
"""Dump, and compare, the columns a scan derives by *reading* each file.

Every change that makes a scan faster by asking TagLib or ffprobe to do less
work trades accuracy for speed, and the argument for whether that trade is safe
should be a measurement over the whole library rather than a reading of upstream
source. So: dump before, change, rescan, dump after, compare.

Unlike most scripts in tests/, this one needs no running server — it reads the
music database directly, read-only, which is safe while gaindrive is running
because both databases are WAL.

    python3 tests/dump_derived.py /var/lib/gaindrive/gaindrive-music.db > before.tsv
    # rebuild, delete the music DB, rescan
    python3 tests/dump_derived.py /var/lib/gaindrive/gaindrive-music.db > after.tsv
    python3 tests/dump_derived.py --compare before.tsv after.tsv [examples]

`examples` caps how many differing rows are printed per column (default 5); pass
0 for all of them. **Both dumps must come from the same version of this script**
— it decides how a tag's control characters are rendered, so comparing across a
change to that shows differences the library does not have.

**Keyed on path, never on id.** A rowid is reassigned by a rebuild — that is the
same instability stars and playlists key around — so two dumps of the same
library have unrelated ids and identical paths.

Only what a scan derives from opening the file is included. Ids, `last_scanned`
and `created` are excluded because they change every scan and would bury the
signal; `file_size` and `file_modified` because they come from a stat rather
than from reading; album and folder rows because nothing here changes them.
"""

import sqlite3
import sys

# path first: it is the key. The rest is everything TagLib or ffprobe puts in a
# row, so one dump serves both an ffprobe-flag experiment and a TagLib one.
COLUMNS = [
	"path",
	"title", "track_number", "disc_number", "year", "genre", "artist",
	"duration", "bitrate", "sample_rate", "channels",
	"is_video", "width", "height", "video_codec", "audio_codec", "season",
	]

# NULL has to survive the round trip as something no value can collide with:
# songs.artist distinguishes NULL ("never read") from '' ("read, no tag"), and a
# dump that flattened the two would hide exactly the regression worth catching.
# It cannot collide with a real value because a real backslash is doubled below.
NULL = "\\N"

# Every C0 control character plus DEL, escaped rather than removed, and
# backslash doubled so the escaping is reversible.
#
# A tag is arbitrary bytes somebody else wrote, and three of these break the
# format in different ways: \t invents a column, \n invents a row, and \r
# invents a row only on the way back *in*, because Python's text mode treats a
# lone \r as a line ending.
#
# **Escaped and not flattened to a space**, which is the second lesson. A
# trailing \r in a genre tag rendered as " " reads in a diff as "Blues" against
# "Blues " — a difference nobody can see, in a tool whose entire job is showing
# differences. Real trailing whitespace exists in tags too, so the two must not
# render alike.
CONTROL = {c: "\\x%02x" % c for c in range(0x20)}
CONTROL[0x7F] = "\\x7f"
CONTROL[ord("\\")] = "\\\\"


def render(name, value):
	if value is None:
		return NULL
	# REAL, so its repr varies with how it was computed; ffprobe reports
	# fractional seconds and TagLib whole ones. Three places is finer than any
	# real difference and coarse enough not to invent one.
	if name == "duration":
		return "%.3f" % float(value)
	return str(value).translate(CONTROL)


def dump(db_path, out):
	# Read-only, so a mistyped argument cannot write to a live library.
	uri = "file:%s?mode=ro" % db_path.replace("?", "%3f").replace("#", "%23")
	db = sqlite3.connect(uri, uri=True)
	out.write("\t".join(COLUMNS) + "\n")
	sql = "SELECT %s FROM songs ORDER BY path" % ", ".join(COLUMNS)
	for row in db.execute(sql):
		out.write("\t".join(render(c, v) for c, v in zip(COLUMNS, row)) + "\n")
	db.close()


def load(path):
	# newline="\n" and not the default: universal newline mode would split a row
	# at a bare \r, and dumps taken before render() flattened those still have
	# them. Note "" does not do this — it keeps universal newline *detection*
	# and only skips the translation.
	with open(path, encoding="utf-8", newline="\n") as f:
		header = f.readline().rstrip("\n").split("\t")
		rows = {}
		for n, line in enumerate(f, start=2):
			fields = line.rstrip("\n").split("\t")
			# Named here rather than left to fail as a KeyError somewhere in
			# compare(), which says nothing about which line was malformed.
			if len(fields) != len(header):
				raise ValueError(
					"%s line %d: %d fields, expected %d"
					% (path, n, len(fields), len(header)))
			rows[fields[0]] = dict(zip(header, fields))
	return header, rows


def compare(before_path, after_path, limit=5):
	"""Per-column mismatch counts, which is what a 27k-line diff cannot show.

	limit is how many examples to print per column; 0 prints every one.
	"""
	header_a, before = load(before_path)
	header_b, after = load(after_path)
	if header_a != header_b:
		print("columns differ between the two dumps; regenerate both")
		return 1

	gone = sorted(set(before) - set(after))
	new = sorted(set(after) - set(before))
	shared = sorted(set(before) & set(after))

	print("%d rows before, %d after, %d in both" % (len(before), len(after), len(shared)))
	shown = len(before) if limit == 0 else limit * 2
	for label, paths in (("only in before", gone), ("only in after", new)):
		if paths:
			print("\n%d %s:" % (len(paths), label))
			for p in paths[:shown]:
				print("  " + p)
			if len(paths) > shown:
				print("  ... and %d more (pass 0 to see every one)"
				      % (len(paths) - shown))

	worst = 0
	for col in header_a[1:]:
		diffs = [p for p in shared if before[p][col] != after[p][col]]
		if not diffs:
			continue
		worst = max(worst, len(diffs))
		print("\n%s: %d of %d differ" % (col, len(diffs), len(shared)))
		for p in (diffs if limit == 0 else diffs[:limit]):
			print("  %s\n    before %s\n    after  %s"
			      % (p, before[p][col], after[p][col]))
		if limit and len(diffs) > limit:
			print("  ... and %d more (pass 0 to see every one)"
			      % (len(diffs) - limit))

	if not gone and not new and worst == 0:
		print("\nidentical: the change cost no accuracy on this library")
		return 0
	return 1


def main():
	if len(sys.argv) in (4, 5) and sys.argv[1] == "--compare":
		limit = int(sys.argv[4]) if len(sys.argv) == 5 else 5
		return compare(sys.argv[2], sys.argv[3], limit)
	if len(sys.argv) == 2:
		dump(sys.argv[1], sys.stdout)
		return 0
	print(__doc__.strip(), file=sys.stderr)
	return 2


if __name__ == "__main__":
	sys.exit(main())

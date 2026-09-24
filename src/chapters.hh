#pragma once

#include <string>
#include <string_view>
#include <vector>

// Chapter markers for a single-file concert or film.
//
// The list lives in a sidecar `<stem>.chapters.txt` beside the video, in the
// format mp4chaps(1) exports - `HH:MM:SS.mmm Title`, one marker a line.  That
// is not an invention: `mp4chaps --export` writes exactly this file and
// `--import` reads it back, so the markers can be moved into the container by
// someone who wants them there.  Chapters are deliberately *not* written into
// the container by gaindrive: MP4 chapters are a track inside the file, so
// every save would be an `ffmpeg -c copy` rewrite of every byte of a
// multi-gigabyte concert, needing that much free space as well as the time.
//
// Pure string work: no database, no filesystem, no ffmpeg.  Same reasoning as
// videoname.hh - the rules get tuned against real files people typed by hand,
// and they are worth being able to test by running --chapters-test over one.
//
// The parse is deliberately liberal, because the other thing that lands in
// this file is a block pasted out of a YouTube description:
//
//     0:00 Shine On You Crazy Diamond
//     13:35 - Learning to Fly
//     90:00 Encore
//
// What it will *not* do is grow a second syntax.  The OGM/mkvmerge form
// (`CHAPTER01=`/`CHAPTER01NAME=`) is out of scope by decision, not by
// oversight; "liberal" is not an invitation to add it without a rule.
struct Chapter
	{
	double      start = 0;   // seconds from the start of the file
	std::string name;
	};

// `skipped` counts lines that held no timestamp.  It exists so a caller can
// tell a hand-written file that yielded nothing from no file at all - which
// matters here, because an *empty* sidecar is the tombstone meaning "this film
// has no chapters" and must not be confused with a file full of typos.
struct ChapterParse
	{
	std::vector<Chapter> chapters;
	int                  skipped = 0;
	};

// Control characters are dropped rather than escaped, on both the read and the
// write path, because the format is line-based and has no escape syntax: a
// newline inside a title would silently become another marker.  This is not
// the same job as utf8_clean(), which validates the *encoding* and passes
// control characters straight through - both are needed, and the endpoint does
// the other one.
std::string chapter_clean_name(std::string_view s);

// Consumes a timestamp anchored at the start of `s`, returning how many
// characters it took, or 0 when there is no timestamp there.
//
// Two rules in here are decisions rather than arithmetic:
//
//  * The fraction is a **decimal fraction, not a count of milliseconds**.
//    `00:00.5` is half a second, not five thousandths of one.
//  * Seconds are always < 60, but **minutes may exceed 59 in the two-field
//    form**: `90:00 Encore` is ordinary in a hand-typed list, while `13:99:00`
//    is a typo.  That single asymmetry is what separates the two cases.
//
// The timestamp must be terminated by whitespace or end of line, which is what
// stops `13:35abc` from parsing as a time followed by a title.
size_t chapter_scan_time(std::string_view s, double& out);

// `13:35 - Learning to Fly` is as common as the bare form, and without this
// every title in half the files people paste in begins with "- ".  Only a
// separator followed by whitespace is taken, so a title that genuinely starts
// with a dash keeps it.
std::string_view chapter_strip_separator(std::string_view t);

// Sorted by start, stably.  Equal starts keep the order the file gave them
// rather than being merged: dropping one is data loss on a file somebody
// typed, and every consumer's lookup ("the last chapter starting at or before
// t") is well defined either way.
ChapterParse parse_chapters(std::string_view text);

// llround, not truncation: 1.001 held as a double is 1.000999..., so a
// truncating round-trip walks every timestamp backwards one millisecond per
// save.  The hours field is not clamped to two digits -- %02lld is a minimum.
std::string format_chapter_time(double secs);

// Writes the file, in the order given -- parse_chapters() is what sorts, and
// the save path runs the text through it first, so this stays a pure
// formatter.  LF, never CRLF.
//
// An empty name is written as an empty line body rather than being filled in
// with "Chapter N": saving a file somebody hand-edited must not rewrite lines
// they deliberately left bare.  The API boundary does that filling, where it
// can number *after* the sort.
std::string format_chapters(const std::vector<Chapter>& chapters);

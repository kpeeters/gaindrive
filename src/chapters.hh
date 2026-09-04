#pragma once

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

// Chapter markers for a single-file concert or film.
//
// The list lives in a sidecar `<stem>.chapters.txt` beside the video, in the
// format mp4chaps(1) exports — `HH:MM:SS.mmm Title`, one marker a line.  That
// is not an invention: `mp4chaps --export` writes exactly this file and
// `--import` reads it back, so the markers can be moved into the container by
// someone who wants them there.  Chapters are deliberately *not* written into
// the container by gaindrive: MP4 chapters are a track inside the file, so
// every save would be an `ffmpeg -c copy` rewrite of every byte of a
// multi-gigabyte concert, needing that much free space as well as the time.
//
// Pure string work: no database, no filesystem, no ffmpeg.  Same reasoning as
// videoname.hh — the rules get tuned against real files people typed by hand,
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
// tell a hand-written file that yielded nothing from no file at all — which
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
// control characters straight through — both are needed, and the endpoint does
// the other one.
inline std::string chapter_clean_name(std::string_view s)
	{
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s)
		if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
	size_t b = out.find_first_not_of(" \t");
	if (b == std::string::npos) return std::string();
	size_t e = out.find_last_not_of(" \t");
	return out.substr(b, e - b + 1);
	}

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
inline size_t chapter_scan_time(std::string_view s, double& out)
	{
	size_t             i = 0;
	long long          fields[3];
	int                n = 0;

	while (true) {
		size_t start = i;
		while (i < s.size() && s[i] >= '0' && s[i] <= '9') i++;
		if (i == start || i - start > 9) return 0;
		long long v = 0;
		for (size_t k = start; k < i; k++) v = v * 10 + (s[k] - '0');
		fields[n++] = v;
		if (n == 3) break;
		if (i < s.size() && s[i] == ':') { i++; continue; }
		break;
		}
	if (n < 2) return 0;   // a bare number is a track index, not a time

	// A comma is accepted as the decimal point because that is what SRT uses
	// and this file is edited by the same people.
	double frac = 0;
	if (i < s.size() && (s[i] == '.' || s[i] == ',')) {
		size_t start = i + 1, j = start;
		while (j < s.size() && s[j] >= '0' && s[j] <= '9') j++;
		if (j > start) {
			double scale = 1;
			for (size_t k = start; k < j; k++) {
				scale *= 10;
				frac  += (s[k] - '0') / scale;
				}
			i = j;
			}
		}

	if (i < s.size() && s[i] != ' ' && s[i] != '\t') return 0;

	long long h = 0, m = 0, sec = 0;
	if (n == 2) { m = fields[0]; sec = fields[1]; }
	else        { h = fields[0]; m = fields[1]; sec = fields[2];
	              if (m >= 60) return 0; }
	if (sec >= 60) return 0;

	out = static_cast<double>(h) * 3600.0 + static_cast<double>(m) * 60.0
	    + static_cast<double>(sec) + frac;
	return i;
	}

// `13:35 - Learning to Fly` is as common as the bare form, and without this
// every title in half the files people paste in begins with "- ".  Only a
// separator followed by whitespace is taken, so a title that genuinely starts
// with a dash keeps it.
inline std::string_view chapter_strip_separator(std::string_view t)
	{
	static constexpr std::string_view seps[] = {
		"-", "|", "\xe2\x80\x93" /* en dash */, "\xe2\x80\x94" /* em dash */
		};
	for (std::string_view sep : seps) {
		if (t.size() <= sep.size() || t.substr(0, sep.size()) != sep) continue;
		char after = t[sep.size()];
		if (after != ' ' && after != '\t') continue;
		t.remove_prefix(sep.size());
		while (!t.empty() && (t.front() == ' ' || t.front() == '\t'))
			t.remove_prefix(1);
		return t;
		}
	return t;
	}

// Sorted by start, stably.  Equal starts keep the order the file gave them
// rather than being merged: dropping one is data loss on a file somebody
// typed, and every consumer's lookup ("the last chapter starting at or before
// t") is well defined either way.
inline ChapterParse parse_chapters(std::string_view text)
	{
	ChapterParse out;

	// A BOM is stripped once, at offset 0.  Without this exactly one chapter
	// -- the first -- fails to parse while every other line is fine, which
	// reads as a missing marker rather than as an encoding problem.  Notepad
	// writes one.
	if (text.size() >= 3 && text.compare(0, 3, "\xef\xbb\xbf") == 0)
		text.remove_prefix(3);

	size_t pos = 0;
	while (pos <= text.size()) {
		size_t nl   = text.find('\n', pos);
		size_t end  = (nl == std::string_view::npos) ? text.size() : nl;
		std::string_view line = text.substr(pos, end - pos);
		pos = end + 1;

		// CRLF only.  A lone-CR (classic Mac) file is not supported, and
		// saying so is the point: an invisible CR would otherwise reach a JSON
		// string, an XML attribute and back out into the file again.
		if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
		while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
			line.remove_prefix(1);

		if (line.empty()) continue;
		// mp4chaps has no comment convention, so this is our choice.
		if (line.front() == '#') continue;

		double start = 0;
		size_t used  = chapter_scan_time(line, start);
		if (used == 0) { out.skipped++; continue; }

		std::string_view rest = line.substr(used);
		while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
			rest.remove_prefix(1);

		// Everything after the whitespace run is the title, verbatim.  That
		// anchoring is what makes "13:35 Pt. 1: Shine On", "13:35 2 Minutes To
		// Midnight" and even a second timestamp inside a title all come out
		// right with no special cases -- and it is what makes escaping
		// unnecessary on the way back out.  Do not turn this into a search.
		Chapter c;
		c.start = start;
		c.name  = chapter_clean_name(chapter_strip_separator(rest));
		out.chapters.push_back(std::move(c));
		}

	std::stable_sort(out.chapters.begin(), out.chapters.end(),
	                 [](const Chapter& a, const Chapter& b) {
	                    return a.start < b.start;
	                    });
	return out;
	}

// llround, not truncation: 1.001 held as a double is 1.000999..., so a
// truncating round-trip walks every timestamp backwards one millisecond per
// save.  The hours field is not clamped to two digits -- %02lld is a minimum.
inline std::string format_chapter_time(double secs)
	{
	long long ms = std::llround(secs * 1000.0);
	if (ms < 0) ms = 0;
	long long h = ms / 3600000; ms %= 3600000;
	long long m = ms / 60000;   ms %= 60000;
	long long s = ms / 1000;    ms %= 1000;
	char buf[64];
	std::snprintf(buf, sizeof buf, "%02lld:%02lld:%02lld.%03lld", h, m, s, ms);
	return std::string(buf);
	}

// Writes the file, in the order given -- parse_chapters() is what sorts, and
// the save path runs the text through it first, so this stays a pure
// formatter.  LF, never CRLF.
//
// An empty name is written as an empty line body rather than being filled in
// with "Chapter N": saving a file somebody hand-edited must not rewrite lines
// they deliberately left bare.  The API boundary does that filling, where it
// can number *after* the sort.
inline std::string format_chapters(const std::vector<Chapter>& chapters)
	{
	std::string out;
	for (const auto& c : chapters) {
		out += format_chapter_time(c.start);
		out += ' ';
		out += chapter_clean_name(c.name);
		out += '\n';
		}
	return out;
	}

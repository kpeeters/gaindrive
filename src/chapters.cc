#include "chapters.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>

std::string chapter_clean_name(std::string_view s)
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

size_t chapter_scan_time(std::string_view s, double& out)
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

std::string_view chapter_strip_separator(std::string_view t)
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

ChapterParse parse_chapters(std::string_view text)
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

std::string format_chapter_time(double secs)
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

std::string format_chapters(const std::vector<Chapter>& chapters)
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

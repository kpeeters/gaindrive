#include "untrusted.hh"

#include <cctype>

std::string utf8_clean(std::string_view s, size_t max_bytes)
	{
	std::string out;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char c = s[i];
		size_t len = c < 0x80 ? 1
		           : (c & 0xE0) == 0xC0 ? 2
		           : (c & 0xF0) == 0xE0 ? 3
		           : (c & 0xF8) == 0xF0 ? 4 : 0;
		if (len == 0 || i + len > s.size()) { i++; continue; }
		bool ok = true;
		for (size_t k = 1; k < len; k++)
			if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
		if (!ok) { i++; continue; }
		if (out.size() + len > max_bytes) break;
		out.append(s.substr(i, len));
		i += len;
		}
	return out;
	}

std::string clean_name(std::string_view s)
	{
	std::string kept;
	kept.reserve(s.size());
	for (char ch : s) {
		auto c = static_cast<unsigned char>(ch);
		if (c < 0x20 || c == 0x7f) continue;
		kept.push_back(ch);
		}
	return utf8_clean(kept, MAX_NAME_BYTES);
	}

std::string clean_prose(std::string_view s, size_t max_bytes)
	{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = s[i];
		if (c == '\r') {
			if (i + 1 < s.size() && s[i + 1] == '\n') continue;  // CRLF: keep the LF
			out.push_back('\n');
			continue;
			}
		if (c == '\n' || c == '\t') { out.push_back(static_cast<char>(c)); continue; }
		if (c < 0x20 || c == 0x7f) continue;
		out.push_back(static_cast<char>(c));
		}
	return utf8_clean(out, max_bytes);
	}

std::string clean_url(std::string_view s, size_t max_bytes)
	{
	if (s.size() > max_bytes) return std::string();

	auto starts_with_ci = [&](std::string_view p) {
		if (s.size() < p.size()) return false;
		for (size_t i = 0; i < p.size(); ++i)
			if (std::tolower((unsigned char)s[i]) != p[i]) return false;
		return true;
		};
	if (!starts_with_ci("http://") && !starts_with_ci("https://"))
		return std::string();

	for (unsigned char c : s)
		if (c < 0x20 || c == 0x7f || c == ' ' ||
		    c == '<' || c == '>' || c == '"')
			return std::string();

	// Last rather than first: the checks above are the security ones and are
	// cheap, and this one only decides whether the bytes can be serialised.
	std::string out = utf8_clean(s, max_bytes);
	if (out.size() != s.size()) return std::string();
	return out;
	}

std::string clean_genre(std::string_view s)
	{
	if (s.find('|') != std::string_view::npos) return std::string();
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s)
		if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
	size_t b = out.find_first_not_of(" \t");
	if (b == std::string::npos) return std::string();
	size_t e = out.find_last_not_of(" \t");
	return utf8_clean(std::string_view(out).substr(b, e - b + 1), MAX_NAME_BYTES);
	}

bool is_uuid(std::string_view v)
	{
	if (v.size() != 36) return false;
	for (size_t i = 0; i < v.size(); ++i) {
		bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
		if (dash ? v[i] != '-' : !std::isxdigit((unsigned char)v[i]))
			return false;
		}
	return true;
	}

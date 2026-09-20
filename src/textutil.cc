#include "textutil.hh"

#include <algorithm>
#include <cctype>
#include <filesystem>

std::string log_safe(const std::string& s, size_t max_bytes)
	{
	std::string out;
	const size_t n = std::min(s.size(), max_bytes);
	out.reserve(n);
	for (size_t i = 0; i < n; ++i) {
		const unsigned char c = static_cast<unsigned char>(s[i]);
		out += (c < 0x20 || c == 0x7f) ? '.' : s[i];
		}
	if (s.size() > max_bytes) out += "...";
	return out;
	}

std::string url_encode(const std::string& s)
	{
	static const char hex[] = "0123456789ABCDEF";
	std::string out;
	for (unsigned char c : s) {
		if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
			out += c;
		else { out += '%'; out += hex[c >> 4]; out += hex[c & 0xf]; }
		}
	return out;
	}

std::string ext_of(const std::string& path)
	{
	std::string ext = std::filesystem::path(path).extension().string();
	if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext;
	}

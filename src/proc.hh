#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>

#include <unistd.h>

// Reads the tail of a captured stderr stream for logging.  A child process says
// nothing useful on success and prints the reason on the last line or two when
// it fails, so the tail is the part worth keeping.  Newlines are flattened so
// one failure produces one log line.
//
// This lives in a header of its own rather than in any one caller because three
// unrelated subsystems — the streamer, the transcode cache and the URL fetcher —
// each run a child with its stderr redirected to a std::tmpfile() and each wants
// the same answer when it exits non-zero.  A pipe is deliberately not used at
// any of the three: nothing drains one, so a chatty child blocks for ever once
// the pipe buffer fills.
inline std::string stderr_tail(FILE* f, size_t max_bytes = 4096)
	{
	if (!f) return {};
	std::fflush(f);
	if (std::fseek(f, 0, SEEK_END) != 0) return {};
	long end = std::ftell(f);
	if (end <= 0) return {};
	long start = end > static_cast<long>(max_bytes)
	           ? end - static_cast<long>(max_bytes) : 0;
	if (std::fseek(f, start, SEEK_SET) != 0) return {};
	std::string buf(static_cast<size_t>(end - start), '\0');
	buf.resize(std::fread(buf.data(), 1, buf.size(), f));
	for (auto& c : buf)
		if (c == '\n' || c == '\r') c = ' ';
	return buf;
	}

// Is this executable reachable?
//
// Here beside stderr_tail() for the same reason that one is: two unrelated
// subsystems want the identical small answer.  UrlFetcher drops a handler whose
// tool is not installed at startup rather than failing on every request, which
// is what makes "no yt-dlp on this machine" show up as a client that does not
// offer the row; --install-service refuses before writing anything when there is
// no systemctl to enable the unit with.
inline bool on_path(const std::string& prog)
	{
	if (prog.empty()) return false;
	if (prog.find('/') != std::string::npos)
		return ::access(prog.c_str(), X_OK) == 0;
	const char* path = std::getenv("PATH");
	if (!path) return false;
	std::string p(path);
	size_t start = 0;
	while (start <= p.size()) {
		size_t end = p.find(':', start);
		if (end == std::string::npos) end = p.size();
		std::string dir = p.substr(start, end - start);
		if (dir.empty()) dir = ".";
		if (::access((dir + "/" + prog).c_str(), X_OK) == 0) return true;
		start = end + 1;
		}
	return false;
	}

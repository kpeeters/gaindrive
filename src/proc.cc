#include "proc.hh"

#include <cstdlib>
#include <unistd.h>

std::string stderr_tail(FILE* f, size_t max_bytes)
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

bool on_path(const std::string& prog)
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

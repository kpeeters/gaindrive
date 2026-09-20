#pragma once

#include <cstdio>
#include <string>

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
std::string stderr_tail(FILE* f, size_t max_bytes = 4096);

// Is this executable reachable?
//
// Here beside stderr_tail() for the same reason that one is: two unrelated
// subsystems want the identical small answer.  UrlFetcher drops a handler whose
// tool is not installed at startup rather than failing on every request, which
// is what makes "no yt-dlp on this machine" show up as a client that does not
// offer the row; --install-service refuses before writing anything when there is
// no systemctl to enable the unit with.
bool on_path(const std::string& prog);

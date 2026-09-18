#pragma once

// Small string shaping used across the API surface: one for the log, one for
// building URLs, one for reading a codec off a filename.
//
// Split out of gaindrive.cc because each has callers in several of the route
// files and in the modules beside them, and a second copy of log_safe in
// particular is a copy that will drift.

#include <cstddef>
#include <string>

// A string safe to put in a log line: control characters replaced and the
// length bounded.
//
// Everything logged here — a path, a parameter, a username — arrives from the
// network, and it was written out raw. A CR or LF in any of it forges whole
// log lines, which matters more once something downstream reads this log to
// decide whom to block, and a long value simply makes the log useless.
std::string log_safe(const std::string& s, size_t max_bytes = 512);

std::string url_encode(const std::string& s);

// The lowercased extension of a path with no leading dot, which is the form
// songs.codec holds and therefore the form is_video_ext() and codec_to_mime()
// expect. Empty for a path with no extension.
std::string ext_of(const std::string& path);

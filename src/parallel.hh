#pragma once

#include <cstddef>
#include <functional>
#include <string>

// The scan's parallel primitive, and deliberately the only one.
//
// A fixed set of threads pulls indices off one atomic counter and runs `body`
// on each.  Dynamic rather than a static split, because the per-item cost
// varies by orders of magnitude - an ffprobe of a feature film against a TagLib
// read of a three-megabyte MP3 - so a fixed share leaves one thread working
// long after the others have finished.
//
// This is not a general thread pool and must not become one.  The threads are
// created for one loop and joined at the end of it, which is what makes the
// lifetime of everything `body` captures obvious: the callers in the scanner
// capture references to vectors on their own stack.  Thread creation is
// microseconds against a phase measured in minutes.
//
// Why parallel at all on a spinning disk, where the head is one: what this buys
// is queue depth, so the kernel's elevator and the drive's own queue can
// reorder by block address instead of servicing one seek at a time.  The useful
// window for that is small - see --scan-jobs, which exists to bound it.
//
// **An exception is captured, never allowed to escape a worker.**  One escaping
// a thread's top-level function calls std::terminate, which is a dead server
// rather than a lost file.  The first is kept and the remaining indices are
// skipped - exactly what a throw out of the sequential `for` loop this replaces
// did - and it is rethrown on the calling thread, so every existing try/catch
// around the scan still sees what it saw before.
void parallel_for(std::size_t n, int jobs,
                  const std::function<void(std::size_t)>& body);

// The lock every log line written from a worker goes through.
//
// std::cout is free of data races but promises nothing about ordering, and the
// lines this exists for are built from four or five separate `<<` calls - so
// without it two workers' output arrives spliced together mid-path.  That is
// worst for exactly the lines that matter, whose entire content is the path of
// a file something could not read.
void log_line(const std::string& text);

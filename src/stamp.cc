#include "stamp.hh"

#include <string>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <ctime>

#include <sys/stat.h>
#include <unistd.h>

// Is our stdout the journal?  Decided once, on first use.
//
// journald stamps every line it receives, so under a systemd unit a timestamp
// of ours is a second one on the same line -- twenty columns of duplication on
// every line of a scan, in a log that is read through `journalctl -u gaindrive`
// precisely because it does the timestamping.
//
// The signal is $JOURNAL_STREAM rather than anything naming systemd, and the
// difference is the point: it means "this stream is connected to the journal",
// which is the question being asked, while $INVOCATION_ID only means "this is a
// unit". A unit with StandardOutput=append:/var/log/gaindrive.log sets the
// second and not the first, and that file wants timestamps -- so keying on the
// stream gets every configuration right without knowing about any of them.
//
// **Its presence alone is not the test.** systemd.exec(5) documents the value
// as the device and inode of that stream, and it is inherited by children:
// gaindrive spawns ffmpeg and yt-dlp, and anything started in turn from a unit
// whose own output went elsewhere would read a value describing somebody else's
// stream. Comparing it against fstat() of our own descriptor is what makes the
// answer about this process.
//
// stdout only, though three log lines go to stderr. Suppressing on the strength
// of stderr could strip the timestamps from a stdout redirected to a file, and
// a missing timestamp in a file cannot be recovered afterwards while a doubled
// one in the journal is merely noise.
static bool stdout_is_journal()
	{
	static const bool answer = []() {
		const char* js = std::getenv("JOURNAL_STREAM");
		if (!js || !*js) return false;
		char*              end = nullptr;
		unsigned long long dev = std::strtoull(js, &end, 10);
		if (!end || *end != ':') return false;
		unsigned long long ino = std::strtoull(end + 1, &end, 10);
		if (!end || *end != '\0') return false;
		struct stat st{};
		if (::fstat(STDOUT_FILENO, &st) != 0) return false;
		return static_cast<unsigned long long>(st.st_dev) == dev
		    && static_cast<unsigned long long>(st.st_ino) == ino;
		}();
	return answer;
	}

std::string stamp(std::string label)
	{
	// On the journal an unlabelled line gets no prefix at all, rather than the
	// bare label column: that column is sixteen spaces wide, and leading every
	// message with it would trade a duplicated timestamp for an indent. The
	// column survives for a labelled line, which is what keeps the addresses in
	// the access log aligned with each other -- the only reason it exists.
	if (stdout_is_journal() && label.empty()) return {};

	std::ostringstream ss;
	if (!stdout_is_journal()) {
		std::time_t t = std::time(nullptr);
		std::tm tm = *std::localtime(&t);
		ss << std::put_time(&tm, "%F %T") << " ";
		}
	ss << std::right << std::setw(15) << label << " ";
	return ss.str();
	}

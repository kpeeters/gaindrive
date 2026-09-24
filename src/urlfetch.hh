#pragma once

#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <vector>

// Fetching media from a URL by running somebody else's tool.
//
// This knows how to match a URL against a configured table, expand an argv
// template and run the result, and nothing else: no database, no MediaStore, no
// HTTP, no job queue.  Same shape as Tmdb and VideoArt, and for the same
// reason - the table is what an operator tunes against the sites they actually
// use, and the whole class is what a different tool would replace.
// --url-fetch-test exercises it standalone.
//
// **A URL matching no handler is refused, and that refusal is the security
// boundary.**  There is no fallback handler and there must not be one: without
// the table, a pasted URL is an outbound request from inside the network to
// anywhere the server can reach, issued on the credentials of any account
// allowed to upload.  Two rules keep that boundary honest and both are easy to
// undo by accident:
//
//   * the pattern is matched **whole** (std::regex_match, never regex_search).
//     A pattern written as a bare site name and searched for would be satisfied
//     by file:///etc/shadow#youtube.com.
//   * the scheme is checked **before** the table is consulted, so a pattern
//     cannot admit anything but http and https however loosely it is written.
//
// There is no shell anywhere in this path - reproc takes an argv vector - so
// nothing about a URL needs quoting or escaping.  The one thing that does need
// care is a URL beginning with a dash, which a tool reads as an option; the
// templates end with a literal "--" before %URL% for that.
struct UrlHandler
	{
	std::string              name;         // shown to the user
	std::string              pattern;      // as written in the config
	std::vector<std::string> audio_argv;   // empty: this handler refuses audio
	std::vector<std::string> video_argv;   // empty: this handler refuses video
	std::regex               re;           // compiled from pattern at startup
	};

// True for an http or https URL and nothing else.  Free because both the
// endpoint (which reports the scheme separately, so the message names the real
// problem) and UrlFetcher::match() have to agree about it.
bool urlfetch_http_url(const std::string& url);

// The percentage this output line reports, or nothing when it carries none - in
// which case the caller **keeps the value it had**.  A tool's post-processing
// lines ("[ExtractAudio] Destination: …") carry no percentage, and resetting to
// zero for them would snap the bar back for the slowest visible phase of a
// fetch.
//
// Free, and declared here, so the rule can be exercised against canned tool
// output with no child process - the same bargain tmdb_pick() strikes.
std::optional<int> urlfetch_progress(const std::string& line);

// An argv template with %URL% and %DIR% substituted.  Whole elements only: a
// placeholder is never a substring of a larger argument, which is what makes
// the substitution unambiguous and keeps every value a single argv element.
std::vector<std::string> urlfetch_expand(const std::vector<std::string>& tmpl,
                                          const std::string& url,
                                          const std::string& dir);

class UrlFetcher
	{
	public:
		// nullopt means the built-in table.  An explicitly empty vector disables
		// the feature; a plain vector could not express both, and the difference
		// between "the operator said nothing" and "the operator said no" is the
		// difference between a working default and a silent one.
		//
		// timeout_s bounds a single fetch.  Generous, because a film over a slow
		// link legitimately takes an hour, but bounded, because one worker runs
		// the queue and a tool waiting on a throttled host would otherwise stop
		// it for ever.
		explicit UrlFetcher(
			const std::optional<std::vector<UrlHandler>>& handlers,
			int timeout_s = 2 * 60 * 60);
		~UrlFetcher();

		// The built-in table: one yt-dlp entry.  Public so --url-fetch-test and
		// the documentation can print exactly what will run.
		static std::vector<UrlHandler> default_handlers();

		// False when the table is empty, every pattern failed to compile or
		// every tool is missing from PATH - in which case nothing here does
		// anything at all.  Callers check this rather than discovering it as a
		// failure on every request, and it is what getUrlHandlers reports so a
		// client can leave the row undrawn.
		bool configured() const { return !handlers_.empty(); }

		// What a client is told about the table.  Deliberately not the pattern
		// and not the argv: an argv can hold --cookies, a proxy credential or an
		// API key, and the pattern is operator configuration rather than
		// anything a client can act on.
		struct Capability { std::string name; bool audio = false, video = false; };
		std::vector<Capability> capabilities() const;

		// The handler for this URL, or nullptr.  Never throws: every pattern was
		// compiled at construction and a bad one was dropped there.
		const UrlHandler* match(const std::string& url) const;

		struct Result
			{
			bool        ok    = false;
			int         files = 0;    // regular files left under dir
			std::string error;        // empty when ok
			};

		// Runs one fetch to completion.  `progress` is called for every line the
		// tool writes, with the running percentage and the raw line; it runs on
		// the calling thread, so the caller owns any locking and any sanitising
		// of what it stores - a progress line names the file being written, and
		// that is an absolute path.
		//
		// `job_id` is the name cancel() uses.  Blocking and slow by nature: this
		// must never be called from an HTTP thread.
		Result run(const UrlHandler& h, bool audio, const std::string& url,
		           const std::filesystem::path& dir, const std::string& job_id,
		           const std::function<void(int, const std::string&)>& progress);

		// Kills the child of a running job.  Safe to call from another thread; a
		// no-op when that job is not the one currently running.
		bool cancel(const std::string& job_id);

		// Kills whatever is running, whatever it is.  For shutdown: without it
		// the worker's join waits out the current fetch, and a fetch is allowed
		// to take hours.
		bool cancel_any();

	private:
		// reproc is deliberately kept out of this header, as videoart.hh keeps
		// it out of its own: a child-process detail should not reach every
		// translation unit that only wants the table.
		struct Child;

		std::vector<UrlHandler> handlers_;
		int                     timeout_s_;

		// Guards the running child only.  run() registers its stack-allocated
		// Child here for the duration and clears it before returning, so cancel()
		// can never name a destroyed process.
		mutable std::mutex mu_;
		Child*             child_ = nullptr;
		std::string        child_id_;
	};

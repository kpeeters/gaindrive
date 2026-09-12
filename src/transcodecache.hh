#pragma once

#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

// On-disk cache of transcoded audio.
//
// The point of materialising a transcode rather than piping it is that ffmpeg
// cannot seek backwards in a pipe, so a piped MP3 has no XING header and a
// piped FLAC/Ogg has no seektable.  Writing to a real file gives correct
// headers, a known Content-Length, and byte-range support — which is what makes
// a transcoded stream seekable for third-party clients and resumable for
// offline downloads.  See CLAUDE.md for the history of the piped-output bugs.
//
// The directory *is* the index: entry names encode everything needed to decide
// whether a file is still valid, so there is no sidecar state to keep in sync
// or repair after a crash.
class TranscodeCache
	{
	public:
		// A cached file, held open for as long as a request is serving it.
		// Destroying the Entry releases the in-use count, which is what stops
		// prune() from deleting a file out from under a response.
		class Entry
			{
			public:
				Entry(TranscodeCache& owner, std::string key,
				      std::filesystem::path path, int64_t size, bool hit);
				~Entry();
				Entry(const Entry&)            = delete;
				Entry& operator=(const Entry&) = delete;

				const std::filesystem::path& path() const { return path_; }
				int64_t                      size() const { return size_; }
				// False when this request had to run ffmpeg to produce it.
				bool                         hit()  const { return hit_; }

			private:
				TranscodeCache&       owner_;
				std::string           key_;
				std::filesystem::path path_;
				int64_t               size_;
				bool                  hit_;
			};

		// Token a caller puts in argv where the output path belongs;
		// get_or_build() substitutes the real cache path for it.  The \x01
		// sentinels make a collision with a real filename impossible.
		//
		// Written as two adjacent literals deliberately: in "\x01cache-out"
		// the hex escape consumes *every* following hex digit, and c and a
		// qualify — so it reads as \x01cac followed by "he-out", which is out
		// of range for a char and not the string it appears to be.  Splitting
		// the literal terminates the escape at the quote.
		static constexpr const char* OUT_PLACEHOLDER = "\x01" "cache-out\x01";

		// cap_bytes <= 0 or an unusable directory disables the cache: every
		// get_or_build() then returns nothing and callers fall back to
		// streaming.  There is deliberately no separate "enabled" flag to test.
		TranscodeCache(const std::filesystem::path& dir, int64_t cap_bytes,
		               int jobs);

		// Returns the cached file for `key`, transcoding it with `argv` first if
		// it is not already there.  Blocks for the duration of the transcode.
		// Empty when the cache is disabled or the transcode failed — never fatal,
		// the caller streams instead.
		//
		// `out_placeholder` is the token in argv to replace with the real output
		// path; the caller builds argv without knowing the cache's naming.
		std::shared_ptr<const Entry> get_or_build(
			const std::string& key, const std::string& ext,
			const std::vector<std::string>& argv,
			const std::string& out_placeholder);

		// The entry for `key` if it is already on disk, and nothing else: this
		// never runs ffmpeg and never waits for a build somebody else started.
		// Empty means "not there (yet)", which is the caller's cue to serve this
		// one request another way — see serve_video()'s remux tier.
		std::shared_ptr<const Entry> get_if_present(const std::string& key,
		                                            const std::string& ext);

		// Builds the entry for `key` on a detached thread and returns at once, so
		// no request thread waits on it.  True when the entry may be expected to
		// appear — a build was started, or one was already running; false when the
		// cache is disabled, the file is already there, or no slot was free.
		//
		// The answer is for the log rather than for control flow: the caller's
		// fallback is to stream this request either way.
		bool build_in_background(const std::string& key, const std::string& ext,
		                         const std::vector<std::string>& argv,
		                         const std::string& out_placeholder);

	private:
		void release(const std::string& key);
		// Caller holds mu_.  Opens `final` as an Entry with the in-use count
		// taken and the LRU mtime touched, or {} when it is not a usable file.
		// One definition, so the touch and the refcount cannot drift between the
		// two lookups that need them.
		std::shared_ptr<const Entry> open_locked(
			const std::string& key, const std::filesystem::path& final, bool hit);
		// The body both build paths share, run with a slot already claimed and
		// `key` already in building_.  Releases both.
		std::shared_ptr<const Entry> run_build(
			const std::string& key, const std::filesystem::path& final,
			const std::filesystem::path& part,
			const std::vector<std::string>& argv,
			const std::string& out_placeholder, bool background);
		// Deletes least-recently-used entries until the total is comfortably
		// under the cap.  Called only after a successful build.
		void prune();
		bool run_ffmpeg(const std::vector<std::string>& argv,
		                const std::filesystem::path& part);

		std::filesystem::path dir_;
		int64_t               cap_bytes_;
		int                   jobs_;
		bool                  enabled_ = false;

		std::mutex                             mu_;
		std::condition_variable                cv_;
		std::set<std::string>                  building_;
		std::unordered_map<std::string, int>   in_use_;
		int                                    running_ = 0;
		// Background builds inside running_, capped separately.  A burst of first
		// plays must not fill every --transcode-jobs slot and push the next
		// *audio* transcode onto the piped path, which is the one with no XING
		// header and no seektable — precisely what this cache exists to avoid.
		// Nobody is waiting on a background build, so losing the race costs it
		// nothing; an audio request losing it costs seekability.
		int                                    bg_running_ = 0;
	};

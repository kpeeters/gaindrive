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

	private:
		void release(const std::string& key);
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
	};

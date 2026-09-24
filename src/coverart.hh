#pragma once

#include <condition_variable>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

class MediaStore;

// Scaled cover art, produced once and then read back.
//
// getCoverArt is asked for a pixel size by every client gaindrive has, and
// each of those requests used to fork ffmpeg and decode the full-size source.
// This class is what turns that into one decode per (image, size) in the
// lifetime of the library: it puts a memory cache in front of the
// cover_thumbs table, and imagescale in front of both.
//
// It knows MediaStore and it knows how to run ffmpeg, but it knows nothing
// about HTTP - the handler resolves an id into a CoverSource and then asks
// only for bytes.
class CoverArtCache
	{
	public:
		// Where the original image is, and how to tell when it has changed.
		//
		// One key space serves all three kinds of art, which works because a
		// directory and a file cannot share a path: a cover or extra image is
		// keyed on its own path, a video_art blob on the *media* file's path
		// (already what cover_path holds for one), and an artist portrait on
		// the artist folder's path.
		struct Source
			{
			enum class Kind { File, Blob };
			Kind        kind = Kind::File;
			std::string key;        // stored form, "<root>/<rest>"
			std::string abs_path;   // File: what to open
			std::string bytes;      // Blob: the original image
			int64_t     stamp = 0;  // mtime, file_modified, or fetched_at
			};

		struct Result
			{
			std::string mime;
			std::string bytes;
			int         width  = 0;
			int         height = 0;
			};

		CoverArtCache(MediaStore& store, std::size_t mem_bytes = 32u * 1024 * 1024,
		              int decode_jobs = 0);

		// Rounds a client's `size` up to the next rung of a fixed ladder, or
		// returns 0 for "do not scale, serve the source".
		//
		// **This is a bound on the key space, not a nicety.** `size` arrives
		// from the client unvalidated and cover_thumbs has no eviction policy,
		// so without quantising, one authenticated account could write a row
		// per pixel value for every cover in the library and fill the disk.
		// The ladder holds every size gaindrive's own clients ask for, so none
		// of them is affected; a third-party client asking 137 gets 144 and
		// downsamples it, and the Subsonic API has never promised that `size`
		// is the exact dimension returned.
		static int ladder_size(int requested);

		// The image at `size`, from memory, then the DB, then a decode.
		//
		// nullopt means "serve the original": either the source is already
		// smaller than the target, or nothing could decode it. Both are
		// recorded, so neither is retried on the next request.
		std::optional<Result> scaled(const Source& src, int size);

		// Drops every cached size for one key. The DB rows are dropped by
		// MediaStore; this clears the memory tier, which nothing else can see.
		void invalidate(const std::string& key);

		// Snapshot for getServerStatus.
		struct Stats
			{
			std::size_t mem_used, mem_cap, entries;
			int         building, jobs;
			};
		Stats stats();

	private:
		struct Entry
			{
			std::string ckey;      // key \x1f size \x1f stamp
			Result      val;
			};

		// A build in progress. The second and later arrivals for a cold key
		// wait on this rather than decoding the same JPEG again.
		struct Pending
			{
			std::condition_variable  cv;
			bool                     done = false;
			bool                     none = false;   // "serve the original"
			Result                   val;
			};

		std::optional<Result> mem_get(const std::string& ckey);
		void                  mem_put(const std::string& ckey, const Result& v);
		std::optional<Result> build(const Source& src, int size);

		MediaStore& store_;

		std::mutex                                                   mem_mu_;
		std::list<Entry>                                             lru_;
		std::unordered_map<std::string, std::list<Entry>::iterator>  index_;
		std::size_t                                                  mem_used_ = 0;
		std::size_t                                                  mem_cap_;

		std::mutex                                                   build_mu_;
		std::condition_variable                                      slot_cv_;
		std::unordered_map<std::string, std::shared_ptr<Pending>>     building_;
		int                                                          running_ = 0;
		int                                                          jobs_;
	};

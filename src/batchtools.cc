#include "batchtools.hh"
#include "textutil.hh"
#include "stamp.hh"
#include "chapters.hh"
#include "jsonread.hh"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <iostream>
#include <map>
#include <random>
#include <vector>

#include <nlohmann/json.hpp>
#include <openssl/rand.h>

#include <archive.h>
#include <archive_entry.h>

#include <taglib/fileref.h>
#include <taglib/tag.h>
#include <tpropertymap.h>

// What one uploaded archive may expand to. An archive's compressed size bounds
// nothing — the ratio is the attack — so the extraction has to carry its own
// limit or a few megabytes fills the uploads volume, which is a filesystem
// other people's music is also on.
static constexpr uint64_t MAX_ARCHIVE_BYTES   = 8ull * 1024 * 1024 * 1024;
static constexpr int      MAX_ARCHIVE_ENTRIES = 20000;

// A v4 UUID, from the CSPRNG.
//
// It was mt19937_64 seeded from a single 32-bit random_device draw, which is
// trivially predictable. Neither of the two things it names is a capability
// today — an upload batch directory sits under a path already scoped by the
// authenticated username, and a fetch job id is looked up within the caller's
// own list — so this is not a fix for a live hole. It is that both are ids
// handed back to a client, which is exactly the shape of thing that later
// grows into a credential, and OpenSSL is already linked.
std::string make_uuid()
   {
   uint64_t a = 0, b = 0;
   uint8_t  raw[16];
   if (RAND_bytes(raw, sizeof(raw)) != 1) {
      // The CSPRNG failing is not survivable by carrying on with whatever was
      // on the stack, which is what ignoring the return value amounts to.
      std::cout << stamp() << "RAND_bytes failed generating a UUID" << std::endl;
      throw std::runtime_error("no entropy available");
      }
   std::memcpy(&a, raw,     8);
   std::memcpy(&b, raw + 8, 8);
   // Set UUID v4 version and variant bits.
   a = (a & 0xFFFFFFFFFFFF0FFFull) | 0x0000000000004000ull;
   b = (b & 0x3FFFFFFFFFFFFFFFull) | 0x8000000000000000ull;
   std::ostringstream ss;
   ss << std::hex << std::setfill('0')
      << std::setw(8)  << (uint32_t)(a >> 32)        << '-'
      << std::setw(4)  << (uint32_t)((a >> 16) & 0xFFFF) << '-'
      << std::setw(4)  << (uint32_t)(a & 0xFFFF)     << '-'
      << std::setw(4)  << (uint32_t)(b >> 48)         << '-'
      << std::setw(12) << (b & 0x0000FFFFFFFFFFFFull);
   return ss.str();
   }

// Extract a zip/tar/tar.gz/tgz archive into dest_dir.
// Entry paths are sanitised: absolute components and ".." are stripped so
// no file can escape dest_dir. Returns the number of regular files written,
// -1 if the archive could not be opened, or -2 if every entry failed to be
// written. That last one is its own answer because nothing else would ever be
// told: an extraction that wrote not one byte otherwise returns 0, which the
// caller reports as a success carrying a file count of zero -- the shape the
// symlink bug below wore for as long as it lasted, visible only in this log.
// libarchive's error string can embed an entry's own name, and an entry name
// is attacker bytes that may hold newlines — a forged log line, in the log a
// host-level blocker reads. NULL when there is no message.
static std::string archive_err(struct archive* a)
   {
   const char* e = archive_error_string(a);
   return log_safe(e ? e : "(none)");
   }

int extract_archive_to_dir(const std::filesystem::path& archive_file,
                            const std::filesystem::path& dest_dir)
   {
   namespace fs = std::filesystem;

   // The destination with its own symlinks resolved, because SECURE_SYMLINKS
   // below refuses to write *through* one -- and libarchive walks the whole
   // pathname it is handed, starting at '/', so it is our own ancestors it
   // trips over long before it reaches anything the archive supplied. An
   // uploads root spelled through a symlink is a supported configuration and
   // deliberately so: abs_path() in main.cc leaves a root's symlinks alone on
   // purpose, and the MediaStore ctor says a root that is itself a symlink
   // still works. With one, every entry of every upload failed with "Cannot
   // extract through symlink" while the reply went on saying ok.
   //
   // Resolving the prefix gives up nothing. What the flag exists to guard is
   // the part below it, which comes out of the archive, and that is still
   // walked exactly as before. weakly_canonical rather than canonical to match
   // every other canonicalisation here, and because it needs no existence
   // guarantee; falling back on error leaves the previous behaviour.
   std::error_code base_ec;
   fs::path base = fs::weakly_canonical(dest_dir, base_ec);
   if (base_ec) base = dest_dir;

   struct archive* a = archive_read_new();
   archive_read_support_format_all(a);
   archive_read_support_filter_all(a);

   struct archive* wd = archive_write_disk_new();
   // NOABSOLUTEPATHS is still not set, and for the original reason: it would
   // reject every entry, because we set an absolute destination path
   // ourselves.
   //
   // SECURE_SYMLINKS **is** now set, and the note that used to be here — that
   // path traversal is prevented entirely by the sanitisation loop below — was
   // true of an entry's *pathname* and false of a symlink's *target*, which
   // the loop never looked at. An archive holding
   //
   //     esc  -> /etc        (a symlink entry)
   //     esc/passwd          (an ordinary file entry)
   //
   // has both pathnames pass the loop unchanged, since neither contains "..",
   // and libarchive then follows the link it has just created. The objection
   // to the flag was that it refuses to extract through a host symlink that
   // the music root may contain, and the answer given here was that the
   // destination is a batch directory gaindrive created itself. That is true
   // of the components *below* the destination and silently assumed the ones
   // above it were not being looked at. They are, and `base` above is what
   // answers for them.
   //
   // The filetype filter below makes the flag belt-and-braces rather than the
   // only defence, since a link that is never written cannot be followed.
   archive_write_disk_set_options(wd, ARCHIVE_EXTRACT_TIME
                                    | ARCHIVE_EXTRACT_SECURE_SYMLINKS
                                    | ARCHIVE_EXTRACT_SECURE_NODOTDOT);

   auto cleanup = [&]{
      archive_read_close(a);
      archive_read_free(a);
      archive_write_close(wd);
      archive_write_free(wd);
      };

   // From a file, not memory: the upload handler streams the body to disk
   // precisely so an archive is never held in RAM, and reading it back here
   // would undo that.
   int r = archive_read_open_filename(a, archive_file.c_str(), 64 * 1024);
   if (r != ARCHIVE_OK) {
      std::cout << stamp() << "extract: open failed (" << r << "): "
                << archive_err(a) << std::endl;
      cleanup();
      return -1;
      }

   int count = 0, skipped = 0, entries = 0, failed = 0;
   uint64_t written = 0;
   struct archive_entry* entry;
   int hr;
   while ((hr = archive_read_next_header(a, &entry)) == ARCHIVE_OK
          || hr == ARCHIVE_WARN) {
      if (hr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: header warn: "
                   << archive_err(a) << std::endl;

      const char* raw = archive_entry_pathname(entry);
      if (!raw) { skipped++; continue; }

      // Build a sanitised relative path by dropping any "..", ".", and "/" components.
      fs::path safe;
      for (const auto& part : fs::path(raw)) {
         auto s = part.string();
         if (s == ".." || s == "." || s == "/") continue;
         safe /= part;
         }
      if (safe.empty()) {
         std::cout << stamp() << "extract: skip (empty after sanitise): " << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }

      // Only ordinary files and the directories holding them. A symlink or a
      // hardlink entry names a *target*, which is a second path the
      // sanitisation above never sees — and following one is how an extraction
      // escapes a directory it cannot traverse out of. Nothing an uploader
      // legitimately sends is either.
      const auto ft = archive_entry_filetype(entry);
      if (ft != AE_IFREG && ft != AE_IFDIR) {
         std::cout << stamp() << "extract: skip (not a file or directory): "
                   << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }
      if (archive_entry_hardlink(entry) || archive_entry_symlink(entry)) {
         std::cout << stamp() << "extract: skip (link entry): " << log_safe(raw) << std::endl;
         skipped++;
         continue;
         }
      // `entries` counts everything written, directories included — counting
      // only files let a zip of a few hundred kilobytes create millions of
      // directories, which is inode exhaustion on a filesystem other people's
      // music is also on, with the byte cap never tripping because a
      // directory carries no data.
      if (entries >= MAX_ARCHIVE_ENTRIES) {
         std::cout << stamp() << "extract: entry limit reached, stopping"
                   << std::endl;
         break;
         }
      entries++;

      fs::path target = base / safe;
      archive_entry_set_pathname(entry, target.c_str());

      // ARCHIVE_WARN (-20) means partial success; still write the data.
      int wr = archive_write_header(wd, entry);
      if (wr < ARCHIVE_WARN) {
         std::cout << stamp() << "extract: write_header failed (" << wr << ") for "
                   << target << ": " << archive_err(wd) << std::endl;
         failed++;
         skipped++;
         continue;
         }
      if (wr == ARCHIVE_WARN)
         std::cout << stamp() << "extract: write_header warn for "
                   << target << ": " << archive_err(wd) << std::endl;

      // Bounded, because an archive's *compressed* size says nothing about
      // what it expands to: a few megabytes of zeros is gigabytes on disk, and
      // the uploads root is a real filesystem someone else's music is also on.
      const void* buf; size_t sz; la_int64_t off;
      bool truncated = false;
      while (archive_read_data_block(a, &buf, &sz, &off) == ARCHIVE_OK) {
         // The declared block offset counts against the budget as well as the
         // data: a sparse tar entry placing a few bytes at offset 2^50 makes
         // a file of that apparent length for almost nothing, and everything
         // downstream — songs.file_size, the transcode planner — believes the
         // fiction.
         if (written + sz > MAX_ARCHIVE_BYTES
             || off < 0
             || static_cast<uint64_t>(off) + sz > MAX_ARCHIVE_BYTES) {
            truncated = true; break;
            }
         archive_write_data_block(wd, buf, sz, off);
         written += sz;
         }
      if (truncated) {
         std::cout << stamp() << "extract: size limit reached at " << target
                   << ", stopping" << std::endl;
         archive_write_finish_entry(wd);
         skipped++;
         break;
         }
      if (archive_entry_filetype(entry) == AE_IFREG)
         count++;
      archive_write_finish_entry(wd);
      }

   if (hr != ARCHIVE_EOF)
      std::cout << stamp() << "extract: read_next_header stopped (r=" << hr << "): "
                << archive_err(a) << std::endl;

   std::cout << stamp() << "extract: done, files=" << count
             << " skipped=" << skipped << " failed=" << failed << std::endl;
   cleanup();
   return (count == 0 && failed > 0) ? -2 : count;
   }

// Make a string safe to use as a single directory name. Shared by the upload
// reorganiser, by moveAlbum and by the URL fetcher's name override, so a name
// typed by hand lands in exactly the directory the uploader would have created
// for the same tag.
//
// Dropping '/' is also the traversal guard: a name is one path component by
// construction, so no input can escape the folder it is being created in. Note
// ".." needs no case of its own: the leading-dot strip below annihilates it,
// along with "." and "...", and an empty result is refused by every caller that
// took a name from a person.
//
// Dropping the control characters is what keeps such a name out of two places
// it has no business reaching. A directory name is echoed into the log by the
// scanner and by the batch renamer, where an embedded newline forges a log
// line; and it is written into a tinyxml2 attribute by getFetchJobs, where a
// raw C0 byte is not well-formed XML and nothing escapes it. Neither mattered
// while every component came from yt-dlp or from TagLib; a typed one is the
// first arbitrary string here to become a path component.
// Case-insensitively, because a filesystem extension is not case-sensitive to
// a person: someone renaming a film to "The Third Man.MKV" has already typed
// the extension and must not be given a second one.
bool ends_with_ci(const std::string& s, const std::string& suffix)
	{
	if (suffix.size() > s.size()) return false;
	return std::equal(suffix.rbegin(), suffix.rend(), s.rbegin(),
	                   [](char a, char b) {
		return std::tolower(static_cast<unsigned char>(a))
		     == std::tolower(static_cast<unsigned char>(b));
		});
	}

std::string sanitise_component(const std::string& s)
   {
   std::string r;
   for (char c : s)
      if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f &&
          c != '/' && c != '\\' && c != ':' &&
          c != '*' && c != '?'  && c != '"' && c != '<' && c != '>')
         r += c;
   while (!r.empty() && (r.front() == ' ' || r.front() == '.'))
      r.erase(r.begin());
   while (!r.empty() && (r.back()  == ' ' || r.back()  == '.'))
      r.pop_back();
   return r;
   }

// Reorganise all audio files under batch_root into <artist>/<album>/ subdirs
// using embedded tag metadata. Non-audio siblings follow their audio files when
// all audio in a source dir maps to the same (artist, album) target. Empty
// directories left behind are pruned. Falls back to "Unknown Artist" /
// "Unknown Album" for files with no usable tags.
void reorganise_by_tags(const std::filesystem::path& batch_root)
   {
   namespace fs = std::filesystem;

   static const std::set<std::string> AUDIO_EXT = {
      ".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"};
   auto is_audio = [](const fs::path& p) {
      std::string ext = p.extension().string();
      std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
      return AUDIO_EXT.count(ext) > 0;
      };
   // A tag that sanitises away to nothing still has to land somewhere, so this
   // path substitutes a name. moveAlbum deliberately does not — there a name
   // was typed, and silently filing it under "Unknown" would hide the mistake.
   auto sanitise = [](const std::string& s) -> std::string {
      std::string r = sanitise_component(s);
      return r.empty() ? "Unknown" : r;
      };

   // Collect audio files that are NOT already in an Artist/Album subdir
   // (depth >= 2 from batch_root). Those are left in place; only flat or
   // single-level files need reorganising.
   struct AudioFile { fs::path path; std::string artist; std::string album; };
   std::vector<AudioFile> audio_files;
   std::error_code ec;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec)) {
      if (!e.is_regular_file() || !is_audio(e.path())) continue;
      // A zip built on macOS carries a "._Track01.mp3" beside every track.
      // TagLib reads nothing from a resource fork, so each would be filed
      // under Unknown Artist and moved into the library.
      if (is_hidden_name(e.path())) continue;
      auto rel   = e.path().lexically_relative(batch_root);
      int  depth = (int)std::distance(rel.begin(), rel.end()) - 1; // -1 for filename
      if (depth >= 2) continue;  // already in Artist/Album structure
      std::string artist = "Unknown Artist", album = "Unknown Album";
      TagLib::FileRef ref(e.path().c_str());
      if (!ref.isNull() && ref.tag()) {
         auto a = ref.tag()->artist().to8Bit(true);
         auto b = ref.tag()->album().to8Bit(true);
         if (!a.empty()) artist = a;
         if (!b.empty()) album  = b;
         }
      audio_files.push_back({e.path(), sanitise(artist), sanitise(album)});
      }

   if (audio_files.empty()) return;

   // Move each audio file into batch_root/<artist>/<album>/.
   // Track which source directories contributed to which (artist, album) targets
   // so sibling non-audio files (covers, .m3u, etc.) can follow.
   std::map<fs::path, std::set<std::pair<std::string, std::string>>> dir_targets;
   for (auto& af : audio_files) {
      fs::path target = batch_root / af.artist / af.album;
      fs::create_directories(target, ec);
      fs::rename(af.path, target / af.path.filename(), ec);
      if (ec)
         std::cout << stamp() << "reorganise: rename failed for "
                   << af.path << ": " << ec.message() << std::endl;
      else
         dir_targets[af.path.parent_path()].insert({af.artist, af.album});
      }

   // For source dirs that fed exactly one (artist, album) target, relocate
   // any remaining files (covers, lyrics, etc.) to the same destination.
   for (auto& [src, targets] : dir_targets) {
      if (targets.size() != 1) continue;
      auto& [artist, album] = *targets.begin();
      fs::path target = batch_root / artist / album;
      for (auto& e : fs::directory_iterator(src, ec))
         if (e.is_regular_file())
            fs::rename(e.path(), target / e.path().filename(), ec);
      }

   // Prune empty directories — collect them all first, then sort deepest-first
   // so children are removed before parents.
   std::vector<fs::path> dirs;
   for (auto& e : fs::recursive_directory_iterator(batch_root, ec))
      if (e.is_directory()) dirs.push_back(e.path());
   std::sort(dirs.begin(), dirs.end(),
             [](const fs::path& a, const fs::path& b){
                return b.string().size() < a.string().size();
                });
   for (auto& d : dirs)
      if (fs::is_empty(d, ec)) fs::remove(d, ec);
   }

// Splits a filename into the part a sibling shares and the part that names its
// kind, treating a sidecar's double extension as one suffix.
//
// std::filesystem splits on the *last* dot, so "Track.chapters.txt" has stem
// "Track.chapters" -- and that separates it from "Track.opus" in two different
// places downstream. reparent_loose_media() would file it under an album called
// "Track.chapters" while the media went to "Track"; and batch_merge_into()'s
// collision suffixing would produce "Track.chapters (2).txt", a name that no
// longer ends in CHAPTERS_SUFFIX, so it stops being a sidecar at all *and*
// starts appearing in getAlbumTexts as liner notes.
//
// Both were latent while nothing put a sidecar in a batch. A fetch that writes
// one from the tool's metadata makes them reachable.
static std::pair<std::string, std::string> split_sidecar_name(
	const std::filesystem::path& p)
	{
	const std::string name(p.filename().string());
	const std::string suffix(MediaStore::CHAPTERS_SUFFIX);
	if (name.size() > suffix.size()
	        && name.compare(name.size() - suffix.size(), suffix.size(),
	                        suffix) == 0)
		return { name.substr(0, name.size() - suffix.size()), suffix };
	return { p.stem().string(), p.extension().string() };
	}

// Turns a fetch tool's own metadata into the sidecar gaindrive indexes.
//
// yt-dlp writes <name>.info.json beside each download when asked, and its
// `chapters` array is the site's own parse of the timestamps the uploader
// wrote. That is the whole reason a fetched concert or DJ set can list its
// songs with nobody marking them up by hand.
//
// Keyed on the *file* rather than on which handler ran, so it serves an
// uploaded archive that happens to contain one, and a handler somebody
// configured with the flag themselves, without special-casing yt-dlp.
//
// **It must run before anything else in the pipeline renames a file**, which
// is why scan_batch() calls it first. std::filesystem splits on the last dot,
// so "Track.info.json" has stem "Track.info": reparent_loose_media() would
// file it under an album of that name and batch_merge_into() would suffix it
// "Track.info (2).json", either of which separates it from the media it
// describes. Running first also means the sidecar it writes travels the rest
// of the pipeline as an ordinary file, which split_sidecar_name() is what
// makes safe.
void convert_tool_sidecars(const std::filesystem::path& batch_root)
	{
	namespace fs = std::filesystem;
	static constexpr std::string_view INFO_SUFFIX = ".info.json";
	auto ends_with = [](const std::string& s, std::string_view suf) {
		return s.size() > suf.size()
		    && s.compare(s.size() - suf.size(), suf.size(), suf) == 0;
		};

	// Collected before anything is removed: mutating the tree under a
	// recursive_directory_iterator is undefined.
	std::error_code ec;
	std::vector<fs::path> found;
	for (auto& e : fs::recursive_directory_iterator(
	         batch_root, fs::directory_options::skip_permission_denied, ec)) {
		if (!e.is_regular_file()) continue;
		if (ends_with(e.path().filename().string(), INFO_SUFFIX))
			found.push_back(e.path());
		}

	for (const auto& info : found) {
		std::vector<Chapter> chapters;
		try {
			std::ifstream f(info, std::ios::binary);
			if (f) {
				std::string text((std::istreambuf_iterator<char>(f)),
				                  std::istreambuf_iterator<char>());
				// Through jsonread, never value(): this is third-party JSON
				// reached from fetch_worker()'s own thread, where an escaping
				// exception is std::terminate rather than a failed request --
				// and a video with no chapters sends "chapters": null, which
				// is exactly the shape value() throws on. A discarded parse is
				// not null either, and jsub() answers for that too.
				auto j = nlohmann::json::parse(text, nullptr, false);
				for (const auto& c : jsub(j, "chapters")) {
					if (chapters.size() >= MediaStore::MAX_CHAPTERS) break;
					const auto& st = jsub(c, "start_time");
					if (!st.is_number()) continue;
					Chapter ch;
					ch.start = st.get<double>();
					// Rejects a negative and, because the test is written the
					// positive way round, a NaN.
					if (!(ch.start >= 0)) continue;
					ch.name = chapter_clean_name(jstr(c, "title"));
					chapters.push_back(std::move(ch));
					}
				}
			}
		catch (const std::exception& ex) {
			std::cout << stamp() << "batch sidecars: cannot read " << info
			          << ": " << ex.what() << std::endl;
			}

		// Removed whether or not it yielded anything: it is the tool's
		// exhaust, and the library keeps the media and the sidecar.
		std::error_code rec;
		fs::remove(info, rec);

		// No chapters is not the same as *no* chapters: an empty sidecar is
		// the tombstone that overrules a container's own list, and a fetch has
		// no business asserting that on someone's behalf.
		if (chapters.empty()) continue;

		const std::string name = info.filename().string();
		fs::path target = info.parent_path()
		    / (name.substr(0, name.size() - INFO_SUFFIX.size())
		       + std::string(MediaStore::CHAPTERS_SUFFIX));

		// Never over a file that is already there: on a re-fetch that would
		// destroy markers somebody had corrected by hand.
		std::error_code xec;
		if (fs::exists(target, xec)) {
			std::cout << stamp() << "batch sidecars: keeping the existing "
			          << target.filename() << std::endl;
			continue;
			}

		std::ofstream out(target, std::ios::binary | std::ios::trunc);
		if (!out) {
			std::cout << stamp() << "batch sidecars: cannot write " << target
			          << std::endl;
			continue;
			}
		const std::string body = format_chapters(chapters);
		out.write(body.data(), static_cast<std::streamsize>(body.size()));
		out.close();
		if (!out) {
			std::cout << stamp() << "batch sidecars: write failed for "
			          << target << std::endl;
			fs::remove(target, rec);
			continue;
			}
		std::cout << stamp() << "batch sidecars: " << chapters.size()
		          << " chapter(s) -> " << target.filename() << std::endl;
		}
	}

BatchHold::BatchHold(const std::filesystem::path& batch)
	: marker_(batch_marker(batch))
	{
	// The marker sits beside the batch, so on a user's first fetch its
	// directory does not exist yet: fetch_worker() takes the hold before
	// create_directories(dest), which is the whole point of taking it
	// there. Without this the open would fail silently and the batch would
	// be unheld.
	std::error_code ec;
	std::filesystem::create_directories(marker_.parent_path(), ec);
	// Truncating open rather than a create-if-absent test: two producers
	// cannot share a batch uuid, so there is no one to race.
	std::ofstream(marker_).close();
	}

BatchHold::~BatchHold()
	{
	std::error_code ec;
	std::filesystem::remove(marker_, ec);
	}

void sweep_held_batches(const std::string& users_dir)
   {
   namespace fs = std::filesystem;
   std::error_code ec;
   for (auto& user : fs::directory_iterator(users_dir, ec)) {
      std::error_code uec;
      if (!user.is_directory(uec)) continue;
      for (auto& e : fs::directory_iterator(user.path(), uec)) {
         std::error_code bec;
         if (!e.is_directory(bec) || !batch_held(e.path())) continue;
         fs::remove_all(e.path(), bec);
         fs::remove(batch_marker(e.path()), bec);
         std::cout << stamp() << "Uploads: removed unfinished batch "
                   << user.path().filename().string() << "/"
                   << e.path().filename().string() << std::endl;
         }
      }
   }

// Pushes anything still loose in a batch down to <artist>/<album>/file.
//
// Two invariants downstream want exactly that depth and neither of them says so
// out loud. The batch is scanned by enumerating its *directories* — a file
// sitting at the batch root is never scanned at all — and deleteUpload accepts
// only a five-component path, so an album one level too shallow can never be
// removed by its owner. Handing scan_dirs() the batch directory itself
// would fix the first and make the second permanent: whatever directory it is
// given becomes an artist row named by its basename, and the user would be
// looking at a UUID in their artist list.
//
// A file directly in the batch is filed under `fallback_artist` — a handler's
// name reads far better than a UUID; a file one level down keeps the directory
// it is in, since something chose that name. Grouping by stem is what keeps a
// sidecar image with the video it belongs to.
//
// reorganise_by_tags() has usually done this already for audio. This is for
// what it cannot help with: it reads tags, and no video container has any.
int reparent_loose_media(const std::filesystem::path& batch_root,
                          const std::string& fallback_artist)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	std::vector<fs::path> loose;
	for (auto& e : fs::recursive_directory_iterator(batch_root, ec)) {
		if (!e.is_regular_file(ec)) continue;
		auto rel   = e.path().lexically_relative(batch_root);
		int  depth = (int)std::distance(rel.begin(), rel.end()) - 1;
		if (depth >= 2) continue;
		loose.push_back(e.path());
		}
	if (loose.empty()) return 0;

	std::string artist = sanitise_component(fallback_artist);
	if (artist.empty()) artist = "Unknown Artist";

	int moved = 0;
	for (const auto& p : loose) {
		// split_sidecar_name, not stem(): a loose "Track.chapters.txt" belongs
		// to the album "Track", beside the media it describes, not to one
		// called "Track.chapters" of its own.
		std::string album = sanitise_component(split_sidecar_name(p).first);
		if (album.empty()) album = "Unknown Album";
		fs::path parent = p.parent_path() == batch_root
		    ? batch_root / artist : p.parent_path();
		fs::path target = parent / album;
		fs::create_directories(target, ec);
		fs::rename(p, target / p.filename(), ec);
		if (ec)
			std::cout << stamp() << "reparent: rename failed for " << p << ": "
			          << ec.message() << std::endl;
		else
			moved++;
		}
	// Worth a line: for a URL fetch this means the handler's output template
	// wrote too shallow, which is the operator's to fix.
	if (moved)
		std::cout << stamp() << "reparent: filed " << moved
		          << " loose file(s) under " << batch_root << std::endl;
	return moved;
	}

// Moves everything in `src` into `dst`, which may already hold entries of the
// same name, then removes the emptied `src`.
//
// A plain fs::rename refuses a non-empty target, which is exactly the case
// merging exists for, so the move is done entry by entry. It recurses only
// where both sides are directories; doing a whole subtree in one call would
// abandon half a batch with nothing said about it.
static void batch_merge_into(const std::filesystem::path& src,
                             const std::filesystem::path& dst)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// Collected before anything moves: the first rename mutates the directory
	// this iterator is walking, and that is undefined.
	std::vector<fs::path> entries;
	for (auto& e : fs::directory_iterator(src, ec)) entries.push_back(e.path());
	if (ec) {
		std::cout << stamp() << "batch names: cannot read " << src << ": "
		          << ec.message() << std::endl;
		return;
		}

	fs::create_directories(dst, ec);
	if (ec) {
		std::cout << stamp() << "batch names: cannot create " << dst << ": "
		          << ec.message() << std::endl;
		return;
		}

	for (const auto& p : entries) {
		fs::path target = dst / p.filename();
		std::error_code sec, tec;
		if (fs::is_directory(p, sec) && fs::is_directory(target, tec)) {
			batch_merge_into(p, target);
			continue;
			}
		// Two sources really do collide: the built-in handler writes a
		// cover.<ext> into every album directory, so merging two albums under
		// one name brings two of them, and a rename would destroy one silently.
		for (int n = 2; n < 100; n++) {
			std::error_code xec;
			if (!fs::exists(target, xec)) break;
			// The number goes *before* a sidecar's double extension, so
			// "Track.chapters.txt" becomes "Track (2).chapters.txt" and stays
			// paired with the "Track (2).opus" beside it.
			auto [base, ext] = split_sidecar_name(p);
			target = dst / (base + " (" + std::to_string(n) + ")" + ext);
			}
		std::error_code rec;
		fs::rename(p, target, rec);
		if (rec)
			std::cout << stamp() << "batch names: rename failed for " << p
			          << " -> " << target << ": " << rec.message() << std::endl;
		}

	// Only when it really is empty: a rename that failed above left a file
	// behind, and removing the directory anyway would delete it.
	std::error_code eec;
	if (fs::is_empty(src, eec) && !eec) fs::remove(src, eec);
	}

// Renames every directory directly inside `parent` to `name`, merging where
// that collides. Never recurses — see apply_batch_names for why the depth
// matters.
static void batch_rename_level(const std::filesystem::path& parent,
                               const std::string& name)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// Two error codes on purpose: is_directory() would otherwise overwrite the
	// iterator's, so a directory that could not be opened would be reported as
	// whatever the last entry's type test happened to say.
	std::vector<fs::path> dirs;
	std::error_code dec;
	for (auto& e : fs::directory_iterator(parent, ec))
		if (e.is_directory(dec)) dirs.push_back(e.path());
	if (ec) {
		std::cout << stamp() << "batch names: cannot read " << parent << ": "
		          << ec.message() << std::endl;
		return;
		}

	const fs::path target = parent / name;

	// A regular file already sitting under the wanted name is not something to
	// merge into. Nothing should put one there, since reparent_loose_media ran
	// first, but iterating it would only set an error code and the sources
	// would be left renamed nowhere with no explanation.
	std::error_code fec;
	if (fs::exists(target, fec) && !fs::is_directory(target, fec)) {
		std::cout << stamp() << "batch names: " << target
		          << " is not a directory; leaving the names alone" << std::endl;
		return;
		}

	for (const auto& d : dirs) {
		if (d.filename() == name) continue;   // already the typed name

		// On a case-insensitive filesystem — macOS by default — exists() is
		// true for "artist" while "Artist" is what is on disk. Merging a
		// directory into itself would move each child into the directory it is
		// already in and then remove it, so the two are told apart by identity
		// rather than by name: equivalent() compares device and inode. A plain
		// rename is right there, and is what corrects the case.
		std::error_code xec, qec;
		bool exists = fs::exists(target, xec);
		bool same   = exists && fs::equivalent(d, target, qec) && !qec;

		if (exists && !same) {
			batch_merge_into(d, target);
			continue;
			}
		std::error_code rec;
		fs::rename(d, target, rec);
		if (rec)
			std::cout << stamp() << "batch names: rename failed for " << d
			          << " -> " << target << ": " << rec.message() << std::endl;
		}
	}

// Files a whole batch under names a person typed before pressing Fetch, by
// renaming the two directory levels the producers create. An empty name means
// "keep whatever the handler chose", so a blank artist with an album given
// renames only the second level.
//
// **This is a plain fs::rename and must never become relocate_prefix().**
// Nothing under the batch has been indexed yet, and that is what BatchHold
// guarantees: both producers take a hold before the batch directory exists and
// scan_batch() releases it only after the fold, so every path that walks the
// uploads root — scan(), scan_dirs() and the watcher through them — skips this
// batch whole for the whole of the window these renames happen in. There is
// therefore no row holding any of these paths to repair — and relocate_prefix's
// plain UPDATEs are safe only because its callers first checked the destination
// was free, which merging deliberately does not do.
//
// Renaming rather than substituting the names into the handler's -o template is
// also deliberate, and the second reason is the stronger one. urlfetch_expand()
// replaces whole argv elements, so a placeholder inside -o would need a
// substring rewrite; and -o is yt-dlp's own format language, where '%' is
// significant, so a name interpolated there would be expanded by the tool
// rather than by us. The guarantee that a hostile *field* cannot escape the
// batch says nothing about hostile template text.
//
// **Only the top two levels are touched.** A handler that wrote a third
// (Artist/Album/Disc 2/track) keeps it: that is a disc directory to the
// scanner, and renaming it would fuse two discs into one. Leaving the depth
// alone is also what preserves deleteUpload's exact-five-component check.
//
// Several directories at a level are merged into the one typed name, because
// "fetch this playlist as Artist X, Album Y" is the request being answered.
//
// Tags are deliberately *not* rewritten, unlike moveAlbum. The scanner takes
// artist and album from directory names and never from tags, so nothing in the
// API is wrong; there is no folder id yet to keep consistent; and running
// TagLib over a whole batch inside the fetch worker would add minutes to a path
// whose entire point is that "done" means done.
void apply_batch_names(const std::filesystem::path& batch_root,
                       const std::string& artist,
                       const std::string& album)
	{
	namespace fs = std::filesystem;
	if (artist.empty() && album.empty()) return;

	// Belt and braces over sanitise_component's traversal guard, the same
	// pairing moveAlbum keeps. It cannot fire for a name that came through
	// the endpoint; it is here so that stops being an accident.
	auto one_component = [](const std::string& s) {
		return !s.empty() && s != "." && s != ".."
		    && s.find('/')  == std::string::npos
		    && s.find('\\') == std::string::npos;
		};
	if ((!artist.empty() && !one_component(artist))
	    || (!album.empty() && !one_component(album))) {
		std::cout << stamp() << "batch names: refusing a name that is not one "
		             "path component" << std::endl;
		return;
		}

	if (!artist.empty()) batch_rename_level(batch_root, artist);

	// The album level is renamed inside *every* artist directory, not just one.
	// With no artist given there may be several, and "call the album X" is as
	// true of each; with an artist given there is exactly one by now, so the
	// two cases are the same loop.
	if (!album.empty()) {
		std::error_code ec, dec;
		std::vector<fs::path> artists;
		for (auto& e : fs::directory_iterator(batch_root, ec))
			if (e.is_directory(dec)) artists.push_back(e.path());
		for (const auto& a : artists) batch_rename_level(a, album);
		}

	std::cout << stamp() << "batch names: " << batch_root << " filed under "
	          << (artist.empty() ? "<handler>" : artist) << " / "
	          << (album.empty()  ? "<handler>" : album) << std::endl;
	}

// Folds a finished batch into the user's earlier ones, and fills [to_scan] with
// the stored-form path each of its artist directories ended up at.
//
// **The batch UUID isolates a fetch while it runs; it is not how the uploads
// area is organised.** Without this, every fetch is its own island:
// scan_artist_dir() parents an artist directory straight to the root
// (`upsert_folder(artist_path, root_id)`), skipping the <user>/<uuid> levels, so
// two batches naming the same artist become two folder rows with the same name
// and the personal listing shows both. Fetching six tracks of one concert — the
// case the sticky name fields exist for — produced six artists holding one
// one-track album each.
//
// So the isolation is kept exactly where it is needed and dropped afterwards.
// The batch must stay its own directory *during* the fetch: a failed, cancelled
// or timed-out job does remove_all() on it, and a shared directory would let a
// failure delete files an earlier fetch had already put there.
//
// batch_merge_into() does the work and already has the right semantics — it
// recurses where both sides are directories, so an album inside a merged artist
// merges too, and it suffixes colliding *files* rather than overwriting them,
// which it does because the built-in handler writes a cover.<ext> into every
// album directory.
//
// Callers must serialise this: two batches folding into each other at once would
// each move the other's contents away. See batch_fold_mu_.
void fold_batch_into_siblings(const std::filesystem::path& batch_root,
                              const std::string& rel_batch,
                              std::set<std::string>& to_scan)
	{
	namespace fs = std::filesystem;
	std::error_code ec;

	// "<uploads root>/<user>", the stored-form prefix every batch of this user
	// shares. rel_batch always has at least two components, being built as
	// "<root>/<user>/<uuid>".
	auto cut = rel_batch.rfind('/');
	if (cut == std::string::npos) return;
	const std::string rel_user = rel_batch.substr(0, cut);

	// The user's other batches, in a fixed order so that repeated fetches
	// converge on the same one rather than picking a different target each
	// time. In practice at most one holds any given name, because this runs
	// after every successful batch — the ordering matters only for batches
	// that predate it.
	std::vector<fs::path> siblings;
	for (auto& e : fs::directory_iterator(batch_root.parent_path(), ec)) {
		std::error_code dec;
		if (!e.is_directory(dec)) continue;
		// Identity, not name: the same reason batch_rename_level uses it.
		std::error_code qec;
		if (fs::equivalent(e.path(), batch_root, qec) && !qec) continue;
		siblings.push_back(e.path());
		}
	std::sort(siblings.begin(), siblings.end());

	// **The destinations below are deliberately not held**, though a merge
	// moves content into one and a scan could catch it half-moved.
	//
	// A hold means "this batch was never complete", which is what lets
	// sweep_held_batches() delete one still held at startup. A destination here
	// is an established batch with rows, so holding it would have a crash
	// mid-fold destroy somebody's earlier upload -- trading a transient wrong
	// listing for permanent data loss. The race is self-healing: keep_rel goes
	// into to_scan a few lines below, so the same paths are rescanned the
	// moment the fold returns.
	//
	// Closing it properly means telling "incomplete" from "busy", which is a
	// second marker or a DB test in the sweep. Not worth it for a window this
	// short, and recorded in ISSUES.md rather than left to be rediscovered.

	// Collected before anything moves: merging mutates the directory this would
	// otherwise still be iterating.
	std::vector<fs::path> mine;
	for (auto& e : fs::directory_iterator(batch_root, ec)) {
		std::error_code dec;
		if (e.is_directory(dec)) mine.push_back(e.path());
		}

	for (const auto& a : mine) {
		const std::string name = a.filename().string();

		// Every earlier batch already holding this name. Normally at most one,
		// since this runs after every successful batch — but a library that
		// predates the fold can hold several, and merging *all* of them is what
		// clears those up instead of leaving the strays there for ever.
		std::vector<fs::path> holders;
		for (const auto& s : siblings) {
			std::error_code xec;
			if (fs::is_directory(s / name, xec)) holders.push_back(s);
			}
		if (holders.empty()) {
			to_scan.insert(rel_batch + "/" + name);
			continue;
			}

		const fs::path keep       = holders.front() / name;
		const std::string keep_rel =
			rel_user + "/" + holders.front().filename().string() + "/" + name;

		// The strays first, so the survivor holds everything before this batch
		// joins it. Each of *these* was scanned when it was made, unlike the
		// batch being folded — so its own path goes into the scan set too: the
		// directory is about to stop existing, and scan_artist_dir() finding it
		// gone is what prunes the folder row that still names it. Leaving that
		// out would swap one visible duplicate for one invisible phantom.
		//
		// A stray's stars and play counts do not survive, and cannot: they are
		// keyed on the path, and the one tool for moving that key —
		// relocate_prefix() — is safe only when the destination is free, which
		// is precisely what merging is not. Accepted rather than worked around,
		// because this is pre-promotion staging: the rows can only exist if
		// somebody played a duplicate they had not filed yet, and the
		// alternative is leaving the duplicate on screen for ever.
		for (size_t i = 1; i < holders.size(); i++) {
			batch_merge_into(holders[i] / name, keep);
			to_scan.insert(rel_user + "/" + holders[i].filename().string()
			               + "/" + name);
			}

		// Nothing under *this* batch was ever indexed — it has been held since
		// before it existed, and scan_batch() does not release until this
		// returns — so the source needs no prune, only the destination a
		// rescan.
		batch_merge_into(a, keep);
		to_scan.insert(keep_rel);
		std::cout << stamp() << "batch merge: " << rel_batch << "/" << name
		          << " -> " << keep_rel
		          << (holders.size() > 1
		              ? " (and " + std::to_string(holders.size() - 1) + " stray)"
		              : "")
		          << std::endl;
		}

	// Only when the fold emptied it. A batch holding an artist nobody else had
	// stays exactly where it is.
	std::error_code eec;
	if (fs::is_empty(batch_root, eec) && !eec) fs::remove(batch_root, eec);
	}

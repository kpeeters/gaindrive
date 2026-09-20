#pragma once

// Everything between a producer writing files into a batch directory and that
// batch being a shape the scanner can index: unpacking an archive, sorting
// loose files into <artist>/<album>, converting the sidecars a download tool
// leaves behind, applying names the user typed, and folding a finished batch
// into the ones that came before it.
//
// Split out of gaindrive.cc because two producers share every step of it: the
// /upload endpoint, which unpacks an archive, and the URL fetcher, which runs
// a download tool. They differ only in how the bytes arrive.

#include <filesystem>
#include <set>
#include <string>

#include "mediastore.hh"

// A random batch identifier, also the name of the batch's directory.
std::string make_uuid();

// True when `s` ends with `suffix`, ignoring case.
bool ends_with_ci(const std::string& s, const std::string& suffix);

// One path component with everything a filesystem should not be asked to hold
// removed. Applied on the way in, because these become directory names rather
// than only wire strings.
std::string sanitise_component(const std::string& s);

// Unpacks `archive_file` under `dest_dir`. Returns the number of entries
// written, or a negative value on failure. Bounded in both entries and bytes:
// an archive's compressed size bounds nothing, the ratio is the attack.
int extract_archive_to_dir(const std::filesystem::path& archive_file,
                           const std::filesystem::path& dest_dir);

// Files whatever is still loose in the batch under an artist directory: a file
// directly in the batch goes to `fallback_artist`, one a level down keeps the
// directory it is in. Returns how many it moved. reorganise_by_tags() has
// usually done this already for audio; this is for what it cannot help with,
// since it reads tags and no video container has any.
int reparent_loose_media(const std::filesystem::path& batch_root,
                         const std::string& fallback_artist);

// Sorts whatever is under `batch_root` into <artist>/<album>/file, reading the
// tags to decide.
void reorganise_by_tags(const std::filesystem::path& batch_root);

// Turns the sidecar files a download tool writes beside the media into the
// forms the scanner understands.
void convert_tool_sidecars(const std::filesystem::path& batch_root);

// Renames the batch's two directory levels to the names the user typed, each
// already sanitised. Does nothing when both are empty.
void apply_batch_names(const std::filesystem::path& batch_root,
                       const std::string& artist,
                       const std::string& album);

// Merges a finished batch into the user's earlier ones, adding every artist
// directory that has to be rescanned to `to_scan`.
void fold_batch_into_siblings(const std::filesystem::path& batch_root,
                              const std::string& rel_batch,
                              std::set<std::string>& to_scan);

// A marker outlives the process that wrote it, so a batch still holding one at
// startup was being written when the server stopped. What is under it is a
// part-extracted archive or a part-downloaded video, which is not playable and
// never will be -- the same judgement fetch_worker() makes when a download
// fails, and the reason a job keeps no state a restart would have to repair.
//
// Without this the marker would be permanent and the batch invisible for ever,
// which is the one way an on-disk hold is worse than an in-memory one.
void sweep_held_batches(const std::string& users_dir);

// Creates the marker batch_held() tests, and removes it however the scope is
// left. The producers hold one across everything that moves a directory under
// a batch; scan_batch() releases it deliberately, after the fold and before the
// scan, so the guard's own destructor is the safety net for an early return or
// a throw rather than the normal path.
class BatchHold {
   public:
      explicit BatchHold(const std::filesystem::path& batch);
      ~BatchHold();
      BatchHold(const BatchHold&)            = delete;
      BatchHold& operator=(const BatchHold&) = delete;
   private:
      std::filesystem::path marker_;
   };

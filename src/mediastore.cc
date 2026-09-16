#include "mediastore.hh"
#include "stamp.hh"
#include "md5.hh"

#include <iostream>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <ctime>
#include <iomanip>
#include <map>
#include <exception>
#include <fstream>
#include <iterator>
#include <regex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <taglib/fileref.h>
#include <tfilestream.h>
// The one container-specific header here, for MP4::Properties::codec(): a .m4a
// is AAC or ALAC and nothing outside the container says which.  The Ogg family
// is identified from its own first packet instead -- see
// read_song_audio_form() -- which needs no header at all.
#include <mp4file.h>
#include <taglib/tag.h>
#include <taglib/audioproperties.h>
#include <tpropertymap.h>

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

#include "codecs.hh"
#include "dvd.hh"
#include "parallel.hh"
#include "tmdb.hh"
#include "untrusted.hh"
#include "videoname.hh"

namespace fs = std::filesystem;

// How long a contended write waits before SQLite reports SQLITE_BUSY.
// Generous enough to absorb the brief overlaps that happen in practice, short
// enough that a writer which is genuinely stuck gets reported rather than
// hanging the scan indefinitely.
static constexpr int DB_BUSY_TIMEOUT_MS = 10000;

// How long one ffprobe of one file may take before the scan gives up on it.
// Generous, because a feature-length container on a spinning disk is seconds;
// a probe that times out leaves the row without duration or dimensions, which
// is what a probe failure has always meant.
static constexpr reproc::milliseconds PROBE_TIMEOUT(60000);

// The music DB's cache-scheme version, in PRAGMA user_version.
//
// It is not a schema version -- new columns arrive through the ALTER TABLE
// migrations below and need nothing here. It covers the other kind of change:
// one that alters the *bytes* a derived row holds without altering anything
// the row keys on, so nothing already stored can ever be recognised as stale.
// cover_thumbs is the case it was written for -- keyed (source_key, size)
// with staleness checked against source_stamp, none of which a change to the
// scaler moves. A quality change, a ladder change or a new encoder are the
// same shape.
//
//   1 -- thumbnails fit the short edge, not the long one
//   2 -- artist lookups searched MusicBrainz's name field only, so an artist
//        filed under a romanization or any other alias resolved to nothing --
//        and, the search having returned 200, that nothing was cached as an
//        answer. See mb_artist_query() in artistmatch.hh.
//
// Dropping the rows is safe because the music DB is a cache by invariant: the
// cost of being wrong here is one re-scale per image per size, or one lookup
// re-made. This is the server-side twin of the "t2-" ETag marker in
// gaindrive.cc, which does the same job for the copies clients hold; the two
// want bumping together.
//
// The steps are guarded individually rather than run together under
// `have < MUSIC_CACHE_VERSION`, so an install already at 1 does not re-scale
// every thumbnail it holds to pick up an unrelated change to step 2.
static constexpr int MUSIC_CACHE_VERSION = 2;

// Adds its lifetime, in microseconds, to one of MediaStore::scan_times_'
// accumulators.  Declared at the top of a phase's scope so the timing is one
// line rather than a pair of statements the next edit can separate.
struct PhaseTimer {
	std::atomic<long long>&               sink;
	std::chrono::steady_clock::time_point t0;
	explicit PhaseTimer(std::atomic<long long>& s)
		: sink(s), t0(std::chrono::steady_clock::now()) {}
	~PhaseTimer()
		{
		sink.fetch_add(std::chrono::duration_cast<std::chrono::microseconds>(
			std::chrono::steady_clock::now() - t0).count());
		}
	};

static const std::set<std::string> AUDIO_EXTENSIONS = {
	".flac", ".mp3", ".ogg", ".oga", ".m4a", ".aac", ".wav", ".opus", ".wma"
	};

// Video sources.  Kept in sync with VIDEO_TARGETS in codecs.hh, which maps the
// same extensions to MIME types; this set is the scanner's admission test and
// that table is the serving side.
static const std::set<std::string> VIDEO_EXTENSIONS = {
	".mkv", ".mp4", ".m4v", ".avi", ".mpg", ".mpeg", ".mov", ".webm", ".wmv",
	".vob"
	};

// Candidate cover art filenames in priority order.  Add more here as needed.
static const std::vector<std::string> COVER_FILENAMES = {
	"cover.jpg", "cover.jpeg", "folder.jpg", "folder.jpeg",
	"front.jpg", "front.jpeg", "cover.png"
	};

static bool iends_with(const std::string& s, const std::string& suffix)
	{
	if (suffix.size() > s.size()) return false;
	std::string lo = s;
	std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
	return lo.compare(lo.size() - suffix.size(), suffix.size(), suffix) == 0;
	}

// Only ever called for a folder that is an album and nothing else, which is
// why pass 5 may recurse freely.  It used to take a `recurse` flag, for the
// one folder that was an album *and* a parent of albums — a section holding
// loose files beside its subfolders, under the rule where such a folder was
// itself an album.  Pass 5 would have handed that section one of its own
// albums' covers.  A loose file is its own album now, so no such folder
// exists and the flag had no callers left.
static std::string find_cover(const fs::path& dir)
	{
	// Pass 1: exact well-known names.
	for (auto& name : COVER_FILENAMES) {
		auto p = dir / name;
		if (fs::exists(p)) return p.string();
		}
	// Pass 2: *front.jpg/jpeg  Pass 3: *.jpg/jpeg  Pass 4: any image (single scan for all).
	std::string jpg_fallback;
	std::string any_img_fallback;
	for (auto& entry : fs::directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		// "._cover.jpg" is an AppleDouble resource fork, not the cover.  Pass 1
		// is safe without this because it matches whole names; every pass from
		// here down matches a suffix, which a fork's name shares.
		if (is_hidden_name(entry.path())) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, "front.jpg") || iends_with(fname, "front.jpeg"))
			return entry.path().string();
		if (jpg_fallback.empty() && (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg")))
			jpg_fallback = entry.path().string();
		if (any_img_fallback.empty() && iends_with(fname, ".png"))
			any_img_fallback = entry.path().string();
		}
	if (!jpg_fallback.empty()) return jpg_fallback;
	if (!any_img_fallback.empty()) return any_img_fallback;
	// Pass 5: recurse into subdirectories for any image.
	for (auto& entry : fs::recursive_directory_iterator(dir)) {
		if (!entry.is_regular_file()) continue;
		if (is_hidden_name(entry.path())) continue;
		std::string fname = entry.path().filename().string();
		if (iends_with(fname, ".jpg") || iends_with(fname, ".jpeg") || iends_with(fname, ".png"))
			return entry.path().string();
		}
	return "";
	}

// A poster belonging to one file rather than to a folder: "film.mp4" is
// matched by "film.jpg" or "film-poster.jpg".  That is what Kodi and Jellyfin
// read beside a flat movie file, and it is the only way a loose file can carry
// a cover of its own — a folder cover belongs to the whole section it sits in.
static std::string find_song_cover(const fs::path& file)
	{
	static const std::vector<std::string> SUFFIXES = {
		".jpg", ".jpeg", ".png",
		"-poster.jpg", "-poster.jpeg", "-poster.png"
		};
	std::string stem = (file.parent_path() / file.stem()).string();
	for (auto& suffix : SUFFIXES) {
		fs::path p = stem + suffix;
		if (fs::exists(p)) return p.string();
		}
	return "";
	}

// Cover art for a song row: its own sidecar image when it has one, otherwise
// the album folder's cover.  al.folder_id rather than s.folder_id because a
// song on a multi-disc album lives in a disc subfolder, which has no albums
// row to resolve a cover on.  See MediaStore::SONG_COVER_ID_BASE for why a
// song's own cover is an offset id.
static const std::string SONG_COVER_ART_SQL =
	"       CASE WHEN s.cover_path IS NOT NULL AND s.cover_path != ''"
	"            THEN " + std::to_string(MediaStore::SONG_COVER_ID_BASE) + " + s.id"
	"            WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
	"            THEN COALESCE(al.folder_id, s.folder_id)"
	"            ELSE -1 END AS cover_art_id,";

// The three columns a video entry needs beyond the ones audio already selects:
// the codec pair nativeSeek is derived from, and the season that separates
// "Series 2" from "Disc 2" in a client's grouping.
//
// It is a *suffix*, with a leading comma, and every caller appends it to the
// very end of its SELECT list.  That is not stylistic: several of these queries
// carry their own columns in the middle — pq.is_current, b.position, an
// explicitly unused f.parent_id — so appending is what keeps every existing
// getColumn() index valid.  Inserting these beside the other song columns would
// silently renumber the rest.
//
// Cover art is deliberately *not* here: SONG_COVER_ART_SQL above answers it as
// one column in the middle of the list, sidecar arm included, so a query that
// uses that fragment needs nothing further.  The queries that hand-rolled a
// cover CASE without the sidecar arm were the cover half of the same gap, and
// the fix for those is to use the shared fragment rather than to select
// s.cover_path raw.
static const std::string SONG_VIDEO_COLS_SQL =
	", s.video_codec, s.audio_codec, s.season";

// The artist folder of an album whose folder is aliased `f`: the folder above
// it, always.
//
// It used to have a second arm, for a folder that was an album *and* its own
// artist — a section that directly contained media, under the rule where such
// a folder was itself an album.  Reporting the parent there named the *root*,
// and a client anchoring its album list on this field landed on the list of
// sections.  A loose file is its own album now and its parent is the section,
// which is a real artist folder, so the case cannot arise.
//
// The root arm survives as insurance for a database not yet rescanned under
// the new rule, where a section still carries its old albums row.
static const std::string ALBUM_ARTIST_ID_SQL =
	"       CASE WHEN f.parent_id IS NULL"
	"            THEN f.id ELSE f.parent_id END,";

// How many of an album's songs are video, appended as the trailing column of
// the three AlbumEntry queries.
//
// A count rather than a flag because it costs the same and says more: a folder
// holding one bonus documentary beside its songs is not a folder of films, and
// a client can label the row from it.
//
// Computed here rather than stored on the album row, unlike every other album
// aggregate. albums.song_count and albums.duration are written by no scan path
// at all -- there is one INSERT INTO albums and it names neither, and no UPDATE
// touches them -- so both have always read 0, and a stored video_count would be
// a third column waiting for a write that never comes. songs.album_id is
// indexed, so this is one seek per listed album.
static const std::string ALBUM_VIDEO_COUNT_SQL =
	",      (SELECT COUNT(*) FROM songs sv"
	"         WHERE sv.album_id = al.id AND sv.is_video = 1)";

// Returns sorted list of image paths in dir, excluding cover_path.  Recursive
// unless the caller says otherwise — see get_extra_image_paths(), where a
// folder with subfolders is a section whose albums' images are not its own.
static std::vector<std::string> find_extra_images(const fs::path& dir,
                                                   const std::string& cover_path,
                                                   bool recurse = true)
	{
	static const std::set<std::string> IMG_EXT = {".jpg", ".jpeg", ".png"};
	std::vector<std::string> result;
	auto consider = [&](const fs::directory_entry& entry) {
		if (!entry.is_regular_file()) return;
		if (is_hidden_name(entry.path())) return;
		std::string ext = entry.path().extension().string();
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		if (!IMG_EXT.count(ext)) return;
		if (entry.path().string() == cover_path) return;
		result.push_back(entry.path().string());
		};
	try {
		if (recurse)
			for (auto& entry : fs::recursive_directory_iterator(dir)) consider(entry);
		else
			for (auto& entry : fs::directory_iterator(dir))           consider(entry);
		}
	catch (...) {}
	std::sort(result.begin(), result.end());
	return result;
	}

static std::string lower_ext(const fs::path& p)
	{
	std::string ext = p.extension().string();
	std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
	return ext;
	}

static bool is_video_file(const fs::path& p)
	{
	return VIDEO_EXTENSIONS.count(lower_ext(p)) > 0;
	}

// Admission test for the scanner.  Audio and video share the songs table and
// therefore share this gate; is_video_file() decides which metadata reader
// runs later.
static bool is_media_file(const fs::path& p)
	{
	std::string ext = lower_ext(p);
	return AUDIO_EXTENSIONS.count(ext) > 0 || VIDEO_EXTENSIONS.count(ext) > 0;
	}

// ffprobe reports every numeric field as a JSON *string*, but not every field
// is present in every container.  Returns 0 for missing, null or unparseable.
static double probe_num(const nlohmann::json& j, const char* key)
	{
	auto it = j.find(key);
	if (it == j.end() || it->is_null()) return 0;
	if (it->is_number()) return it->get<double>();
	if (it->is_string()) {
		try { return std::stod(it->get<std::string>()); }
		catch (...) { return 0; }
		}
	return 0;
	}

// Everything the streamer and the video endpoints need to know about a video
// file.  TagLib cannot open these containers at all, so unlike the audio path
// there is no fallback reader: without ffprobe a video row has no duration, no
// bitrate and no dimensions.
struct VideoProbe
	{
	double      duration = 0;
	int         bitrate  = 0;   // kbps, the same unit the audio path stores
	int         width    = 0;
	int         height   = 0;
	std::string video_codec;
	std::string audio_codec;
	};

// A probe failure is deliberately not fatal.  The caller still creates the
// row: a video that plays but reports duration 0 is a better outcome than a
// file silently missing from the library, and the next scan retries.
// How much of a file avformat_find_stream_info() may look at on the first
// attempt.  The defaults are 5 MB and 5 s of content, and reading them is what a
// probe actually costs: measured per video, 483 ms in a scan against 111 ms
// with the file already in page cache and 89 ms for a bare `ffprobe -version`,
// so ~77% of it is disk.  Across ~2000 films that is 10 GB read to extract a
// duration, two codec names and a resolution — and 10 GB pushed through the
// page cache is also what evicts the directory metadata Phase 1 lives on.
static const char* PROBE_SIZE_FAST     = "1000000";   // bytes
static const char* PROBE_DURATION_FAST = "1000000";   // microseconds

static std::optional<VideoProbe> probe_video(const std::string& path,
                                              bool thorough = false)
	{
	std::vector<std::string> args = {
		// -fpsprobesize 0 because nothing here stores a frame rate.  It is the
		// AVFormatContext `fps_probe_size` option, the number of frames
		// avformat_find_stream_info() reads to establish avg_frame_rate, and
		// decoding those frames is a large part of what a probe costs — 491.7
		// ms a file, measured, against ~86 ms for a TagLib read.  Every field
		// this function does read comes from somewhere else: duration and
		// bit_rate from the container header, width, height and the codec
		// names from the stream parameters.
		//
		// **No -threads 1 here, and it is not an oversight.**  It was tried,
		// on the theory that eight concurrent probes each defaulting their
		// decoder pool to the core count was scheduler thrash.  Two clean runs
		// either side of it — every other phase matching within 10% — put
		// `meta` 31 s worse with it than without, so forcing each probe
		// single-threaded cost more than the oversubscription did.  The
		// evidence for the theory never appeared either: `ps -eLf` showed 3-4
		// threads per probe with the flag set, and a bare `ffprobe -version`
		// uses 1.27 cores with no file open at all, so most of what looked
		// like decode threads is process start-up.
		"ffprobe", "-v", "quiet", "-fpsprobesize", "0"
		};
	// The narrowed window is a *first* attempt, never the only one: see
	// read_video_probe() below, which re-runs at ffmpeg's defaults whenever the
	// cheap pass came back without the fields that matter.  That is what makes
	// this a latency change rather than an accuracy trade — the files a small
	// window cannot describe pay for two probes, and nothing is quietly lost.
	if (!thorough) {
		args.push_back("-probesize");
		args.push_back(PROBE_SIZE_FAST);
		args.push_back("-analyzeduration");
		args.push_back(PROBE_DURATION_FAST);
		}
	args.push_back("-print_format");
	args.push_back("json");
	args.push_back("-show_format");
	args.push_back("-show_streams");
	args.push_back(path);

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	// A deadline, for the reason VideoArt::run() has one: this runs inside a
	// scan, and a corrupt file — or one on a mount that has gone away — must
	// not stall it.  Parallelising Phase 3 downgraded that from "the scan
	// stops" to "one worker stops", which is better and still wrong.
	opts.deadline = PROBE_TIMEOUT;
	if (proc.start(args, opts)) return std::nullopt;

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return std::nullopt;

	VideoProbe vp;
	try {
		auto j = nlohmann::json::parse(out);

		if (auto f = j.find("format"); f != j.end()) {
			vp.duration = probe_num(*f, "duration");
			vp.bitrate  = static_cast<int>(probe_num(*f, "bit_rate") / 1000.0);
			}

		auto streams = j.find("streams");
		if (streams != j.end() && streams->is_array())
			for (const auto& s : *streams) {
				auto type = s.value("codec_type", std::string());
				// An embedded cover image is carried as a video stream with
				// attached_pic set.  Taking it as *the* video stream would
				// describe an m4a-style cover as a 600x600 mjpeg "movie".
				int attached = 0;
				if (auto d = s.find("disposition"); d != s.end())
					attached = d->value("attached_pic", 0);
				if (type == "video" && vp.video_codec.empty() && attached == 0) {
					vp.video_codec = s.value("codec_name", std::string());
					vp.width       = static_cast<int>(probe_num(s, "width"));
					vp.height      = static_cast<int>(probe_num(s, "height"));
					}
				else if (type == "audio" && vp.audio_codec.empty())
					vp.audio_codec = s.value("codec_name", std::string());
				}
		}
	catch (const std::exception&) { return std::nullopt; }

	// Containers that omit format.bit_rate (some MKVs) still have a size and a
	// duration, and the throttle needs a non-zero figure to pace with.
	if (vp.bitrate <= 0 && vp.duration > 0) {
		std::error_code fec;
		auto size = fs::file_size(path, fec);
		if (!fec && size > 0)
			vp.bitrate = static_cast<int>(
				static_cast<double>(size) * 8.0 / vp.duration / 1000.0);
		}

	return vp;
	}

// One file's probe: cheap window first, ffmpeg's defaults only when the cheap
// one came back unusable.
//
// "Unusable" is deliberately narrow — no duration at all, or no stream of
// either kind found.  Those are the two shapes a probe window that was too
// small actually takes, and both break something downstream: hls.m3u8 is
// arithmetic over the duration, and the codec pair is what picks the serving
// tier in serve_video().  Anything else the small window returns is the same
// answer the large one would have given, because it comes from the container
// header or the stream parameters rather than from analysis.
//
// Note what is deliberately *not* a retry: a container with audio and no video
// stream.  Those exist and reach here — a `.webm` holding Opus is filed as a
// video by extension alone, which is why the URL-fetch handler pins its output
// format — and re-probing one on every scan would buy nothing, since the wide
// window would find no video stream either.
static std::optional<VideoProbe> read_video_probe(const std::string& path)
	{
	auto vp = probe_video(path);
	if (vp && vp->duration > 0
	       && !(vp->video_codec.empty() && vp->audio_codec.empty()))
		return vp;

	auto full = probe_video(path, true /* thorough */);
	// The fast result is kept when the thorough one fails outright, so a
	// narrowed window can only ever add information here, never remove it.
	return full ? full : vp;
	}

// Returns file modification time as Unix seconds.
static int64_t mtime_of(const fs::path& p)
	{
	auto lwt  = fs::last_write_time(p);
	auto sys  = std::chrono::file_clock::to_sys(lwt);
	return std::chrono::duration_cast<std::chrono::seconds>(
		sys.time_since_epoch()).count();
	}

// Helpers for the music-root-relative path convention. All paths persisted in
// the music DB (folders.path, songs.path, albums.cover_path) and the client DB
// (client.*) are stored as "<root name>/<path within that root>"; strip_root()
// converts an absolute path to that form on write, and join_root() composes the
// absolute form when a filesystem call needs it.

const MediaStore::RootRec* MediaStore::root_for_abs(const std::string& abs) const
	{
	for (const auto& r : roots_) {
		if (abs == r.cfg.path) return &r;
		if (abs.size() > r.path_slash.size()
		    && abs.compare(0, r.path_slash.size(), r.path_slash) == 0)
			return &r;
		}
	return nullptr;
	}

const MediaStore::RootRec* MediaStore::root_for_rel(const std::string& rel) const
	{
	auto slash = rel.find('/');
	std::string name = (slash == std::string::npos) ? rel : rel.substr(0, slash);
	for (const auto& r : roots_)
		if (r.cfg.name == name) return &r;
	return nullptr;
	}

// Absolute path -> "<root name>/<path within that root>".
std::string MediaStore::strip_root(const std::string& abs) const
	{
	const RootRec* r = root_for_abs(abs);
	if (!r) return abs;   // outside every root — shouldn't happen; pass through
	if (abs == r->cfg.path) return r->cfg.name;
	return r->cfg.name + "/" + abs.substr(r->path_slash.size());
	}

// "<root name>/<rest>" -> absolute path.  An unknown root name yields an empty
// string rather than a path built from a guess: every caller feeds the result
// to the filesystem, and a plausible-but-wrong path is worse than a failure.
std::string MediaStore::join_root(const std::string& rel) const
	{
	if (rel.empty()) return rel;
	const RootRec* r = root_for_rel(rel);
	if (!r) return {};
	auto slash = rel.find('/');
	if (slash == std::string::npos) return r->cfg.path;
	return r->path_slash + rel.substr(slash + 1);
	}

// Defence-in-depth: returns true iff the canonicalised candidate sits within
// the (already-canonicalised) base. Used to refuse any filesystem operation on
// a path that — after symlink resolution — escapes every root. The bases are
// canonicalised once at MediaStore ctor; the candidate is canonicalised every
// call.
static bool is_within(const fs::path& candidate, const fs::path& canonical_base)
	{
	std::error_code ec;
	auto c = fs::weakly_canonical(candidate, ec);
	if (ec) return false;
	auto [it_b, it_c] = std::mismatch(canonical_base.begin(), canonical_base.end(),
	                                   c.begin(), c.end());
	return it_b == canonical_base.end();
	}

// ---- MediaStore -------------------------------------------------------

// Insert suffix before ".db" extension, or append if no extension.
static std::string derive_path(const std::string& base, const std::string& suffix)
	{
	auto dot = base.rfind(".db");
	if (dot != std::string::npos && dot == base.size() - 3)
		return base.substr(0, dot) + suffix + ".db";
	return base + suffix;
	}

std::string MediaStore::client_db_path(const std::string& db_path)
	{
	return derive_path(db_path, "-client");
	}

MediaStore::MediaStore(const std::string& db_path, const std::vector<Root>& roots,
                       const std::string& user_db_path, int video_art_px,
                       bool video_art_frames, bool video_art_embedded,
                       int scan_jobs)
	// The third argument is the busy timeout, and it defaults to 0 — meaning
	// SQLite gives up on a contended write *immediately* and SQLiteCpp turns
	// that into a throw.  Any external writer (a second gaindrive, or sqlite3
	// running BEGIN IMMEDIATE) would therefore kill a scan in progress, even
	// though the contention is almost always momentary.  Waiting is the whole
	// fix for the common case; the exception handling around the scan covers
	// a writer that genuinely sits on the lock.
	//
	// Set here rather than by PRAGMA because the pragmas below — including
	// journal_mode=WAL — can themselves hit a locked database.  The timeout is
	// a property of the connection, so it also covers the attached client
	// schema.  db_mutex_ is no help: it serialises our own threads, and this
	// contention is between processes.
	: db_music_(derive_path(db_path, "-music"),
	            SQLite::OPEN_READWRITE | SQLite::OPEN_CREATE,
	            DB_BUSY_TIMEOUT_MS)
	{
	// 0 means "decide here", the way --transcode-jobs spells the same thing.
	// Clamped as well as resolved, because this is public API and main() is not
	// its only possible caller.
	//
	// **Capped, and the cap is about the disk rather than the CPU** — which is
	// the half a later reader is most likely to "fix". Measured on one library:
	// the knee was at 8 on an eight-core machine and 16 bought nothing, so the
	// spindle was already at its limit well below the core count. What was
	// never separated is whether that knee was the cores or the disk, and this
	// shape is right either way — if it was the cores, scaling with them is
	// correct and the cap only bites on large machines where the disk would
	// bind anyway; if it was the disk, the cap does the work and the scaling
	// only keeps a two-core NAS from oversubscribing itself.
	//
	// hardware_concurrency() returns 0 when it cannot tell, which the lower
	// bound absorbs, and it reports *host* cores rather than a cgroup quota —
	// so a CPU-limited container over-reports, and the cap bounds how wrong
	// that can be.
	scan_jobs_ = scan_jobs > 0
	           ? std::clamp(scan_jobs, 1, 16)
	           : (int)std::max(2u, std::min(8u,
	                   std::thread::hardware_concurrency()));
	std::cout << stamp() << "Scan reads metadata from " << scan_jobs_
	          << " file(s) at a time" << std::endl;

	// Emplaced even when every tier is off, because purge_disabled_video_art()
	// below asks it which ones are. Phase 3b is guarded on enabled(), not on
	// this being set.
	if (video_art_px > 0)
		video_art_.emplace(video_art_px, video_art_frames, video_art_embedded);

	// Normalise each root: strip trailing '/' so path never has one, derive
	// the always-has-one form used to compose and strip absolute paths, and
	// canonicalise once. path_is_within_root() compares against the canonical
	// forms, so a symlink inside a root that escapes it is caught at every
	// file open — while a root that is *itself* a symlink still works, since
	// its own canonical form is one of the bases.
	for (const auto& r : roots) {
		RootRec rec;
		rec.cfg = r;
		while (rec.cfg.path.size() > 1 && rec.cfg.path.back() == '/')
			rec.cfg.path.pop_back();
		rec.path_slash = rec.cfg.path + "/";
		std::error_code ec;
		rec.canonical = fs::weakly_canonical(fs::path(rec.cfg.path), ec);
		if (ec) rec.canonical = fs::path(rec.cfg.path);
		roots_.push_back(std::move(rec));
		}
	for (const auto& r : roots_)
		roots_public_.push_back(r.cfg);
	for (const auto& r : roots_)
		if (r.cfg.type == "uploads") {
			uploads_prefix_ = r.cfg.name + "/";
			uploads_like_   = r.cfg.name + "/%";
			break;
			}

	// User/state DB: explicit override if given, else derived from db_path.
	std::string client_path = user_db_path.empty()
	                          ? client_db_path(db_path) : user_db_path;
	db_music_.exec("PRAGMA journal_mode=WAL");
	db_music_.exec("PRAGMA foreign_keys=ON");
	// The two halves are given *different* durability, and the asymmetry is the
	// point of stating both rather than leaving either to the default.
	//
	// Under WAL, synchronous=FULL fsyncs the write-ahead log on every commit.
	// The scan commits once per album — measured at 33 ms each on a spinning
	// disk, which is one seek and a platter flush, against maybe a millisecond
	// of actual inserts — so it was 67 s of a 1660 s scan spent waiting for the
	// platter.  NORMAL syncs at checkpoints instead, and what it costs is that
	// a power loss or a kernel panic can lose the last few commits.  The
	// database is not corrupted by that, and an ordinary process crash loses
	// nothing at all, since the WAL is already in the page cache.
	//
	// That trade is only acceptable because of what this file is: the music DB
	// is a cache, and DATABASE.md says so as an invariant — delete it, rescan,
	// and everything comes back.  Losing the last few album commits costs a
	// rescan and nothing else.
	db_music_.exec("PRAGMA synchronous=NORMAL");
	db_music_.exec("ATTACH DATABASE '" + client_path + "' AS client");
	db_music_.exec("PRAGMA client.journal_mode=WAL");
	db_music_.exec("PRAGMA client.foreign_keys=ON");
	// FULL is already the default here; it is written out because the line
	// above changed the other half, and a reader who finds only that one has to
	// guess whether the client DB was considered.  It was: this is the half
	// nothing can rebuild — stars, playlists, play counts, a hand-picked cover,
	// a typed video title — so it keeps the fsync per commit.  Its writes are
	// small and rare, and a scan does not touch it.
	db_music_.exec("PRAGMA client.synchronous=FULL");
	create_schema();
	purge_disabled_video_art();
	sync_roots();
	}

// Images stored by an earlier run, by a tier that is now off.
//
// Leaving them would make "off" mean only "stop making new ones", which is not
// what anybody asking for it wants: the images would go on being the cover art
// for the whole collection. They are cheap to get back — one rescan with the
// tier enabled — which is what makes deleting them the right default rather
// than a destructive one.
//
// A TMDB poster is never touched here. It is not manufactured from the file
// and no local flag disables it; the key that would delete it is the API key.
void MediaStore::purge_disabled_video_art()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	if (!video_art_ || !video_art_->frames_allowed()) {
		db_music_.exec("DELETE FROM video_art WHERE source = 'frame'");
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "video art: dropped " << n
			          << " frame grabs (the frame tier is off)" << std::endl;
		}

	if (!video_art_ || !video_art_->embedded_allowed()) {
		db_music_.exec("DELETE FROM video_art WHERE source = 'embedded'");
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "video art: dropped " << n
			          << " embedded covers (the embedded tier is off)"
			          << std::endl;
		}

	// Whatever the reason a row went away, the cover_path pointing at it has
	// to go too. A cover_path that is also a songs.path is by construction the
	// video-art convention — a real image is never a song — so this repairs
	// exactly the dangling pointers and nothing else. Without it the art is
	// gone but every affected album still claims to have some, and getCoverArt
	// answers 404 for a cover the client was told existed.
	db_music_.exec(
		"UPDATE albums SET cover_path = ''"
		" WHERE cover_path IN (SELECT path FROM songs)"
		"   AND cover_path NOT IN (SELECT path FROM video_art)");
	db_music_.exec(
		"UPDATE songs SET cover_path = ''"
		" WHERE cover_path IN (SELECT path FROM songs)"
		"   AND cover_path NOT IN (SELECT path FROM video_art)");

	// And whatever was scaled from a blob that is now gone. Same test as the
	// two repairs above, for the same reason: a source_key that is also a
	// songs.path is a video-art key by construction.
	db_music_.exec(
		"DELETE FROM cover_thumbs"
		" WHERE source_key IN (SELECT path FROM songs)"
		"   AND source_key NOT IN (SELECT path FROM video_art)");

	txn.commit();
	}

void MediaStore::create_schema()
	{
	SQLite::Transaction txn(db_music_);

	// Asked before the CREATEs below, because afterwards it is unanswerable.
	// See the seed at the end of this function: song_genres has to be filled
	// from what the database already knows the first time it appears, or an
	// upgraded install shows no genres at all until every file is re-read.
	bool song_genres_is_new = true;
		{
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM sqlite_master"
			" WHERE type = 'table' AND name = 'song_genres'");
		song_genres_is_new = !q.executeStep();
		}

	// Music library tables (gaindrive-music.db, main schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS folders (
			id           INTEGER PRIMARY KEY,
			parent_id    INTEGER REFERENCES folders(id),
			path         TEXT NOT NULL UNIQUE,    -- "<root>/<rest>"; a root row stores just "<root>"
			name         TEXT NOT NULL,
			-- Set only on root rows: "artists" or "categories". Refreshed from
			-- the configuration on every start, so this is a cache of config
			-- rather than a source of truth.
			content_type TEXT,
			last_scanned DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_folders_parent ON folders(parent_id);

		CREATE TABLE IF NOT EXISTS artists (
			id             INTEGER PRIMARY KEY,
			name           TEXT NOT NULL,
			sort_name      TEXT,
			musicbrainz_id TEXT,
			image_path     TEXT,
			biography      TEXT,
			UNIQUE(name)
		);

		CREATE TABLE IF NOT EXISTS albums (
			id             INTEGER PRIMARY KEY,
			folder_id      INTEGER NOT NULL REFERENCES folders(id) UNIQUE,
			title          TEXT NOT NULL,
			sort_title     TEXT,
			year           INTEGER,
			genre          TEXT,
			disc_count     INTEGER DEFAULT 1,
			duration       REAL DEFAULT 0,
			song_count     INTEGER DEFAULT 0,
			cover_path     TEXT,                  -- "<root>/<rest>"
			-- Derived from this album's songs during the scan, and only when
			-- every tagged track agrees; a folder whose tracks disagree keeps
			-- NULL and keeps the online lookup. The release id is what a
			-- client means by an album's MBID; the release-group id is what
			-- getAlbumInfo2 can look up.
			musicbrainz_id              TEXT,
			musicbrainz_releasegroup_id TEXT,
			created        DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_scanned   DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_albums_folder ON albums(folder_id);

		CREATE TABLE IF NOT EXISTS album_artists (
			album_id      INTEGER NOT NULL REFERENCES albums(id)  ON DELETE CASCADE,
			artist_id     INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
			role          TEXT NOT NULL DEFAULT 'albumartist',
			display_order INTEGER DEFAULT 0,
			PRIMARY KEY (album_id, artist_id, role)
		);
		CREATE INDEX IF NOT EXISTS idx_album_artists_artist ON album_artists(artist_id);
		CREATE INDEX IF NOT EXISTS idx_album_artists_role   ON album_artists(role);

		CREATE TABLE IF NOT EXISTS songs (
			id                  INTEGER PRIMARY KEY,
			album_id            INTEGER NOT NULL REFERENCES albums(id) ON DELETE CASCADE,
			folder_id           INTEGER NOT NULL REFERENCES folders(id),
			path                TEXT NOT NULL UNIQUE, -- "<root>/<rest>"
			filename            TEXT NOT NULL,
			title               TEXT NOT NULL,
			sort_title          TEXT,
			track_number        INTEGER,
			disc_number         INTEGER DEFAULT 1,
			year                INTEGER,
			genre               TEXT,
			-- The file's own ARTIST tag, which is a different fact from the
			-- folder-derived artist in song_artists: on a compilation every
			-- track has a real artist while the folder says "Various Artists".
			--
			-- NULL means "never read" and '' means "read, no tag".  The
			-- distinction is what terminates the back-fill pass in Phase 3;
			-- collapsing the two makes it repeat for ever.
			artist              TEXT,
			duration            REAL NOT NULL DEFAULT 0,
			bitrate             INTEGER,
			sample_rate         INTEGER,
			channels            INTEGER,
			codec               TEXT,
			file_size           INTEGER,
			-- Video rows live in this table too: every piece of client state
			-- (stars, play counts, playlists, queue, bookmarks) joins on
			-- songs.path, so a separate videos table would mean duplicating
			-- all of it.  Subsonic makes the same choice.
			is_video            INTEGER DEFAULT 0,
			width               INTEGER DEFAULT 0,
			height              INTEGER DEFAULT 0,
			video_codec         TEXT,
			audio_codec         TEXT,
			audio_container     TEXT,
			-- The season an episode belongs to, from an S02E03 marker or from
			-- a "Season 2" folder; 0 for everything that is not an episode.
			-- disc_number carries the same number, because that is the field
			-- clients group and sort by; this one says the grouping is a
			-- season rather than a disc, which is all that separates
			-- "Series 2" from "Disc 2" in a client.
			season              INTEGER DEFAULT 0,
			-- Sidecar image beside this file ("<root>/<rest>"), for a loose
			-- file whose folder cover belongs to a whole section rather than
			-- to it. Empty for everything that inherits its album's cover.
			cover_path          TEXT,
			has_embedded_cover  INTEGER DEFAULT 0,
			-- MusicBrainz identifiers taken from the file's own tags, which
			-- is the whole point of them: they are the answer the online
			-- search would have guessed at, written by whoever tagged the
			-- file.  All four hold a UUID or nothing; see mb_uuid().
			--
			-- musicbrainz_id is the *recording*, and the two album columns are
			-- different entities that must not be confused: ALBUMID is a
			-- release, RELEASEGROUPID is the group of releases it belongs to,
			-- and getAlbumInfo2 looks up the latter. Asking /ws/2/release-group
			-- for a release id is a 404.
			musicbrainz_id                TEXT,
			musicbrainz_album_id          TEXT,
			musicbrainz_releasegroup_id   TEXT,
			musicbrainz_albumartist_id    TEXT,
			file_modified       INTEGER,
			created             DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_scanned        DATETIME
		);
		CREATE INDEX IF NOT EXISTS idx_songs_album  ON songs(album_id);
		CREATE INDEX IF NOT EXISTS idx_songs_folder ON songs(folder_id);
		CREATE INDEX IF NOT EXISTS idx_songs_genre  ON songs(genre);

		CREATE TABLE IF NOT EXISTS song_artists (
			song_id       INTEGER NOT NULL REFERENCES songs(id)   ON DELETE CASCADE,
			artist_id     INTEGER NOT NULL REFERENCES artists(id) ON DELETE CASCADE,
			role          TEXT NOT NULL DEFAULT 'artist',
			display_order INTEGER DEFAULT 0,
			PRIMARY KEY (song_id, artist_id, role)
		);
		CREATE INDEX IF NOT EXISTS idx_song_artists_artist ON song_artists(artist_id);
		CREATE INDEX IF NOT EXISTS idx_song_artists_role   ON song_artists(role);

		CREATE TABLE IF NOT EXISTS artist_info_cache (
			folder_id    INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid         TEXT NOT NULL DEFAULT '',
			last_fm_url  TEXT NOT NULL DEFAULT '',
			biography    TEXT NOT NULL DEFAULT '',
			image_url    TEXT NOT NULL DEFAULT '',
			wiki_url     TEXT NOT NULL DEFAULT '',
			allmusic_url TEXT NOT NULL DEFAULT '',
			discogs_url  TEXT NOT NULL DEFAULT '',
			fetched_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		CREATE TABLE IF NOT EXISTS album_info_cache (
			folder_id    INTEGER PRIMARY KEY REFERENCES folders(id),
			mbid         TEXT NOT NULL DEFAULT '',
			notes        TEXT NOT NULL DEFAULT '',
			wiki_url     TEXT NOT NULL DEFAULT '',
			allmusic_url TEXT NOT NULL DEFAULT '',
			fetched_at   INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- Cover art manufactured from a video file itself, by VideoArt.  Video
		-- containers carry no tag anything actually writes and these files are
		-- rarely named well, so without this every video shows a placeholder.
		-- It sits here with the other derived caches rather than being written
		-- into the library as a sidecar image: nothing gaindrive derives should
		-- land in the user's collection.
		--
		-- Keyed on the stored path, not on songs.id, for the reason stars and
		-- playlists are: a rowid is not stable across a rescan.  file_modified
		-- is what invalidates the art when the file is replaced or re-tagged.
		CREATE TABLE IF NOT EXISTS video_art (
			path          TEXT PRIMARY KEY,   -- "<root>/<rest>"
			file_modified INTEGER NOT NULL,
			mime          TEXT NOT NULL,
			source        TEXT NOT NULL,      -- embedded | frame | tmdb
			image         BLOB NOT NULL,
			created_at    INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- The artist portrait itself, rather than a URL to it.
		--
		-- artist_info_cache stores an image_url, which is a promise a third
		-- party may not keep. The bytes lived only in a std::unordered_map in
		-- the HTTP server, so every restart re-fetched every portrait from
		-- Wikimedia, TheAudioDB or Discogs — and a getCoverArt for an artist
		-- nobody had resolved yet ran the whole MusicBrainz -> Wikidata ->
		-- Wikipedia -> TheAudioDB -> Discogs chain *inside the request
		-- thread*, two one-second pacing sleeps included. An artist grid could
		-- occupy the entire HTTP pool.
		--
		-- Keyed on the artist folder's stored path, not on folders.id.
		-- artist_info_cache keys on the id and is wrong for the same reason a
		-- rowid is wrong everywhere else here; the difference is that its rows
		-- cost one lookup to rebuild and these cost a download.
		--
		-- The image is normalised to at most 800px on the long edge when it is
		-- stored, so whatever a provider sent — a PNG, a Wikimedia render of
		-- an SVG, a 4000px Discogs scan — what is kept is one predictable
		-- thing that can be scaled again without another download.
		--
		-- status records a failure as much as a success, the same reasoning as
		-- video_meta. 'none' means the providers had nothing and is not
		-- re-asked for 30 days; 'error' means the network failed, which says
		-- nothing about the artist, so it is retried at once.
		CREATE TABLE IF NOT EXISTS artist_art (
			folder_path TEXT PRIMARY KEY,     -- "<root>/<artist>"
			name        TEXT NOT NULL,        -- what the providers were asked
			status      TEXT NOT NULL,        -- ok | none | error
			source      TEXT NOT NULL DEFAULT '',
			source_url  TEXT NOT NULL DEFAULT '',
			mime        TEXT NOT NULL DEFAULT '',
			width       INTEGER NOT NULL DEFAULT 0,
			height      INTEGER NOT NULL DEFAULT 0,
			image       BLOB,
			fetched_at  INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- Scaled cover art. getCoverArt is asked for a specific pixel size by
		-- every client there is — 64, 80, 256 and 400 from the web client,
		-- 144/288/512 from Android, 144/288/800 from iOS — and before this
		-- table each of those forked ffmpeg and decoded the full-size source,
		-- on every request, for ever. A folder cover is routinely 3000x3000,
		-- so an album grid was a few hundred process launches.
		--
		-- Keyed on the stored path for the reason video_art, stars and
		-- playlists are: a rowid moves across a rescan. The key is the image
		-- file's own path for a cover or an extra image, the *media* file's
		-- path for a video_art blob (which is already what cover_path holds
		-- for one), and the *artist folder's* path for a portrait. Those
		-- three cannot collide, because a directory and a file cannot share a
		-- path and an artist folder is a directory.
		--
		-- source_stamp is deliberately not in the key. (source_key, size)
		-- being the key makes re-encoding after a cover is replaced an
		-- INSERT OR REPLACE rather than a second row, so a file edited a
		-- hundred times leaves one row per size and not a hundred.
		--
		-- `size` is quantised to a ladder before it gets here (see
		-- imagescale.hh). That is not cosmetic: size is an unvalidated client
		-- integer and this table has no eviction policy, so without the
		-- ladder any authenticated account could write a row per pixel value
		-- and fill the disk. With it the worst case is one row per rung.
		--
		-- status 'unscalable' records an image neither stb nor ffmpeg could
		-- decode, with a zero-length blob. Without it a file nothing can read
		-- would be retried on every single request for ever — a negative
		-- result is a result, the same reasoning as video_meta's 'unmatched'.
		CREATE TABLE IF NOT EXISTS cover_thumbs (
			source_key   TEXT    NOT NULL,   -- "<root>/<rest>"
			size         INTEGER NOT NULL,   -- short edge, already quantised
			source_stamp INTEGER NOT NULL,
			status       TEXT    NOT NULL,   -- ok | unscalable
			mime         TEXT    NOT NULL,
			width        INTEGER NOT NULL,
			height       INTEGER NOT NULL,
			image        BLOB    NOT NULL,
			created_at   INTEGER NOT NULL DEFAULT (strftime('%s','now')),
			PRIMARY KEY (source_key, size)
		);

		-- What an online provider was asked about a video, and what it said.
		-- The filename parser (src/videoname.hh) produces the question; TMDB
		-- answers with a poster, a plot and a canonical title.
		--
		-- `path` is the *album folder* for a film in a folder of its own, and
		-- the song for a loose file in a section: a film is a folder, so it is
		-- one question however many parts it was split into.
		--
		-- The row exists as much to record a *failure* as a success. Without
		-- it every scan would re-ask about the same unmatchable file forever,
		-- and this is the table that makes a rescan cost no traffic at all.
		-- `query` is what was asked; when the parser produces a different
		-- question — because the file was renamed — the old answer no longer
		-- applies and the lookup runs again.
		CREATE TABLE IF NOT EXISTS video_meta (
			path       TEXT PRIMARY KEY,   -- "<root>/<rest>"
			query       TEXT NOT NULL,     -- "title|year" as asked
			media_type  TEXT NOT NULL,     -- movie | tv
			tmdb_id     INTEGER,
			title       TEXT,
			year        INTEGER,
			overview    TEXT,
			-- Kept so the poster can be re-fetched without asking TMDB who
			-- this is again: art and identity expire for different reasons.
			poster_path TEXT,
			status     TEXT NOT NULL,      -- matched | unmatched | error
			-- TMDB's genres for this title, joined with '|'.  Our own
			-- encoding rather than a tag, so the separator is safe: no TMDB
			-- genre contains one.
			--
			-- NULL means "asked before this column existed" and '' means
			-- "asked, TMDB had none".  Conflating them breaks one of two
			-- ways, exactly as songs.artist documents: with no distinction
			-- either every already-matched film is re-asked on every scan for
			-- ever, or none of them ever is and the feature does nothing on
			-- an upgraded install.
			genre      TEXT,
			fetched_at INTEGER NOT NULL DEFAULT (strftime('%s','now'))
		);

		-- The song markers inside one video, as an *index*.
		--
		-- The sidecar <stem>.chapters.txt beside the video is the authority
		-- and always has been: getChapters reads that file on every call,
		-- because it is a per-playback lookup that has to be right. This table
		-- exists so *browsing* need not -- the album view lists a concert's
		-- songs from one query, and search can match a chapter title, neither
		-- of which could afford a file read (or, for a rip with no sidecar, an
		-- ffprobe) per video per request.
		--
		-- It is a cache in the strict sense DATABASE.md means: every row is
		-- re-derived from a file on disk, so deleting this database and
		-- rescanning puts all of it back. Nothing a person typed lives only
		-- here.
		--
		-- Keyed on the stored path rather than songs.id, like video_art and
		-- for the same reason: a rowid does not survive a rescan. And with no
		-- foreign key to anything, also like video_art -- album_info_cache's
		-- FK to folders(id) with no cascade is what once made DELETE FROM
		-- folders fail and roll an entire scan back.
		--
		-- Only sidecars are indexed. A container's own chapters still reach
		-- the player through getChapters, but reading them here would mean
		-- -show_chapters on every probe, which read_video_probe()'s "unusable"
		-- retry test does not cover -- so a list a narrow -probesize missed
		-- would be recorded silently as none.
		-- Every genre a song carries, in source order.
		--
		-- songs.genre survives beside this and holds the *first* of them: it
		-- is the single-valued Subsonic `genre` field, and what albums.genre
		-- rolls up from, so keeping it is what leaves the twelve ChildEntry
		-- queries untouched. This table is the full list, and it is what
		-- getGenres, getSongsByGenre and getAlbumList type=byGenre read.
		--
		-- Multi-value is not a nicety for video: a film is normally two or
		-- three genres (Alien is Horror *and* Science Fiction), and filing it
		-- under only the first is precisely the loss this exists to prevent.
		-- Audio reaches it too, from a multi-valued Vorbis GENRE.
		--
		-- Keyed on the stored path, not songs.id, for the reason chapters and
		-- video_art are: INSERT OR REPLACE reassigns a rowid, so an id-keyed
		-- row would dangle after any rescan that touched the file. No foreign
		-- key either -- album_info_cache's FK to folders(id) with no cascade
		-- is what once made DELETE FROM folders fail and roll a whole scan
		-- back.
		--
		-- It is a cache in the strict DATABASE.md sense: every row is
		-- re-derived from a file's tag or from TMDB, so deleting this
		-- database and rescanning puts all of it back.
		--
		-- Names are trimmed on write, so readers fold case alone and an
		-- ordinary NOCASE index serves rather than an expression index.
		CREATE TABLE IF NOT EXISTS song_genres (
			path TEXT    NOT NULL,   -- "<root>/<rest>"
			idx  INTEGER NOT NULL,   -- 1-based; 1 is the primary genre
			name TEXT    NOT NULL,
			PRIMARY KEY (path, idx)
		);
		CREATE INDEX IF NOT EXISTS idx_song_genres_name
			ON song_genres(name COLLATE NOCASE);

		CREATE TABLE IF NOT EXISTS chapters (
			path  TEXT    NOT NULL,   -- "<root>/<rest>", the video
			idx   INTEGER NOT NULL,   -- 1-based, in start order
			start REAL    NOT NULL,   -- seconds from the start of the file
			title TEXT    NOT NULL,   -- may be empty; the client draws its own
			PRIMARY KEY (path, idx)
		);
	)");

	// Client/user data tables (gaindrive-client.db, attached as "client" schema).
	db_music_.exec(R"(
		CREATE TABLE IF NOT EXISTS client.users (
			id             INTEGER PRIMARY KEY,
			username       TEXT NOT NULL UNIQUE,
			password_enc   TEXT NOT NULL,
			email          TEXT,
			is_admin       INTEGER DEFAULT 0,
			max_bitrate    INTEGER DEFAULT 0,
			created        DATETIME DEFAULT CURRENT_TIMESTAMP,
			last_access    DATETIME,
			upload_allowed INTEGER DEFAULT 0,
			disabled       INTEGER DEFAULT 0,
			cast_allowed   INTEGER DEFAULT 0
		);

		-- All references to music-DB rows are by filesystem path, RELATIVE
		-- form "<root name>/<rest>", never by row id. This keeps client data alive
		-- across:
		--   * a music-DB rebuild (rowids reset)
		--   * the rowid churn from upsert_song_with_data's INSERT OR REPLACE
		--   * moving the whole library to a different on-disk location
		-- The music DB stores paths in the same relative form, so cross-DB
		-- JOINs are direct equality, e.g.
		--   JOIN songs s ON s.path = st.song_path

		CREATE TABLE IF NOT EXISTS client.stars (
			user_id              INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path            TEXT,
			album_folder_path    TEXT,
			artist_folder_path   TEXT,
			created              DATETIME DEFAULT CURRENT_TIMESTAMP,
			UNIQUE(user_id, song_path, album_folder_path, artist_folder_path)
		);

		CREATE TABLE IF NOT EXISTS client.play_counts (
			user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path   TEXT NOT NULL,
			count       INTEGER DEFAULT 0,
			last_played DATETIME,
			PRIMARY KEY (user_id, song_path)
		);

		CREATE TABLE IF NOT EXISTS client.playlists (
			id        INTEGER PRIMARY KEY,
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			name      TEXT NOT NULL,
			comment   TEXT,
			is_public INTEGER DEFAULT 0,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			updated   DATETIME DEFAULT CURRENT_TIMESTAMP
		);

		CREATE TABLE IF NOT EXISTS client.playlist_songs (
			playlist_id INTEGER NOT NULL REFERENCES playlists(id) ON DELETE CASCADE,
			song_path   TEXT NOT NULL,
			position    INTEGER NOT NULL,
			PRIMARY KEY (playlist_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.play_queue (
			user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path  TEXT NOT NULL,
			position   INTEGER NOT NULL,
			is_current INTEGER DEFAULT 0,
			offset_ms  INTEGER DEFAULT 0,
			client     TEXT,
			updated    DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, position)
		);

		CREATE TABLE IF NOT EXISTS client.now_playing (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path TEXT NOT NULL,
			client    TEXT,
			started   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id)
		);

		CREATE TABLE IF NOT EXISTS client.bookmarks (
			user_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
			song_path TEXT NOT NULL,
			position  INTEGER NOT NULL DEFAULT 0,
			comment   TEXT,
			created   DATETIME DEFAULT CURRENT_TIMESTAMP,
			changed   DATETIME DEFAULT CURRENT_TIMESTAMP,
			PRIMARY KEY (user_id, song_path)
		);

		CREATE TABLE IF NOT EXISTS client.settings (
			key   TEXT PRIMARY KEY,
			value TEXT NOT NULL DEFAULT ''
		);

		-- A cover a person chose through setCoverArt.  The *image* survives a
		-- rebuild -- it is written into the album folder as cover.jpg -- but
		-- "a human picked this" is recorded nowhere on disk, and it is the one
		-- thing keeping the scan's TMDB poster tier off a hand-picked cover.
		-- So it lives here, in the DB that is not a cache, rather than in the
		-- music DB, which may be deleted and rebuilt at any time.
		--
		-- No user_id: the image is written into the library tree and everyone
		-- sees it, so the choice belongs to the library.  That makes this a
		-- global row like client.settings, not a per-user one like
		-- client.stars.
		--
		-- Keyed on the album folder's stored-form path rather than a rowid,
		-- because cover_is_manual() is asked in scan Phase 3c -- before Phase
		-- 4 has upserted the folder and so before any id for it exists.
		CREATE TABLE IF NOT EXISTS client.manual_covers (
			album_folder_path TEXT PRIMARY KEY,
			created           DATETIME DEFAULT CURRENT_TIMESTAMP
		);

		-- Metadata a person typed for a *video*, which is the one kind of file
		-- whose edit cannot be written back to the thing it describes.  The
		-- scanner reads a video's title, year and episode number from its
		-- filename and never from its tags -- read_song_metadata() returns
		-- after the ffprobe branch and never reaches TagLib -- so a tag
		-- written here would be a second copy of a fact that nothing reads.
		--
		-- Applied over the scanned values inside the album transaction, so the
		-- music DB still holds the effective title and every read query stays
		-- as it was.  NULL means "not overridden", per column: editing a title
		-- must not blank a year edited earlier.
		--
		-- Keyed on the song's stored-form path for the reason client.stars is:
		-- a rowid does not survive the rebuild this table exists to make safe.
		CREATE TABLE IF NOT EXISTS client.song_meta (
			song_path    TEXT PRIMARY KEY,
			title        TEXT,
			track_number INTEGER,
			year         INTEGER,
			disc_number  INTEGER,
			changed      DATETIME DEFAULT CURRENT_TIMESTAMP
		);
	)");

	// Seed song_genres from what songs.genre already holds, the one time the
	// table appears.
	//
	// Without this an upgraded install has an empty genre table and every
	// genre listing goes blank, because Phase 3 only opens files whose mtime
	// changed and would never re-read the other 25 000. With it nothing
	// regresses on the first scan after an upgrade, and the *multi*-value each
	// file may carry arrives per file as files change or on a rebuild -- which
	// is routine, since this database is a cache.
	//
	// Inside the same transaction as the CREATE, so a crash between the two
	// cannot leave a table that exists, is empty, and will never be seeded
	// again.
	if (song_genres_is_new)
		db_music_.exec(
			"INSERT INTO song_genres (path, idx, name)"
			" SELECT path, 1, TRIM(genre) FROM songs"
			"  WHERE TRIM(COALESCE(genre,'')) <> ''");

	txn.commit();

	// Migrations for existing databases: add columns if they don't exist yet.
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN biography TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN image_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN wiki_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN allmusic_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE album_info_cache ADD COLUMN allmusic_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE artist_info_cache ADD COLUMN discogs_url TEXT NOT NULL DEFAULT ''"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN upload_allowed INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN disabled INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE client.users ADD COLUMN cast_allowed INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN is_video INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN width INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN height INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN video_codec TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN audio_codec TEXT"); }
	catch (const SQLite::Exception&) {}
	// Deliberately no DEFAULT '', for the reason `artist` above gives: an
	// existing row must read back NULL so the Phase 3 back-fill picks it up.
	// Written together with audio_codec and never separately, which is what
	// lets one NULL check stand for both.
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN audio_container TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE folders ADD COLUMN content_type TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN cover_path TEXT"); }
	catch (const SQLite::Exception&) {}
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN season INTEGER DEFAULT 0"); }
	catch (const SQLite::Exception&) {}
	// Deliberately no DEFAULT '', unlike most of its neighbours: an existing
	// row must come out NULL, meaning "the tag was never read", so the
	// back-fill in Phase 3 picks it up.  A default would mark the whole
	// library as already read and the feature would do nothing on any upgraded
	// install, with a clean scan log saying so.
	try { db_music_.exec("ALTER TABLE songs ADD COLUMN artist TEXT"); }
	catch (const SQLite::Exception&) {}

	// No DEFAULT here either, and for a related but not identical reason.
	// These are not back-filled -- there is no narrow re-read pass for them,
	// so a row scanned before they existed keeps NULL until the file changes
	// or the music DB is rebuilt, which is routine because that DB is a cache.
	// NULL therefore means "not known", and '' would mean the same thing while
	// looking like an answer.
	for (const char* col : { "musicbrainz_album_id",
	                         "musicbrainz_releasegroup_id",
	                         "musicbrainz_albumartist_id" }) {
		try { db_music_.exec(std::string("ALTER TABLE songs ADD COLUMN ")
		                     + col + " TEXT"); }
		catch (const SQLite::Exception&) {}
		}
	try { db_music_.exec(
		"ALTER TABLE albums ADD COLUMN musicbrainz_releasegroup_id TEXT"); }
	catch (const SQLite::Exception&) {}
	// No DEFAULT, for the reason songs.artist gives above: an existing row has
	// to read back NULL so tmdb_lookup() knows it predates the column and asks
	// once more. A default would mark every already-matched film as "asked,
	// no genres" and no film in an existing library would ever get one.
	try { db_music_.exec("ALTER TABLE video_meta ADD COLUMN genre TEXT"); }
	catch (const SQLite::Exception&) {}

	// One-time: the hand-picked-cover flag used to be albums.cover_manual, in
	// the music DB -- which a rebuild would have thrown away, taking with it
	// the only thing stopping a wrong TMDB match overwriting the cover again.
	//
	// Gated on the column still existing rather than wrapped in a try: on a
	// database created after it was removed -- which is every new install, and
	// every rebuild from here on -- the SELECT is a hard error, and a catch
	// would either log that on every single start or swallow it.  There is by
	// definition nothing to carry across in that case.
	//
	// INSERT OR IGNORE makes it idempotent, so it needs no "already done"
	// marker.  It would need one the day something can *clear* a manual cover:
	// a repeat pass would resurrect the row from the stale column.
	{
	bool has_cover_manual = false;
	SQLite::Statement cols(db_music_, "PRAGMA table_info(albums)");
	while (cols.executeStep())
		if (cols.getColumn(1).getString() == "cover_manual") {
			has_cover_manual = true;
			break;
			}
	if (has_cover_manual) {
		db_music_.exec(
			"INSERT OR IGNORE INTO client.manual_covers (album_folder_path)"
			" SELECT f.path FROM albums a"
			" JOIN folders f ON f.id = a.folder_id"
			" WHERE a.cover_manual = 1");
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "Migrated " << n
			          << " hand-picked cover(s) to client.manual_covers"
			          << std::endl;
		}
	}

	// Backfill song_artists from album_artists for any songs that were scanned
	// before this link was introduced.
	db_music_.exec(
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" SELECT s.id, aa.artist_id, 'artist'"
		" FROM songs s"
		" JOIN album_artists aa ON aa.album_id = s.album_id AND aa.role = 'albumartist'"
		);

	// See MUSIC_CACHE_VERSION. A database created by this build reports 0 and
	// holds nothing, so the delete is a no-op and only the pragma is written;
	// the log line is therefore about an upgrade, and says how much work the
	// next album grid is being asked to redo.
	//
	// PRAGMA user_version takes no bound parameter, which is why the value is
	// pasted in -- it is an integer constant in this file and never anything
	// a caller supplies.
	{
	SQLite::Statement ver(db_music_, "PRAGMA user_version");
	const int have = ver.executeStep() ? ver.getColumn(0).getInt() : 0;
	if (have < MUSIC_CACHE_VERSION) {
		int thumbs = 0, infos = 0, verdicts = 0;

		if (have < 1) {
			db_music_.exec("DELETE FROM cover_thumbs");
			thumbs = db_music_.getChanges();
			}

		if (have < 2) {
			// Every row, not only the ones holding no mbid. A search that
			// picked the wrong artist -- the first hit for "Ryuichi Sakamoto"
			// is the duo "Alva Noto + Ryuichi Sakamoto" -- left a row that
			// looks perfectly successful, and it is the case the new rule
			// exists to fix.
			db_music_.exec("DELETE FROM artist_info_cache");
			infos = db_music_.getChanges();
			// Only the verdicts, never the pictures. A 'none' or an 'error'
			// is what a failed lookup recorded and is worth re-asking; an
			// 'ok' is downloaded bytes, and nothing here makes a portrait
			// that already arrived wrong.
			db_music_.exec("DELETE FROM artist_art WHERE status <> 'ok'");
			verdicts = db_music_.getChanges();
			}

		db_music_.exec("PRAGMA user_version = "
		               + std::to_string(MUSIC_CACHE_VERSION));
		if (thumbs > 0 || infos > 0 || verdicts > 0)
			std::cout << stamp() << "Cache scheme " << have << " -> "
			          << MUSIC_CACHE_VERSION << ": dropped " << thumbs
			          << " cover thumbnail(s), " << infos
			          << " artist info row(s), " << verdicts
			          << " artist portrait verdict(s)" << std::endl;
		}
	}
	}

MediaStore::ScanStatus MediaStore::scan_status() const
	{
	return { scans_active_.load() > 0, scan_items_.load() };
	}

// One phase's accumulated microseconds as seconds, to one decimal.
static std::string phase_secs(const std::atomic<long long>& us)
	{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1) << (double)us.load() / 1e6;
	return ss.str();
	}

// The breakdown, as the second line of a scan's completion message.
//
// The phases deliberately do not sum to the wall time and are not meant to:
// they are the timed parts of scan_artist_dir(), and everything between them —
// the per-song diff, the log lines, scan()'s own enumeration of level-1
// directories — is not attributed anywhere.  A large gap is itself a finding.
//
// Reported as one string so the whole breakdown reaches the log as a single
// line even when another thread is logging beside it.
std::string MediaStore::scan_times_report(double wall_s) const
	{
	std::ostringstream ss;
	ss << std::fixed << std::setprecision(1)
	   << scan_times_.files.load() << " files read ("
	   << scan_times_.videos.load() << " video), "
	   << scan_times_.albums.load() << " albums, "
	   << scan_items_.load() << " songs written in " << wall_s << "s"
	   << "\n  walk "  << phase_secs(scan_times_.walk)
	   << "  known "    << phase_secs(scan_times_.known)
	   << "  meta "     << phase_secs(scan_times_.meta)
	   << "  art "      << phase_secs(scan_times_.art)
	   << "  tmdb "     << phase_secs(scan_times_.tmdb)
	   << "  write "    << phase_secs(scan_times_.write)
	   << "  prune "    << phase_secs(scan_times_.prune);

	// Phase 3 split by what read the file.  Thread-seconds, not wall: these are
	// summed per file across the workers, so with the jobs saturated they
	// approach `meta` times the job count rather than adding up to it.  Said on
	// the line, because a reader comparing them with `meta` above would
	// otherwise be right to think one of the two numbers was wrong.
	long long videos = scan_times_.videos.load();
	long long audio  = scan_times_.files.load() - videos;
	auto per_file = [](const std::atomic<long long>& us, long long n) {
		std::ostringstream o;
		if (n > 0)
			o << std::fixed << std::setprecision(1)
			  << (double)us.load() / 1000.0 / (double)n << " ms each";
		else
			o << "no files";
		return o.str();
		};
	ss << "\n  meta thread-seconds: audio "
	   << phase_secs(scan_times_.meta_audio) << " over " << audio << " ("
	   << per_file(scan_times_.meta_audio, audio) << "), video "
	   << phase_secs(scan_times_.meta_video) << " over " << videos << " ("
	   << per_file(scan_times_.meta_video, videos) << ")";
	return ss.str();
	}

// The artist directories of an uploads root. They sit two levels deeper than a
// library root's, at <root>/<user>/<uuid>/<artist>, and scan_artist_dir()
// parents whatever it is handed straight to the root -- which is what lets the
// two layouts share every path below here.
//
// A batch still being written is skipped whole. Its contents are moving:
// apply_batch_names() renames the top two levels under a batch with a plain
// fs::rename, which is safe only for directories that have no rows yet.
static void collect_upload_artist_dirs(const fs::path& root,
                                        std::set<fs::path>& out,
                                        std::error_code& ec)
	{
	for (auto& user : fs::directory_iterator(root, ec)) {
		std::error_code uec;
		if (!user.is_directory(uec) || is_hidden_name(user.path())) continue;
		for (auto& batch : fs::directory_iterator(user.path(), uec)) {
			std::error_code bec;
			if (!batch.is_directory(bec) || is_hidden_name(batch.path()))
				continue;
			if (batch_held(batch.path())) continue;
			for (auto& artist : fs::directory_iterator(batch.path(), bec)) {
				std::error_code aec;
				if (!artist.is_directory(aec) || is_hidden_name(artist.path()))
					continue;
				out.insert(artist.path());
				}
			}
		}
	}

void MediaStore::scan()
	{
	ScanGuard guard(*this);
	auto      t0 = std::chrono::steady_clock::now();

	// The API key is a stored setting, not a start-up argument, so it is read
	// here rather than in the constructor: entering it in the client takes
	// effect on the next scan without a restart.
	tmdb_.set_api_key(get_setting("tmdb_key"));

	// The uploads root is walked like any other, two levels deeper and minus
	// the loose-file pass. What used to keep it out of here was that
	// apply_batch_names() renames with a plain fs::rename and is safe only
	// while nothing under a batch has rows; batch_held() is now the thing that
	// says so, rather than the absence of any code that could look.
	//
	// The cost is that a file dropped into somebody's upload space by hand is
	// indexed now, where before only what a producer wrote was. That is per-user
	// space and never library content, so it is the right answer there and would
	// not be under a library root.
	for (const auto& root : roots_) {
		const bool uploads = root.cfg.type == "uploads";
		std::cout << stamp() << "Scan started: " << root.cfg.name
		          << " (" << root.cfg.path << ")" << std::endl;

		// Ensure the root folder row exists before per-artist work begins.
		// Its stored path is the root *name*, which is what makes every path
		// below it self-identifying.
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		upsert_folder(fs::path(root.cfg.path), -1);
		txn.commit();
		}

		// Collect paths to scan (absolute, since scan_artist_dir needs absolute
		// for fs ops): level-1 dirs present on disk, plus any still in the DB
		// so deleted ones get pruned.
		std::set<fs::path> to_scan;
		std::error_code ec;
		if (uploads)
			collect_upload_artist_dirs(root.cfg.path, to_scan, ec);
		else {
			for (auto& e : fs::directory_iterator(root.cfg.path, ec)) {
				// Skip hidden directories. Nothing dot-prefixed at the top of
				// a library is an artist: a leftover .users from before uploads
				// got their own root, .stfolder/.stversions on a synced tree,
				// @eaDir on a Synology share. Indexing them adds junk entries
				// and churns every folders.id after them on each rescan.
				if (!e.is_directory()) continue;
				if (is_hidden_name(e.path())) continue;
				to_scan.insert(e.path());
				}
			}
		if (ec)
			std::cout << stamp() << "Scan: cannot read " << root.cfg.path
			          << ": " << ec.message() << std::endl;
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		// **Not the file-albums.** A loose file directly in a root is an album
		// of its own, so it has a level-1 folder row whose path names a file —
		// and scan_root_files() owns it.  Handed to scan_artist_dir() instead,
		// its fs::is_directory() test fails, the row is read as deleted, and
		// every root-level album is pruned on every full scan.
		//
		// The test is `folders.path` being a `songs.path`, which is
		// definitional: a folder row naming a media file is exactly a
		// file-album.  "Has an albums row" is true of one too, but it is also
		// true of a *section* on a database not yet rescanned under this rule,
		// and skipping a section here would stop it ever being pruned.
		SQLite::Statement s(db_music_,
			"SELECT path FROM folders"
			" WHERE parent_id = (SELECT id FROM folders WHERE path = ?)"
			"   AND NOT EXISTS (SELECT 1 FROM songs s WHERE s.path = folders.path)");
		s.bind(1, root.cfg.name);
		while (s.executeStep())
			to_scan.insert(fs::path(join_root(s.getColumn(0).getString())));
		}

		// The reinstatement above is by row, and a row can name an artist under
		// a batch that is held right now -- the fold takes a hold on a batch
		// scanned long ago, to move somebody else's artist into it. Dropped
		// here rather than in the query because holding is a fact about the
		// disk, which SQL cannot see.
		if (uploads)
			for (auto it = to_scan.begin(); it != to_scan.end(); )
				if (batch_held(it->parent_path())) it = to_scan.erase(it);
				else                               ++it;

		// A batch or artist directory that has gone. scan_artist_dir() below
		// prunes the music DB and every derived cache keyed on the path, and
		// has never touched the client schema -- so stars, play counts,
		// playlist entries, the queue, bookmarks, manual_covers and song_meta
		// would outlive the files, invisibly, since they are read through
		// INNER JOINs. deleteUpload does these same two steps in this same
		// order and for this same reason.
		//
		// Uploads only, because only here does a directory going missing mean
		// the files are gone for good. A library artist can be an unmounted
		// share or a drive not plugged in, and forgetting somebody's stars
		// over that would be unforgivable.
		if (uploads) {
			int gone = 0;
			for (const auto& p : to_scan) {
				if (fs::is_directory(p)) continue;
				forget_prefix(strip_root(p.string()));
				gone++;
				}
			if (gone)
				std::cout << stamp() << "Uploads: forgetting " << gone
				          << " removed director" << (gone == 1 ? "y" : "ies")
				          << std::endl;
			}

		// Process each level-1 dir in its own transaction so db_mutex_ is
		// released between them and API handlers stay responsive.
		for (auto& artist_path : to_scan)
			scan_artist_dir(artist_path);

		// Loose files directly in a root, which an uploads root never has:
		// reparent_loose_media() pushes everything a producer wrote down to
		// <artist>/<album>/file before it is ever scanned.
		if (!uploads) scan_root_files(root);
		}

	double wall = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	std::cout << stamp() << "Scan complete: " << scan_times_report(wall)
	          << std::endl;
	}

void MediaStore::scan_dirs(const std::set<std::string>& dirs)
	{
	ScanGuard guard(*this);
	auto      t0 = std::chrono::steady_clock::now();

	tmdb_.set_api_key(get_setting("tmdb_key"));

	// dirs are stored-form paths ("<root>/<level-1 dir>").  A bare root name,
	// or anything that does not resolve, means we lost track of what changed
	// (queue overflow, unknown root) and the safe answer is a full scan.
	for (auto& d : dirs) {
		std::string abs = join_root(d);
		if (abs.empty() || d.find('/') == std::string::npos) {
			scan();
			return;
			}
		}
	// Known to the DB as a folder — which is what tells a section that has just
	// been deleted apart from a file that never was one.
	auto is_known_folder = [&](const std::string& rel) {
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Statement q(db_music_, "SELECT 1 FROM folders WHERE path = ?");
		q.bind(1, rel);
		return q.executeStep();
		};
	// A folder row that names a media file: a loose file, which is its own
	// album.  Same definitional test scan() uses, and for the same reason.
	auto is_file_album = [&](const std::string& rel) {
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM folders f"
			" WHERE f.path = ?"
			"   AND EXISTS (SELECT 1 FROM songs s WHERE s.path = f.path)");
		q.bind(1, rel);
		return q.executeStep();
		};

	for (auto& d : dirs) {
		fs::path abs(join_root(d));
		// A batch a producer took between the caller naming this path and now.
		// Checked here as well as wherever the path came from, because those
		// two moments are not the same one: scan_batch() releases its hold
		// before calling in, so this never skips the scan a batch exists for.
		//
		// The parent of an uploads artist directory is its batch; for a library
		// root the parent is the root, which no one ever holds, so this costs
		// one stat and answers no.
		if (batch_held(abs.parent_path())) continue;
		// An uploads directory that has gone takes its client rows with it,
		// which scan_artist_dir() below cannot do -- see the same two steps in
		// scan(), and in deleteUpload, which is where the order comes from.
		// Harmlessly repeated when deleteUpload is the caller: it has already
		// forgotten the same prefix.
		if (!fs::is_directory(abs)) {
			const RootRec* r = root_for_rel(d);
			if (r && r->cfg.type == "uploads") forget_prefix(d);
			}
		// A depth-1 entry that is not a directory is a loose file sitting in
		// the root itself — added, changed or deleted.  The watcher reports
		// the file, since there is no directory between it and the root.
		//
		// Three cases have to be told apart, and the folder row alone cannot
		// do it now that a loose file has one.  Unknown: a file that has never
		// been scanned.  Known and a file-album: a loose file that already has
		// its own album, present or just deleted.  Both belong to
		// scan_root_files(), which walks the root and re-stamps or prunes.
		// Known and *not* a file-album: a section directory that has just been
		// deleted, which must still reach scan_artist_dir() to be pruned —
		// nothing else will ever remove it.
		if (!fs::is_directory(abs) && (!is_known_folder(d) || is_file_album(d))) {
			const RootRec* r = root_for_rel(d);
			// The uploads root is per-user space, never library content, and
			// scan() does not walk it either.
			if (r && r->cfg.type != "uploads")
				scan_root_files(*r);
			continue;
			}
		scan_artist_dir(abs);
		}

	// The same breakdown a full scan prints.  A live rescan is normally a
	// handful of files, so this is mostly noise — but it is the only way to
	// see the cost of the watcher's own work, and it is what makes an upload
	// or a URL fetch say how long its scan took rather than only that it ran.
	double wall = std::chrono::duration<double>(
		std::chrono::steady_clock::now() - t0).count();
	std::cout << stamp() << "Rescan totals: " << scan_times_report(wall)
	          << std::endl;
	}

// Per-song data collected in Phases 1–3, consumed in Phase 4.
struct SongReadData {
	std::string path;
	std::string folder_path;   // disc dir or album dir
	int64_t     mtime       = 0;
	int64_t     file_size   = 0;
	std::string codec;
	std::string cover;         // sidecar image beside the file; "" for none
	int         disc_number = 0;
	bool        changed     = false;
	bool        is_video    = false;
	// Video only, from the filename parse in Phase 1: the season an episode
	// belongs to. Stored in songs.season, copied into disc_number so clients
	// group and sort by it, and read by Phase 3c to know whether to ask TMDB
	// about a film or about a series.
	int         season      = 0;
	// The file's ARTIST tag.  Read in Phase 3 for a changed file, and also for
	// an *unchanged* file whose row has never had it read (artist_missing) --
	// that is the back-fill.  artist_read is what says the read happened, so a
	// file with no tag is stored as '' and never asked again.
	std::string artist_tag;
	bool        artist_missing = false;   // set in Phase 2
	bool        artist_read    = false;

	// The same shape again, for songs.audio_container and songs.audio_codec on
	// an *audio* row.  Every audio row is in this set, but only the extensions
	// audio_form_needs_read() names cost a file to open -- the rest are filled
	// from implied_audio_form() with no I/O at all.
	//
	// audio_form_read is what stores '' for a file whose form could not be made
	// out, which is what stops it returning on every scan for ever.
	bool        audio_form_missing = false;   // set in Phase 2
	bool        audio_form_read    = false;

	// MusicBrainz identifiers out of the file's own tags. Empty when the tag
	// is absent, unreadable, or names more than one entity — see mb_uuid().
	// These are what let getArtistInfo2 and getAlbumInfo2 skip their
	// search-by-name step, which is the half of the lookup that can silently
	// attach the wrong artist to a name collision.
	std::string mb_track_id;         // MUSICBRAINZ_TRACKID, the recording
	std::string mb_album_id;         // MUSICBRAINZ_ALBUMID, a release
	std::string mb_releasegroup_id;  // MUSICBRAINZ_RELEASEGROUPID
	std::string mb_albumartist_id;   // ALBUMARTISTID, or ARTISTID failing that
	// populated in Phase 3 only when changed == true:
	std::string title;
	int         track_nr    = 0;
	int         year        = 0;
	// The primary genre, which is what songs.genre and the Subsonic `genre`
	// field hold. The full list is `genres`, written to the song_genres table.
	std::string genre;
	// Every genre this song carries, primary first: a multi-valued GENRE tag
	// for audio, TMDB's list for video. `genres_read` is load-bearing and not
	// a convenience -- it says an answer was *obtained*, which an empty
	// `genres` cannot, and apply_song_genres() deletes a song's rows whenever
	// it is called. Without it every unchanged audio row, whose list is empty
	// only because Phase 3 never opened the file, would lose its genres on the
	// next scan. Same distinction artist_read draws, for the same reason.
	std::vector<std::string> genres;
	bool                     genres_read = false;
	double      duration    = 0.0;
	int         bitrate     = 0;
	int         sr          = 0;
	int         channels    = 0;
	// video only; all zero/empty for audio rows:
	int         width       = 0;
	int         height      = 0;
	std::string video_codec;
	// Audio *and* video: ffprobe fills it for a film's soundtrack, and
	// read_song_audio_form() / implied_audio_form() fill it for a music track.
	std::string audio_codec;
	// Audio only -- the container the file turned out to be, which is a
	// different fact from `codec` above holding its extension. See AudioForm
	// in codecs.hh.
	std::string audio_container;
	// DVD rips only: the ordered VOBs of one titleset, discovered in Phase 1
	// so Phase 3 does not have to rediscover them. `path` is the first of
	// these. Empty for everything else, which is what marks a row as ordinary.
	std::vector<std::string> parts;
	// The sidecar's markers, read in Phase 1 for the `chapters` index. Read
	// there and not in Phase 3 for the reason the filename parse is -- Phase 3
	// sees only changed files, and a sidecar is written without touching the
	// media file's mtime, so an edit would never be noticed.
	std::vector<Chapter> chapters;
	// Whether there was a sidecar, and whether it could be read. Two questions,
	// not one: `chapters` is empty for a file that has none, for the empty file
	// that is the deliberate tombstone, and for one that simply would not open,
	// and only the last of those must leave the index alone. See
	// read_sidecar_chapters() and upsert_song_with_data()'s guard.
	bool sidecar_present  = false;
	bool sidecar_readable = false;
	};

// What Phase 2 already knows about a row in the database, for the songs the
// walk just found on disk.
struct KnownSong {
	int64_t mtime       = 0;
	bool    artist_null = false;   // its ARTIST tag has never been read
	// Its form -- container and codec -- has never been read.  The is_video
	// test belongs with it for the reason it does above: a row that can never
	// acquire a value must never enter the pass, or the pass never ends and
	// the log never says why.
	bool    audio_form_null = false;
	};

struct AlbumReadData {
	std::string               path;
	std::string               title;
	std::string               cover;
	std::vector<std::string>  disc_paths;  // sorted; index+1 = disc number
	std::vector<SongReadData> songs;
	// A loose media file, which is its own album: `path` names the file rather
	// than a directory.  Carried as a flag because several steps have to know
	// not to treat that path as a directory, and re-deriving it by testing the
	// extension would answer wrongly for a directory called "Best of 1999.mp3".
	bool                      loose = false;
	// Filled by Phase 3c when TMDB identified this album, consumed by the
	// album transaction in Phase 4 — which is where the folder id, and so the
	// row to attach a description to, finally exists.
	std::string               tmdb_title;
	int                       tmdb_year = 0;
	std::string               overview;
	};

// A leading number on an audio filename — "05 - Song.flac" — is a track number
// as often as a tag is, and for a file with no tags at all it is the only one
// there is.  One regex answers both questions the prefix raises: a title
// stripped of digits that yielded no number is the two halves disagreeing
// about the same characters.
static const std::regex TRACK_PREFIX(R"(^(\d+)[. -]+)");

static std::string strip_track_prefix(const std::string& title)
	{
	return std::regex_replace(title, TRACK_PREFIX, "");
	}

// The number that prefix names, or 0 for none.  Bounded, although the strip
// deliberately is not: "2001 A Space Odyssey.mp3" already loses its leading
// 2001 from the title and must not thereby acquire track 2001.
static int track_prefix_number(const std::string& stem)
	{
	std::smatch m;
	if (!std::regex_search(stem, m, TRACK_PREFIX)) return 0;
	const std::string digits = m[1].str();
	// Bounding the length first is what keeps std::stoi from throwing, which
	// matters because a throw here would be caught somewhere far away and
	// blamed on the file rather than on its name.
	if (digits.size() > 9) return 0;
	const int n = std::stoi(digits);
	return (n >= 1 && n <= 999) ? n : 0;
	}

// The album title for a loose media file, which is its own album.  The stem,
// because the extension is not part of anyone's title, and then the same
// treatment a directory name gets one level up.
//
// A video's stem is handed over raw: apply_album_video_name() runs
// parse_video_name() over whatever is stored here, and that already reads '_'
// as a separator, so normalising it first would only hide the separator from
// the parser that knows what to do with it.
static std::string loose_album_title(const fs::path& p)
	{
	std::string t = p.stem().string();
	if (is_video_file(p)) return t;
	std::replace(t.begin(), t.end(), '_', ' ');
	return strip_track_prefix(t);
	}

// The markers beside a video, for the scan's index, and whether there was a
// sidecar there to read them from. Empty for a file with none, which is nearly
// all of them.
//
// Opened rather than tested for: fs::exists() followed by an open is two
// syscalls where one answers the same question, and this runs for every video
// on every scan. The exists() call is paid only when the open *fails*, which
// is the rare path and the one case where the two answers differ.
//
// That difference is the whole reason this returns more than a vector.
// `present && !readable` -- a sidecar that is there but could not be read --
// must never be confused with no sidecar at all, because the caller clears a
// song's rows when there are no markers to be had. Conflating the two means one
// transient unreadable moment during a scan silently deletes markers the file
// still holds, and they stay deleted until some later scan happens to read it:
// self-healing, intermittent and unlogged, which is the worst shape a bug has.
struct SidecarChapters
	{
	std::vector<Chapter> chapters;
	bool                 present  = false;   // the file is there
	bool                 readable = false;   // ...and its contents are in hand
	};

static SidecarChapters read_sidecar_chapters(const fs::path& p)
	{
	SidecarChapters   out;
	const std::string side = MediaStore::sidecar_chapters_path(p.string());

	std::ifstream f(side, std::ios::binary);
	if (!f) {
		std::error_code ec;
		out.present = fs::exists(side, ec);
		if (out.present)
			log_line("scan: cannot open chapter sidecar " + side
			         + " -- keeping the markers already indexed");
		return out;
		}
	out.present = true;

	std::string text((std::istreambuf_iterator<char>(f)),
	                  std::istreambuf_iterator<char>());
	// bad(), not fail(): reading to the end sets failbit alongside eofbit and is
	// how this loop is supposed to finish. Only badbit says the text in hand is
	// not the text in the file.
	if (f.bad()) {
		log_line("scan: cannot read chapter sidecar " + side
		         + " -- keeping the markers already indexed");
		return out;
		}
	out.readable = true;

	auto parsed = parse_chapters(text);
	if (parsed.chapters.size() > MediaStore::MAX_CHAPTERS)
		parsed.chapters.resize(MediaStore::MAX_CHAPTERS);
	out.chapters = std::move(parsed.chapters);
	return out;
	}

// One media file → everything Phase 1 can learn about it without a lock.
static SongReadData read_song_file(const fs::path& p,
                                    const std::string& folder_path,
                                    int disc_number)
	{
	SongReadData sdat;
	sdat.path        = p.string();
	sdat.folder_path = folder_path;
	sdat.mtime       = mtime_of(p);
	sdat.file_size   = static_cast<int64_t>(fs::file_size(p));
	sdat.codec       = p.extension().string().substr(1);
	std::transform(sdat.codec.begin(), sdat.codec.end(),
	               sdat.codec.begin(), ::tolower);
	sdat.cover       = find_song_cover(p);
	sdat.is_video    = is_video_file(p);
	sdat.disc_number = disc_number;

	// A video's title comes from its filename, and that parse happens here in
	// Phase 1 rather than in Phase 3 with the other metadata reads.  Two
	// reasons: it is pure string work with no I/O, so it costs nothing to do
	// for every file; and Phase 3 runs only for *changed* files, so a parse
	// living there would never reach a video already in the database.  The
	// guarded UPDATE in upsert_song_with_data() is what carries it to those.
	//
	// The folder and the one above it are passed because the title is often on
	// the folder rather than on the file — "The Third Man (1949)/title00.mkv" —
	// and because a "Season 01" folder means the show's name is one level up.
	if (sdat.is_video) {
		auto vn = resolve_video_name(
			p.stem().string(),
			p.parent_path().filename().string(),
			p.parent_path().parent_path().filename().string());
		// A name that is nothing but a year, in a folder that says no more,
		// parses to an empty title.  The raw stem is a poor title but an empty
		// one is worse, and every caller below assumes there is something.
		sdat.title    = vn.title.empty() ? p.stem().string() : vn.title;
		sdat.year     = vn.year;
		sdat.track_nr = vn.episode;
		sdat.season   = vn.season;
		// The season *is* the disc number for an episode, and saying so here
		// is what makes a series group and sort correctly. The number passed
		// in is the position of the subdirectory in sorted order, which is
		// right for the discs of an album and wrong for seasons twice over: a
		// show whose episodes sit flat in one folder has no subdirectory to
		// count, and one with ten season folders sorts "Season 10" second.
		// Only the parsed number can be trusted, and only videos have one.
		if (vn.season > 0) sdat.disc_number = vn.season;
		}
	else {
		// An untagged audio file's only track number is the one in its
		// name, and Phase 3 would be too late to look: that runs for
		// changed files alone, so a derivation living there would never
		// reach a library already scanned.  A tag still wins where there
		// is one — read_song_metadata() overwrites this only for a
		// non-zero track().
		sdat.track_nr = track_prefix_number(p.stem().string());
		}

	// Audio as well as video: a two-hour DJ set or a mixtape fetched as audio
	// wants a song list exactly as much as a concert film does. Outside the
	// branch above rather than repeated in both, and here in Phase 1 rather
	// than Phase 3 because that phase sees only files whose mtime changed --
	// and a sidecar is written without touching the media file's.
	auto side = read_sidecar_chapters(p);
	sdat.chapters         = std::move(side.chapters);
	sdat.sidecar_present  = side.present;
	sdat.sidecar_readable = side.readable;
	return sdat;
	}

// One MusicBrainz identifier out of a PropertyMap, or empty.
//
// Two rejections, both deliberate. **A multi-valued tag is no answer**:
// PropertyMap values are StringLists, and a collaboration carries an artist id
// per performer, which cannot identify the one artist a folder stands for. And
// **anything that is not shaped like a UUID is discarded**, because these reach
// a MusicBrainz URL *path* in resolve_artist_info() — checking the shape here is
// cheaper than having to think about it there, and a tag is arbitrary bytes
// somebody else wrote.
static std::string mb_uuid(const TagLib::PropertyMap& props, const char* key)
	{
	auto it = props.find(key);
	if (it == props.end() || it->second.size() != 1) return "";
	std::string v = it->second.front().toCString(true);
	// The shape check now lives in untrusted.hh, because the provider path
	// needed the same one: a search result's `id` had been trusted where a
	// file's tag was not.
	return is_uuid(v) ? v : "";
	}

// What an audio file is -- container and codec -- for songs.audio_container and
// songs.audio_codec.  Only ever called for an audio_form_needs_read()
// extension; everything else is settled by implied_audio_form().
//
// **Both halves are observed, and that is the point.**  The container is not
// taken from the filename: .ogg and .oga are one container under two
// extensions, and this function proves which container it is on its way to the
// codec, so inferring it from the name afterwards would be discarding the
// answer in order to guess at it.
//
// Codecs are spelled the way ffprobe spells them, because the same column
// already holds ffprobe's answer for every video row and one column must not
// carry two vocabularies.
//
// Ogg is read here rather than through TagLib, and that is the cheaper answer
// as well as the smaller one.  The identification header is the first packet of
// the first page, at a fixed offset once the segment table is stepped over, and
// its first bytes name the codec outright.  TagLib would reach the same answer
// by constructing one of four file objects and parsing the comment header on
// the way, and would cost four more include directories to say so.
//
// MP4 has no such shortcut -- the codec sits in a sample description atom, down
// a nested atom tree -- so that one goes through TagLib, which has already
// written the walk.
//
// A file this cannot open, or whose header says nothing recognisable, still
// counts as read: '' is an answer, and only NULL brings the file back.
static void read_song_audio_form(SongReadData& sdat)
	{
	sdat.audio_form_read = true;

	if (sdat.codec == "m4a") {
		TagLib::FileStream stream(sdat.path.c_str(), true /* readOnly */);
		TagLib::MP4::File  f(&stream, true /* readAudioProperties */);
		if (!f.isValid() || !f.audioProperties()) return;
		switch (f.audioProperties()->codec()) {
			case TagLib::MP4::Properties::AAC:  sdat.audio_codec = "aac";  break;
			case TagLib::MP4::Properties::ALAC: sdat.audio_codec = "alac"; break;
			default: return;   // Unknown: both halves '' and never asked again
			}
		sdat.audio_container = "mp4";
		return;
		}

	// Ogg.  Page header is 27 bytes, the last of which counts the segment-table
	// entries that follow it; the first packet begins after those.  Reading 255
	// + 27 + 16 covers the largest possible table and enough of the packet to
	// name any of the four.
	std::ifstream in(sdat.path, std::ios::binary);
	if (!in) return;
	char buf[298] = {0};
	in.read(buf, sizeof buf);
	const size_t got = static_cast<size_t>(in.gcount());
	if (got < 28 || std::memcmp(buf, "OggS", 4) != 0) return;
	const size_t segs = static_cast<unsigned char>(buf[26]);
	const size_t pkt  = 27 + segs;
	if (got < pkt + 8) return;
	const char* p = buf + pkt;
	if      (!std::memcmp(p, "\x01" "vorbis", 7)) sdat.audio_codec = "vorbis";
	else if (!std::memcmp(p, "OpusHead", 8))      sdat.audio_codec = "opus";
	else if (!std::memcmp(p, "Speex   ", 8))      sdat.audio_codec = "speex";
	else if (!std::memcmp(p, "\x7f" "FLAC", 5))  sdat.audio_codec = "flac";
	else return;   // an Ogg carrying something else: '' rather than a guess
	// Set only once a codec was recognised, so the pair is whole or empty and
	// never half of an answer.
	sdat.audio_container = "ogg";
	}

// How any audio row acquires its form, whether it is being read for the first
// time or back-filled years later.  One definition rather than one per phase:
// the two would drift, and the shape of the failure is a row that keeps being
// offered to the back-fill because one of them forgot to mark it read.
//
// Three outcomes, and the third is the one worth spelling out.  An extension
// with no form at all -- something TARGETS has never heard of -- is still
// *marked* read, because the flag means "an answer was obtained" and "there is
// nothing here" is an answer.  Leaving it clear would put the row in front of
// every future scan for ever.
static void fill_audio_form(SongReadData& sdat)
	{
	if (auto im = implied_audio_form(sdat.codec); !im.container.empty()) {
		sdat.audio_container = im.container;
		sdat.audio_codec     = im.codec;
		sdat.audio_form_read = true;
		}
	else if (audio_form_needs_read(sdat.codec))
		read_song_audio_form(sdat);
	else
		sdat.audio_form_read = true;
	}

// Phase 3 for one changed file: the slow reads, with no lock held.  Shared by
// scan_artist_dir() and scan_root_files().
static void read_song_metadata(SongReadData& sdat)
	{
	// A DVD titleset: dimensions and codecs are identical across the parts,
	// but the duration is not — probing only the first would report a 1 GB
	// fragment's length as the whole title, so sum them.  Title and track
	// number came from the titleset number in Phase 1 and must not be
	// overwritten by the filename-based rules below.
	if (!sdat.parts.empty()) {
		for (const auto& part : sdat.parts) {
			auto vp = read_video_probe(part);
			if (!vp) {
				log_line("scan: ffprobe failed for " + part);
				continue;
				}
			sdat.duration += vp->duration;
			if (sdat.width == 0) {
				sdat.width       = vp->width;
				sdat.height      = vp->height;
				sdat.video_codec = vp->video_codec;
				sdat.audio_codec = vp->audio_codec;
				sdat.bitrate     = vp->bitrate;
				}
			}
		return;
		}

	// Video takes a different reader entirely: TagLib cannot open these
	// containers, so ffprobe supplies duration, bitrate and dimensions.  Title,
	// year and episode number came from the filename back in Phase 1 — see
	// read_song_file().  A probe failure still leaves a usable row.
	if (sdat.is_video) {
		if (auto vp = read_video_probe(sdat.path)) {
			sdat.duration    = vp->duration;
			sdat.bitrate     = vp->bitrate;
			sdat.width       = vp->width;
			sdat.height      = vp->height;
			sdat.video_codec = vp->video_codec;
			sdat.audio_codec = vp->audio_codec;
			}
		else
			log_line("scan: ffprobe failed for " + sdat.path);
		return;
		}

	TagLib::FileStream stream(sdat.path.c_str(), true /* readOnly */);
	TagLib::FileRef    f(&stream);
	sdat.title = fs::path(sdat.path).stem().string();

	// Set even when TagLib could not open the file: '' then means "asked, and
	// there is nothing there", which is what keeps the back-fill from
	// re-opening an unreadable file on every scan for ever.
	sdat.artist_read = true;

	if (!f.isNull() && f.tag()) {
		auto* t = f.tag();
		if (!t->title().isEmpty())
			sdat.title = t->title().toCString(true);
		// Kept apart from the folder-derived artist rather than replacing it:
		// which of the two a client is shown is decided at the API boundary,
		// where both are in hand.
		if (!t->artist().isEmpty())
			sdat.artist_tag = t->artist().toCString(true);
		// Guarded, because a file with no track tag reads back as 0 and
		// would otherwise wipe the number Phase 1 took from the filename.
		if (t->track() > 0) sdat.track_nr = static_cast<int>(t->track());
		sdat.year     = static_cast<int>(t->year());
		if (!t->genre().isEmpty())
			sdat.genre = t->genre().toCString(true);
		}
	// Materialised once and read twice.  It used to sit inside the disc-number
	// guard; hoisting it costs nothing in practice, because a track outside a
	// disc subdirectory has disc_number == 0 and so already took this path —
	// which is most of a library.
	if (!f.isNull()) {
		auto props = f.file()->properties();

		// The whole GENRE tag, not just Tag::genre()'s first value. A Vorbis
		// comment may carry the field several times and an ID3v2 TCON may hold
		// a multi-value frame, and those are the container *stating* two
		// genres -- which is a different thing from a "Rock/Pop" string, where
		// the separator is a guess about someone else's intent and gaindrive
		// deliberately does not guess.
		//
		// genres_read is set whenever the file opened, empty tag included: a
		// file whose genre was deleted must lose its rows, and guarding on a
		// non-empty list would leave them behind for ever.
		sdat.genres_read = true;
		if (auto it = props.find("GENRE"); it != props.end())
			for (const auto& g : it->second) {
				std::string one = g.toCString(true);
				size_t b = one.find_first_not_of(" \t\r\n");
				size_t e = one.find_last_not_of(" \t\r\n");
				if (b != std::string::npos)
					sdat.genres.push_back(one.substr(b, e - b + 1));
				}
		// Tag::genre() and the PropertyMap can disagree -- the former reads
		// ID3v1 where the latter does not -- so the single field keeps its own
		// answer and only falls back to the list, rather than the reverse.
		if (sdat.genre.empty() && !sdat.genres.empty())
			sdat.genre = sdat.genres.front();
		// And the list falls back to the single field, so a tag only
		// Tag::genre() could read still reaches the table.
		if (sdat.genres.empty() && !sdat.genre.empty())
			sdat.genres.push_back(sdat.genre);

		if (sdat.disc_number == 0) {
			auto it = props.find("DISCNUMBER");
			if (it != props.end() && !it->second.isEmpty()) {
				try { sdat.disc_number = it->second.front().toInt(); }
				catch (...) {}
				}
			}

		sdat.mb_track_id        = mb_uuid(props, "MUSICBRAINZ_TRACKID");
		sdat.mb_album_id        = mb_uuid(props, "MUSICBRAINZ_ALBUMID");
		sdat.mb_releasegroup_id = mb_uuid(props, "MUSICBRAINZ_RELEASEGROUPID");
		// The folder an album sits in is an *album artist* folder, so that tag
		// is the one that describes it. ARTISTID is the fallback rather than
		// the first choice: on a single-artist release the two agree, and on a
		// compilation the track artist is precisely not what the folder means.
		sdat.mb_albumartist_id  = mb_uuid(props, "MUSICBRAINZ_ALBUMARTISTID");
		if (sdat.mb_albumartist_id.empty())
			sdat.mb_albumartist_id = mb_uuid(props, "MUSICBRAINZ_ARTISTID");
		}
	if (!f.isNull() && f.audioProperties()) {
		auto* ap      = f.audioProperties();
		sdat.duration = ap->lengthInSeconds();
		sdat.bitrate  = ap->bitrate();
		sdat.sr       = ap->sampleRate();
		sdat.channels = ap->channels();
		}

	// What this file is, for songs.audio_container / songs.audio_codec.  Free
	// for the extensions that settle it, and a second open for the three that
	// do not -- so a file arriving new or re-tagged never has to wait for the
	// back-fill pass to catch up with it.
	fill_audio_form(sdat);
	}

// Phase 3 for an *unchanged* file whose row predates songs.artist: read that
// one tag and nothing else.  The whole library goes through this once, so
// audio properties are switched off -- scanning an MP3 for its length is most
// of what a full read costs, and nothing here wants it.
//
// A file this cannot open still counts as read, for the reason above: '' is an
// answer, and only NULL brings the file back next scan.
static void read_song_artist_tag(SongReadData& sdat)
	{
	sdat.artist_read = true;
	TagLib::FileStream stream(sdat.path.c_str(), true /* readOnly */);
	TagLib::FileRef    f(&stream, false /* readAudioProperties */);
	if (!f.isNull() && f.tag() && !f.tag()->artist().isEmpty())
		sdat.artist_tag = f.tag()->artist().toCString(true);
	}

// What read_one_song() feeds, gathered into one reference so the call site does
// not grow a parameter per statistic.  References rather than a pointer to
// ScanTimes because that type is private to MediaStore and this is a free
// function — which it is so that both scan entry points share one definition of
// the work rather than two copies of it.
struct SongReadStats {
	std::atomic<long long>& files;
	std::atomic<long long>& videos;
	std::atomic<long long>& audio_us;
	std::atomic<long long>& video_us;
	};

// One file's whole Phase 3, factored out so scan_artist_dir() and
// scan_root_files() dispatch an identical body rather than two copies of a rule
// about which files count as read.
//
// **Runs on a parallel_for worker**, so everything it reaches has to be safe
// from several threads at once, and is: path_is_within_root() reads only
// roots_, which is written in the constructor and nowhere else; TagLib is used
// through per-call FileStream and FileRef objects and nothing in gaindrive
// registers a file-type resolver, string handler or debug listener;
// probe_video() forks through reproc, which closes inherited descriptors in the
// child, and catches its own JSON. The counters are atomic, and the log line
// goes through log_line() — a bare std::cout would splice two workers' paths
// together, and a path is the entire content of that message.
//
// The caller filters to `changed || artist_missing || audio_form_missing`, so
// an unchanged file is here for at least one back-fill but not necessarily
// both.  The second branch therefore tests the flags rather than is_video: it
// once read `else if (!sdat.is_video)`, which was only correct while
// artist_missing was the single reason an unchanged file could arrive.
static void read_one_song(const MediaStore& store, SongReadData& sdat,
                          const SongReadStats& st)
	{
	// Defence-in-depth: refuse to open any file that — after symlink
	// resolution — sits outside every root.  Deliberately outside the timing
	// below, which is meant to say what TagLib and ffprobe cost and nothing
	// else; the check is charged to `meta` as a whole like the dispatch is.
	if (!store.path_is_within_root(sdat.path)) {
		log_line("scan: skipping file outside every root: " + sdat.path);
		return;
		}

	auto t0 = std::chrono::steady_clock::now();
	if (sdat.changed) {
		read_song_metadata(sdat);
		st.files.fetch_add(1);
		if (sdat.is_video) st.videos.fetch_add(1);
		}
	// An unchanged audio file whose row predates songs.artist, or whose form
	// nobody has worked out yet: the narrow reads, once each, so an existing
	// library gains both without every file having to be touched.  Both flags
	// are set only for audio rows (Phase 2 filters on is_video), so neither
	// branch repeats that test.
	else if (sdat.artist_missing || sdat.audio_form_missing) {
		if (sdat.artist_missing) read_song_artist_tag(sdat);
		// Most of these cost nothing: an extension that settles the form is
		// answered from the name, and only .m4a and .ogg reach the reader.
		if (sdat.audio_form_missing) fill_audio_form(sdat);
		st.files.fetch_add(1);
		}
	else return;   // nothing was read, so nothing to charge

	auto us = std::chrono::duration_cast<std::chrono::microseconds>(
		std::chrono::steady_clock::now() - t0).count();
	(sdat.is_video ? st.video_us : st.audio_us).fetch_add(us);
	}

// A film's title is as often on the folder as on the file — "The.Third.Man.
// 1949.1080p.BluRay/movie.mkv" — so the album name gets the same treatment the
// song titles get.
//
// Only for albums that actually hold video.  A music album folder legitimately
// named "Album (2017)" would otherwise lose its year, and that year is not
// junk there.
//
// Guarded the same way song titles are, against the folder name as stored:
// upsert_album() is INSERT OR IGNORE, so an existing album's title is never
// rewritten by the scan today, and a title somebody edited must keep that
// property.
// A TMDB match, when there is one, supersedes the filename parse — that is the
// whole point of asking, and it is what turns "THE.THIRD.MAN.1949" into "The
// Third Man". The guard is unchanged either way.
static void apply_album_video_name(SQLite::Database& db, int album_id,
                                    const std::string& stored_title,
                                    const std::vector<SongReadData>& songs,
                                    const std::string& tmdb_title = "",
                                    int tmdb_year = 0)
	{
	if (std::none_of(songs.begin(), songs.end(),
	        [](const SongReadData& s) { return s.is_video; }))
		return;

	std::string title = tmdb_title;
	int         year  = tmdb_year;
	if (title.empty()) {
		VideoName vn = parse_video_name(stored_title);
		if (!vn.cleaned || vn.title.empty()) return;
		title = vn.title;
		year  = vn.year;
		}
	if (title == stored_title) return;

	SQLite::Statement upd(db,
		"UPDATE albums SET title = ?,"
		"                  year = CASE WHEN year = 0 THEN ? ELSE year END"
		" WHERE id = ? AND title = ?");
	upd.bind(1, title);
	upd.bind(2, year);
	upd.bind(3, album_id);
	upd.bind(4, stored_title);
	upd.exec();
	}

// A film's plot goes where an album's liner notes go, which is why this needed
// no new endpoint and no web-client change: getAlbumInfo2 already reads this
// table and the album view already renders `notes`.
//
// ON CONFLICT rather than INSERT OR REPLACE so a row a MusicBrainz lookup
// already created keeps its other columns. Called from inside the album
// transaction, so it takes the database directly — cache_album_info() would
// deadlock, taking db_mutex_ a second time on a non-recursive mutex.
static void apply_album_overview(SQLite::Database& db, int folder_id,
                                  const std::string& overview)
	{
	if (overview.empty()) return;
	SQLite::Statement ins(db,
		"INSERT INTO album_info_cache (folder_id, notes) VALUES (?, ?)"
		" ON CONFLICT(folder_id) DO UPDATE SET notes = excluded.notes");
	ins.bind(1, folder_id);
	ins.bind(2, overview);
	ins.exec();
	}

// A season is parsed, everything else is positional, and the two numberings
// share one column — so in an album that has both they can collide. "Extras"
// sorts before "Season 1" and takes disc number 1 from it, merging a season
// into a bonus folder; the same happens to a loose film sitting beside an
// episode, since a song with no disc folder is written as disc 1.
//
// Anything with no season in an album that has one is therefore renumbered
// above the highest season. That also sorts it last, which is where a specials
// or extras folder belongs. Albums with no season at all — every music album,
// and every film split across discs — are left completely alone.
//
// Runs after Phase 3, because the disc number 0 it reads is also what makes
// read_song_metadata() consult an audio file's DISCNUMBER tag.
static void renumber_unseasoned(std::vector<SongReadData>& songs)
	{
	int max_season = 0;
	for (const auto& s : songs) max_season = std::max(max_season, s.season);
	if (max_season == 0) return;

	// Keyed on the old number so a whole folder moves together, and assigned
	// in the order the songs come, which is the sorted disc-folder order.
	std::map<int, int> renumbered;
	for (auto& s : songs) {
		if (s.season > 0) continue;
		auto [it, inserted] = renumbered.try_emplace(s.disc_number, 0);
		if (inserted) it->second = ++max_season;
		s.disc_number = it->second;
		}
	}

// Phase 3b for one album's worth of files: manufacture cover art for the videos
// that have none.  Returns the path whose art should also serve as the album's,
// or "" — chosen by sort order rather than by re-running find_cover(), whose
// pass 3 takes whatever directory_iterator hands it first, so a season folder
// would otherwise get a different episode's frame on different machines.
//
// It runs for *unchanged* songs too.  The guard is "has no cover and has no
// cached image", not sdat.changed, so switching the feature on back-fills an
// existing library on the next scan without anything having to be re-tagged or
// touched — the unchanged-song UPDATE writes cover_path unconditionally for
// exactly this class of reason.
//
// The image is written immediately rather than carried into Phase 4: a season
// of two dozen episodes would otherwise hold a couple of megabytes of JPEG in
// SongReadData for the whole artist.  Nothing in Phase 4 depends on it; what
// Phase 4 needs is the cover_path this leaves behind.
static std::string make_video_art_songs(
	MediaStore& store, const VideoArt& art, std::vector<SongReadData>& songs,
	const std::unordered_map<std::string, int64_t>& known)
	{
	std::string album_art;

	for (auto& sdat : songs) {
		// A sidecar image beside the file always wins — this tier exists only
		// for the files that have nothing at all.
		if (!sdat.is_video || !sdat.cover.empty()) continue;

		std::string rel = store.rel_path(sdat.path);
		auto        it  = known.find(rel);
		if (it == known.end() || it->second != sdat.mtime) {
			// A DVD titleset is one stream split across VOBs, so the whole
			// concat: list is the input — seeking into the first part alone
			// would land inside a 1 GB fragment rather than inside the film.
			std::string input;
			if (!sdat.parts.empty()) input = dvd_input(sdat.parts.front());

			auto result = art.generate(sdat.path, input);
			if (!result) {
				std::cout << stamp() << "video art: nothing usable in " << rel
				          << std::endl;
				continue;
				}
			store.store_video_art(rel, sdat.mtime, result->mime,
			                      result->source, result->bytes);
			std::cout << stamp() << "video art: " << result->source << ", "
			          << result->bytes.size() << " bytes for " << rel
			          << std::endl;
			}

		// cover_path names the media file itself, which is what makes every
		// existing cover-art query and the whole web client work unchanged.
		// See MediaStore::VideoArtRow.
		sdat.cover = sdat.path;
		if (album_art.empty() || sdat.path < album_art) album_art = sdat.path;
		}

	return album_art;
	}

// TMDB's genre list, to and from the single video_meta column that holds it.
//
// '|' rather than a comma because this is *our* encoding of a list, not a tag
// somebody else wrote, so the separator can simply be one no value contains --
// and no TMDB genre does, while several ("Action & Adventure") would make a
// comma or an ampersand ambiguous. This is the whole reason the music side
// refuses to split a `Rock/Pop` tag and this one may split freely: there the
// separator is a guess about someone else's intent, here it is a fact about a
// string we wrote ourselves.
//
// Values are trimmed and empties dropped on the way out, so the table only
// ever holds usable names and the readers fold case alone.
static std::string join_genres(const std::vector<std::string>& genres)
	{
	std::string out;
	for (const auto& g : genres) {
		if (g.empty()) continue;
		if (!out.empty()) out += '|';
		out += g;
		}
	return out;
	}

static std::vector<std::string> split_genres(const std::string& joined)
	{
	std::vector<std::string> out;
	size_t pos = 0;
	while (pos <= joined.size()) {
		size_t next = joined.find('|', pos);
		if (next == std::string::npos) next = joined.size();
		std::string one = joined.substr(pos, next - pos);
		size_t b = one.find_first_not_of(" \t\r\n");
		size_t e = one.find_last_not_of(" \t\r\n");
		if (b != std::string::npos) out.push_back(one.substr(b, e - b + 1));
		if (next == joined.size()) break;
		pos = next + 1;
		}
	return out;
	}

// The whole of Phase 3b for one artist.  Shared with scan_root_files(), which
// has no AlbumReadData and calls make_video_art_songs() directly.
static void make_video_art(MediaStore& store, const VideoArt& art,
                           std::vector<AlbumReadData>& albums,
                           const std::string& prefix)
	{
	auto known = store.load_video_art_keys(prefix);
	for (auto& adat : albums) {
		std::string album_art = make_video_art_songs(store, art, adat.songs,
		                                              known);
		// An album that already has an image keeps it: a hand-placed poster,
		// or one uploaded through setCoverArt, outranks a frame grab.
		if (adat.cover.empty() && !album_art.empty()) adat.cover = album_art;
		}
	}

// ---- Phase 3c: identify the video online ------------------------------

// An 'error' row is a network failure, which is temporary by nature. An
// 'unmatched' one is a judgement about the name, and re-asking tomorrow would
// get the same answer — only a rename changes it, and a rename changes the
// stored query, which re-asks anyway.
static constexpr int64_t TMDB_ERROR_RETRY_S = 24 * 60 * 60;

// What was asked, stored so a rename can be detected as a different question.
static std::string tmdb_query_key(const std::string& title, int year, bool tv)
	{
	return title + "|" + std::to_string(year) + "|" + (tv ? "tv" : "movie");
	}

// One lookup: consults the cache, asks TMDB only when it has to, and records
// the outcome either way. Returns the row to act on, matched or not.
static MediaStore::VideoMetaRow tmdb_lookup(
	MediaStore& store, const Tmdb& tmdb, const std::string& rel_key,
	const std::string& title, int year, bool tv, int explicit_id)
	{
	std::string query = tmdb_query_key(title, year, tv);

	if (auto cached = store.get_video_meta(rel_key)) {
		bool stale = cached->status == "error"
		    && std::time(nullptr) - cached->fetched_at > TMDB_ERROR_RETRY_S;
		// A matched row written before the genre column existed is owed one
		// more question, once. This is the *only* thing that re-asks about a
		// title the cache has already answered, and it terminates because the
		// answer is stored even when it is empty — genre_known, not a
		// non-empty genre, is the test. Without it the feature would do
		// nothing at all on an existing library, which is the failure mode
		// the songs.artist back-fill exists to avoid.
		bool wants_genre = cached->status == "matched" && !cached->genre_known;
		if (cached->query == query && !stale && !wants_genre) return *cached;
		}

	MediaStore::VideoMetaRow row;
	row.query      = query;
	row.media_type = tv ? "tv" : "movie";

	std::optional<TmdbMatch> m;
	if (explicit_id > 0) m = tmdb.by_id(explicit_id, tv);
	else                 m = tmdb.search(title, year, tv);

	if (m) {
		row.status      = "matched";
		row.tmdb_id     = m->id;
		row.title       = m->title;
		row.year        = m->year;
		row.overview    = m->overview;
		row.poster_path = m->poster_path;
		row.genre       = join_genres(m->genres);
		row.genre_known = true;
		std::cout << stamp() << "tmdb: " << rel_key << " -> " << m->title
		          << " (" << m->year << ") id=" << m->id << std::endl;
		}
	else {
		// A network failure and a considered rejection are recorded
		// differently because they expire differently. There is no way to
		// tell them apart from here, so anything with no key configured or no
		// answer at all counts as an error and will be retried.
		row.status = tmdb.configured() ? "unmatched" : "error";
		std::cout << stamp() << "tmdb: no match for " << rel_key
		          << " (\"" << title << "\""
		          << (year ? " " + std::to_string(year) : "") << ")"
		          << std::endl;
		}

	store.store_video_meta(rel_key, row);
	return row;
	}

// Fetches the poster for a matched row and returns true when art from this
// tier or better exists afterwards. Keeping the poster keyed on a *song* path
// is what lets getCoverArt stay untouched: it resolves cover_path -> video_art
// exactly as it does for an embedded cover.
//
// The skip is on the *source*, not on there being a row at all. A row left by
// a local tier has to be replaced — the poster outranks it, which is the whole
// point — and store_video_art() is INSERT OR REPLACE, so it is. Only a poster
// already fetched for this file stops the download, which is what makes a
// rescan cost no traffic.
static bool tmdb_fetch_poster(MediaStore& store, const Tmdb& tmdb,
                               const MediaStore::VideoMetaRow& row,
                               const std::string& rel_song, int64_t mtime)
	{
	if (row.poster_path.empty()) return false;
	if (store.get_video_art_source(rel_song) == "tmdb") return true;

	auto bytes = tmdb.poster(row.poster_path);
	if (!bytes) return false;
	store.store_video_art(rel_song, mtime, "image/jpeg", "tmdb", *bytes);
	std::cout << stamp() << "tmdb: poster " << bytes->size() << " bytes for "
	          << rel_song << std::endl;
	return true;
	}

// Phase 3c for one artist or root.
//
// **One lookup per album, not per file, with no exception left.** A film is a
// folder, so "Movies/The Third Man (1949)/" is one question however many parts
// the film was split into — and a loose file is an album of its own now, so
// "each loose file in a section is its own work" says the same thing rather
// than contradicting it. The per-file branch this replaced was the only reason
// this function needed to know which album was the section's.
//
// Unifying them is also what gives a loose film the cover_is_manual() check
// the per-file branch deliberately skipped: setCoverArt used to be able to
// reach only the section, so there was no per-film choice to respect. There
// is now, and it is the only remedy for a wrong match.
//
// **The poster outranks whatever local art the scan found**, which is the one
// place video inverts the rule the rest of gaindrive follows. For an album a
// folder image is the album's own art and nothing should displace it; for a
// film it is usually whatever a downloader happened to leave in the directory,
// and the TMDB poster is plainly better. A film nothing matched keeps its
// local image, so this only ever replaces art for a file we can name.
//
// The exception is a cover a person uploaded through setCoverArt, which
// client.manual_covers marks. That upload is also the way to fix a wrong
// match, so putting the wrong poster back on the next scan would take away the
// only remedy the client offers. It is recorded in the client DB rather than
// beside the album because the music DB is a cache: a rebuild would otherwise
// throw the choice away and let the wrong poster win all over again.
static void lookup_video_meta(MediaStore& store, const Tmdb& tmdb,
                              std::vector<AlbumReadData>& albums)
	{
	for (auto& adat : albums) {
		// The first video in sort order: the one whose path carries the album's
		// art, matching what Phase 3b picks for the same reason.
		const SongReadData* first = nullptr;
		bool                is_tv = false;
		for (const auto& s : adat.songs) {
			if (!s.is_video) continue;
			if (s.season > 0) is_tv = true;
			if (!first || s.path < first->path) first = &s;
			}
		if (!first) continue;

		{
			VideoName vn = parse_video_name(adat.title);
			// **A file-album asks the question its *song* already resolved**,
			// not the one this album title parses to. read_song_file() runs
			// resolve_video_name() over the stem *with the folder and the one
			// above it*, so for an uninformative name it has an answer this
			// parse of the bare stem does not — and, more importantly, it is
			// the same string the per-file lookup this replaced composed. The
			// stored `query` is what decides whether a cached answer still
			// applies, so asking differently would re-ask TMDB about every
			// loose film in the library to arrive back where it started.
			//
			// The explicit [tmdbid=] marker is still read from the stem: it is
			// not part of tmdb_query_key(), so it invalidates nothing, and the
			// per-file lookup never supported it.
			std::string title = adat.loose ? first->title
			                  : (vn.title.empty() ? adat.title : vn.title);
			int         year  = adat.loose ? first->year : vn.year;
			int explicit_id = 0;
			if (!vn.tmdb_id.empty()) { try { explicit_id = std::stoi(vn.tmdb_id); }
			                           catch (...) {} }

			// The key stays adat.path, which for a file-album is the media
			// file's own path — the very key the per-file lookup used, so
			// every cached match and every cached rejection survives.
			auto row = tmdb_lookup(store, tmdb, store.rel_path(adat.path),
			                        title, year, is_tv, explicit_id);
			if (row.status != "matched") continue;

			adat.tmdb_title = row.title;
			adat.tmdb_year  = row.year;
			adat.overview   = row.overview;

			// A loose file *is* its album, so the answer is about the track as
			// much as about the album and both carry it. A directory album is
			// the opposite case and must not do this: stamping a film's title
			// on every row would rename each episode of a season after the
			// show.
			if (adat.loose && !row.title.empty())
				for (auto& sdat : adat.songs) {
					if (!sdat.is_video) continue;
					sdat.title = row.title;
					if (row.year > 0) sdat.year = row.year;
					}

			// A film is one work however many parts or episodes it is split
			// into, so its genres belong to every video in the folder. Audio
			// in the same folder is left alone: none of this reasoning applies
			// to it, and a concert's soundtrack keeps whatever its tags say.
			//
			// Set for every video rather than only the changed ones, which is
			// what back-fills a library scanned before this existed -- the
			// same reason the video title is re-derived unconditionally.
			if (std::vector<std::string> gs = split_genres(row.genre);
			        !gs.empty())
				for (auto& sdat : adat.songs) {
					if (!sdat.is_video) continue;
					sdat.genres      = gs;
					sdat.genre       = gs.front();
					sdat.genres_read = true;
					}
			// The title and the plot apply either way; only the cover is held
			// back for a hand-uploaded one.
			if (!store.cover_is_manual(store.rel_path(adat.path))
			        && tmdb_fetch_poster(store, tmdb, row,
			                              store.rel_path(first->path),
			                              first->mtime)) {
				adat.cover = first->path;
				// Drop the videos' own sidecar images (film.jpg beside
				// film.mkv), so they fall through to the album's cover, which
				// is now the poster. Without this a matched film shows the
				// poster in the album grid and the sidecar on the video entry
				// — the same film with two covers, which is worse than either
				// image winning outright. Audio in the folder keeps its art:
				// none of this reasoning applies to it.
				for (auto& sdat : adat.songs)
					if (sdat.is_video) sdat.cover.clear();
				}
			}
		}
	}

// Set one of an album's MusicBrainz ids from its songs, but only when every
// tagged track agrees.
//
// Derived in SQL from the stored song columns rather than in C++ from
// SongReadData, and that is the whole reason it is correct: Phase 3 only reads
// files whose mtime changed, so on an ordinary rescan the in-memory values are
// empty for most of the album while the columns are still there. Reading the
// rows means the answer does not depend on which files this particular scan
// happened to open.
//
// The predicate is "one distinct value, and it is on every track that has one".
// A folder holding two releases, or one whose tags are half filled in with
// different ids, therefore keeps NULL and keeps the online lookup — which is
// the right way round: a wrong id is worse than no id, because nothing
// downstream can tell it is wrong.
//
// upsert_album() is INSERT OR IGNORE, so it cannot carry this: after the first
// scan its insert is silently ignored and the value would never land. Hence an
// UPDATE, the same shape upsert_song_with_data() uses for track_number.
//
// Caller must hold db_mutex_ and an open transaction.
static void apply_album_mbid(SQLite::Database& db, int album_id,
                              const char* album_col, const char* song_col)
	{
	// Spliced rather than bound, because these are column *names* and SQLite
	// binds values only. Safe because both come from the two call sites below
	// as string literals; nothing here is reachable from a request.
	const std::string c   = song_col;
	const std::string sql =
		"UPDATE albums SET " + std::string(album_col) + " = ("
		"  SELECT s." + c + " FROM songs s"
		"   WHERE s.album_id = ?1"
		"     AND s." + c + " IS NOT NULL AND s." + c + " <> ''"
		"   GROUP BY s." + c +
		"   HAVING COUNT(*) = (SELECT COUNT(*) FROM songs t"
		"                       WHERE t.album_id = ?1"
		"                         AND t." + c + " IS NOT NULL"
		"                         AND t." + c + " <> '')"
		") WHERE id = ?1";
	SQLite::Statement upd(db, sql);
	upd.bind(1, album_id);
	upd.exec();
	}

// The same rule as apply_album_mbid(), one level up: an artist's MBID is set
// only when every tagged track under it agrees.
//
// Joined through album_artists rather than through the folder tree, because
// that link is what an artist row actually means here. A compilation folder
// disagrees by construction — that is what a compilation is — so "Various
// Artists" ends up NULL and keeps the online lookup, which is right.
//
// Runs after the album loop rather than in the artist transaction above it:
// that transaction commits before any song of this artist has been written, so
// there would be nothing to read.
//
// Caller must hold db_mutex_ and an open transaction.
static void apply_artist_mbid(SQLite::Database& db, int artist_id)
	{
	SQLite::Statement upd(db,
		"UPDATE artists SET musicbrainz_id = ("
		"  SELECT s.musicbrainz_albumartist_id FROM songs s"
		"    JOIN album_artists aa ON aa.album_id = s.album_id"
		"                         AND aa.role = 'albumartist'"
		"   WHERE aa.artist_id = ?1"
		"     AND s.musicbrainz_albumartist_id IS NOT NULL"
		"     AND s.musicbrainz_albumartist_id <> ''"
		"   GROUP BY s.musicbrainz_albumartist_id"
		"   HAVING COUNT(*) = (SELECT COUNT(*) FROM songs t"
		"                        JOIN album_artists aa2 ON aa2.album_id = t.album_id"
		"                                              AND aa2.role = 'albumartist'"
		"                       WHERE aa2.artist_id = ?1"
		"                         AND t.musicbrainz_albumartist_id IS NOT NULL"
		"                         AND t.musicbrainz_albumartist_id <> '')"
		") WHERE id = ?1");
	upd.bind(1, artist_id);
	upd.exec();
	}

// Re-assert whatever a person typed for the videos of one album, over the
// values this scan just derived from their filenames.
//
// This is what keeps the music DB a cache. A video's edit cannot go back into
// the file — the scanner never reads a video's tags — so it lives in
// client.song_meta, and running it here means the *music* DB still holds the
// effective title. Deleting the music DB and rescanning therefore reproduces
// it, and no read query has to know the override exists.
//
// COALESCE per column, because a NULL in client.song_meta means "not
// overridden": someone who fixed a title must not thereby pin the year.
//
// Caller must hold db_mutex_ and an open transaction.
static void apply_song_meta_overrides(SQLite::Database& db, int album_id)
	{
	SQLite::Statement q(db,
		"UPDATE songs SET"
		"  title = COALESCE("
		"    (SELECT m.title FROM client.song_meta m"
		"      WHERE m.song_path = songs.path), title),"
		"  track_number = COALESCE("
		"    (SELECT m.track_number FROM client.song_meta m"
		"      WHERE m.song_path = songs.path), track_number),"
		"  year = COALESCE("
		"    (SELECT m.year FROM client.song_meta m"
		"      WHERE m.song_path = songs.path), year),"
		"  disc_number = COALESCE("
		"    (SELECT m.disc_number FROM client.song_meta m"
		"      WHERE m.song_path = songs.path), disc_number)"
		" WHERE album_id = ?"
		"   AND EXISTS (SELECT 1 FROM client.song_meta m"
		"                WHERE m.song_path = songs.path)");
	q.bind(1, album_id);
	q.exec();
	}

// Writes one song row; all slow I/O has already happened.
// Caller must hold db_mutex_ and an open transaction. sdat.path is absolute
// (Phase 1/3 use it for TagLib I/O); rel_path is its stored form, which the
// caller computes because strip_root() is a member and this is not.
// The chapter index for one video, rewritten wholesale.
//
// Delete-then-insert rather than an upsert, because a list that got *shorter*
// would otherwise keep its tail for ever -- the rows past the new end match no
// incoming idx and nothing else would reach them.
// A song's genres, replaced wholesale. Same shape as apply_song_chapters()
// below and for the same reasons.
//
// Called only when the caller actually has an answer for this file --
// SongReadData::genres_read -- which is what keeps an unchanged audio row,
// whose in-memory genre list is empty because Phase 3 never opened the file,
// from having its rows deleted. That guard is why this needs no equivalent of
// load_chapter_keys(): chapters are read for every file in Phase 1, so that
// pass had to discover cheaply which files were worth touching, while this one
// is only ever handed the changed audio and the matched video.
static void apply_song_genres(SQLite::Database& db,
                               const std::string& rel_path,
                               const std::vector<std::string>& genres)
	{
	SQLite::Statement del(db, "DELETE FROM song_genres WHERE path = ?");
	del.bind(1, rel_path);
	del.exec();
	if (genres.empty()) return;

	SQLite::Statement ins(db,
		"INSERT OR IGNORE INTO song_genres (path, idx, name)"
		" VALUES (?, ?, ?)");
	int idx = 0;
	for (const auto& g : genres) {
		if (g.empty()) continue;
		ins.reset();
		ins.bind(1, rel_path);
		ins.bind(2, ++idx);
		ins.bind(3, g);
		ins.exec();
		}
	}

static void apply_song_chapters(SQLite::Database& db,
                                 const std::string& rel_path,
                                 const std::vector<Chapter>& chapters)
	{
	SQLite::Statement del(db, "DELETE FROM chapters WHERE path = ?");
	del.bind(1, rel_path);
	del.exec();
	if (chapters.empty()) return;

	SQLite::Statement ins(db,
		"INSERT INTO chapters (path, idx, start, title) VALUES (?, ?, ?, ?)");
	for (size_t i = 0; i < chapters.size(); i++) {
		ins.reset();
		ins.bind(1, rel_path);
		ins.bind(2, static_cast<int>(i + 1));
		ins.bind(3, chapters[i].start);
		ins.bind(4, chapters[i].name);
		ins.exec();
		}
	}

static void upsert_song_with_data(SQLite::Database& db, const SongReadData& sdat,
                                   int album_id, int folder_id, int artist_id,
                                   const std::string& rel_path,
                                   const std::string& rel_cover,
                                   const std::set<std::string>& chapter_keys)
	{
	// Before the changed/unchanged split, deliberately, because it belongs to
	// neither: sdat.changed compares the *media file's* mtime, and a sidecar is
	// written without touching that, so a chapter edit is invisible to it.
	//
	// Touched only when the file has markers now or had them before. The
	// alternative is a DELETE per song per scan on the off-chance, which for a
	// library of 25k audio files is 25k prepared write statements inside the
	// album transactions to discover that almost none of them has any.
	// `chapter_keys` is one query per artist, the same bargain
	// load_video_art_keys() strikes.
	//
	// A sidecar that is there but would not open writes nothing at all. The
	// line below clears the index when it finds no markers, and for an
	// unreadable file "no markers" is not an answer but the absence of one --
	// acting on it deletes what the file still holds, and the next scan that
	// can read the file is the only thing that would put them back.
	const bool sidecar_lost = sdat.sidecar_present && !sdat.sidecar_readable;
	const bool had_rows     = chapter_keys.count(rel_path) > 0;

	if (!sidecar_lost && (!sdat.chapters.empty() || had_rows)) {
		// Clearing rows that existed earns a line of its own: it is how a
		// deleted sidecar and an emptied one take effect, and it is also what a
		// chapter list disappearing from an album listing looks like from in
		// here. When that gets reported, the log should already say whether it
		// happened, to which file, and which of the two it was.
		if (sdat.chapters.empty() && had_rows)
			log_line("scan: clearing indexed chapters of " + rel_path
			         + (sdat.sidecar_present ? " -- its sidecar is now empty"
			                                 : " -- it has no sidecar"));
		apply_song_chapters(db, rel_path, sdat.chapters);
		}

	// Also before the split, and for a related but distinct reason: a video's
	// genres come from Phase 3c rather than from the file, so they arrive for
	// an unchanged row too. The guard is genres_read and never `!empty()` --
	// see SongReadData::genres_read.
	if (sdat.genres_read)
		apply_song_genres(db, rel_path, sdat.genres);

	if (!sdat.changed) {
		// An unchanged song still has to say it was seen: the per-album prune
		// below deletes whatever is left marked unvisited.  Disc number, season
		// and sidecar cover come from the folder layout and the filename, not
		// from the file's contents, so they can change while the file itself
		// does not — and a library scanned before any of them existed is
		// back-filled here rather than needing every file touched.
		//
		// **Which album a song belongs to is the same kind of fact**, and it
		// is written here for the same reason.  A loose file is its own album
		// now; before that it belonged to one album on the whole section, so
		// on an upgrade every loose row in the library has to move — and none
		// of those files has changed, so this is the only branch that will
		// ever see them.  Omitting it leaves the songs on the old section
		// album and every new file-album empty, on a library that reports a
		// clean scan.
		//
		// The track number is the same kind of thing, and is back-filled only
		// into a row that has none: unlike the video title below, a tag is the
		// authority here and the row is already holding it, so only an untagged
		// file has nothing to lose.  A number someone typed survives anyway —
		// apply_song_meta_overrides() re-asserts client.song_meta over this in
		// the same transaction.
		SQLite::Statement upd(db,
			"UPDATE songs SET last_scanned = CURRENT_TIMESTAMP,"
			"                 disc_number = CASE WHEN ? > 0 THEN ? ELSE disc_number END,"
			"                 track_number = CASE WHEN COALESCE(track_number, 0) = 0"
			"                                     AND ? > 0"
			"                                THEN ? ELSE track_number END,"
			"                 season = ?,"
			"                 cover_path = ?,"
			"                 album_id = ?,"
			"                 folder_id = ?"
			" WHERE path = ?");
		upd.bind(1, sdat.disc_number);
		upd.bind(2, sdat.disc_number);
		upd.bind(3, sdat.track_nr);
		upd.bind(4, sdat.track_nr);
		upd.bind(5, sdat.season);
		upd.bind(6, rel_cover);
		upd.bind(7, album_id);
		upd.bind(8, folder_id);
		upd.bind(9, rel_path);
		upd.exec();

		// The back-fill: songs.artist is NULL on every row scanned before the
		// column existed, and those files have not changed, so Phase 3 would
		// never re-read them.  Written whenever the read happened, empty tag
		// included — guarding this on a non-empty tag would leave an untagged
		// file NULL and put it back in the pass on every scan for ever.
		if (sdat.artist_read) {
			SQLite::Statement a(db,
				"UPDATE songs SET artist = ? WHERE path = ?");
			a.bind(1, sdat.artist_tag);
			a.bind(2, rel_path);
			a.exec();
			}

		// The same back-fill for songs.audio_codec, which every audio row has
		// held NULL since the column arrived with video.  Unguarded for the
		// same reason: a container whose codec could not be made out stores ''
		// and is done with, where a guard would put it back in the pass for
		// ever.
		if (sdat.audio_form_read) {
			SQLite::Statement a(db,
				"UPDATE songs SET audio_container = ?, audio_codec = ?"
				" WHERE path = ?");
			a.bind(1, sdat.audio_container);
			a.bind(2, sdat.audio_codec);
			a.bind(3, rel_path);
			a.exec();
			}

		// A video's title is derived from its filename, so it can improve
		// without the file changing — which is how a library scanned before
		// the parser existed gets back-filled.  Unguarded, unlike the version
		// this replaced: a title a person typed is no longer in this row's
		// past, it is in client.song_meta, and apply_song_meta_overrides()
		// puts it back in the same transaction a few lines later.  Guarding it
		// now would only stop the parser back-filling a title nobody typed.
		if (sdat.is_video && !sdat.title.empty()) {
			SQLite::Statement t(db,
				"UPDATE songs SET title = ?,"
				"                 year = CASE WHEN year = 0 THEN ? ELSE year END,"
				"                 track_number = CASE WHEN ? > 0 THEN ?"
				"                                ELSE track_number END,"
				// Guarded on being non-empty, unlike the title beside it: a
				// film TMDB could not identify has no genre to offer and must
				// not wipe one an earlier scan found.
				"                 genre = CASE WHEN ? <> '' THEN ? ELSE genre END"
				" WHERE path = ?");
			t.bind(1, sdat.title);
			t.bind(2, sdat.year);
			t.bind(3, sdat.track_nr);
			t.bind(4, sdat.track_nr);
			t.bind(5, sdat.genre);
			t.bind(6, sdat.genre);
			t.bind(7, rel_path);
			t.exec();
			}
		return;
		}

	// Audio only.  A video title has already been through the filename parser,
	// which takes a leading number as an episode number without touching the
	// title — applying this to it as well would turn "12 Angry Men" into
	// "Angry Men".
	std::string title = sdat.is_video
	    ? sdat.title : strip_track_prefix(sdat.title);

	// `artist` sits at the end of the list rather than beside `genre` where it
	// belongs by subject: the binds below are hand-numbered and positional, so
	// inserting a column mid-list means renumbering fifteen of them — an edit
	// that neither fails to compile nor throws when it goes wrong, it just
	// writes bitrate into duration.  The order of an INSERT's column list
	// carries no meaning, so this costs nothing.
	SQLite::Statement ins(db,
		"INSERT OR REPLACE INTO songs"
		" (album_id, folder_id, path, filename, title, track_number, disc_number,"
		"  year, genre, duration, bitrate, sample_rate, channels, codec,"
		"  file_size, file_modified, is_video, width, height, video_codec,"
		"  audio_codec, season, cover_path, artist,"
		"  musicbrainz_id, musicbrainz_album_id, musicbrainz_releasegroup_id,"
		"  musicbrainz_albumartist_id, audio_container, last_scanned)"
		" VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,"
		"         CURRENT_TIMESTAMP)");
	ins.bind(1,  album_id);
	ins.bind(2,  folder_id);
	ins.bind(3,  rel_path);
	ins.bind(4,  fs::path(sdat.path).filename().string());
	ins.bind(5,  title);
	ins.bind(6,  sdat.track_nr);
	ins.bind(7,  sdat.disc_number > 0 ? sdat.disc_number : 1);
	ins.bind(8,  sdat.year);
	ins.bind(9,  sdat.genre);
	ins.bind(10, sdat.duration);
	ins.bind(11, sdat.bitrate);
	ins.bind(12, sdat.sr);
	ins.bind(13, sdat.channels);
	ins.bind(14, sdat.codec);
	ins.bind(15, sdat.file_size);
	ins.bind(16, sdat.mtime);
	ins.bind(17, sdat.is_video ? 1 : 0);
	ins.bind(18, sdat.width);
	ins.bind(19, sdat.height);
	ins.bind(20, sdat.video_codec);
	ins.bind(21, sdat.audio_codec);
	ins.bind(22, sdat.season);
	ins.bind(23, rel_cover);
	// Bound even when empty, and for video, where it is always empty: '' is
	// "read, no tag", and a NULL here would put the row into the back-fill
	// pass on every future scan.
	ins.bind(24, sdat.artist_tag);
	// Appended after `artist` rather than placed beside musicbrainz_id's
	// neighbours in the schema, for the reason given above: the binds are
	// positional and hand-numbered, so a column added mid-list renumbers
	// everything after it — an edit that compiles, does not throw, and writes
	// the wrong field. Bound even when empty, so a re-tagged file that lost
	// its ids does not keep the old ones.
	ins.bind(25, sdat.mb_track_id);
	ins.bind(26, sdat.mb_album_id);
	ins.bind(27, sdat.mb_releasegroup_id);
	ins.bind(28, sdat.mb_albumartist_id);
	// Appended for the reason the four above were, and bound even when empty:
	// '' is "read, nothing recognised", and a NULL would put the row back in
	// the form pass on every future scan.
	ins.bind(29, sdat.audio_container);
	ins.exec();

	int song_id = static_cast<int>(db.getLastInsertRowid());
	SQLite::Statement lnk(db,
		"INSERT OR IGNORE INTO song_artists (song_id, artist_id, role)"
		" VALUES (?, ?, 'artist')");
	lnk.bind(1, song_id);
	lnk.bind(2, artist_id);
	lnk.exec();
	}

// Targeted rescan of one artist directory.  Same mark→walk→prune pattern as
// scan(), but scoped to a single artist subtree so most of the library is
// untouched.
//
// Structured in four phases so slow filesystem I/O never holds db_mutex_:
//   1. Walk disk — collect album/song paths, mtimes, file sizes (no lock)
//   2. Brief read lock — fetch known mtimes to identify changed files
//   3. TagLib (audio) or ffprobe (video) reads for changed files only (no lock)
//   4. Short write txns: one per album, plus the unvisited-mark and the
//      artist-level prune.  Per-album commits keep the mutex hold time
//      bounded so REST handlers stay responsive during the scan.
void MediaStore::commit_album(const AlbumReadData& adat, int parent_folder_id,
                               int artist_id,
                               const std::set<std::string>& chapter_keys)
	{
	PhaseTimer pt(scan_times_.write);
		{
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);

		// Every album gets its own folder row under the folder above it,
		// including a loose file's — whose row names the file itself, and
		// so needs the title passed in rather than derived from
		// "film.mp4".  What this replaced special-cased the loose album,
		// whose folder *was* the artist folder and could not be upserted
		// with itself as its parent.
		int album_folder_id = upsert_folder(fs::path(adat.path),
		                                     parent_folder_id,
		                                     adat.loose ? adat.title : "");
		int album_id        = upsert_album(album_folder_id, adat.title, artist_id, 0, "");
		apply_album_video_name(db_music_, album_id, adat.title, adat.songs,
		                        adat.tmdb_title, adat.tmdb_year);
		apply_album_overview(db_music_, album_folder_id, adat.overview);

		if (!adat.cover.empty()) {
			SQLite::Statement upd(db_music_,
				"UPDATE albums SET cover_path = ? WHERE id = ?");
			upd.bind(1, strip_root(adat.cover));
			upd.bind(2, album_id);
			upd.exec();
			}

		// Build path→folder_id map for this album's disc dirs.
		std::unordered_map<std::string, int> fid_map;
		fid_map[adat.path] = album_folder_id;
		int disc_count = (int)adat.disc_paths.size();
		for (int dn = 0; dn < disc_count; ++dn) {
			int disc_fid = upsert_folder(fs::path(adat.disc_paths[dn]), album_folder_id);
			fid_map[adat.disc_paths[dn]] = disc_fid;
			}

		// Songs get the same mark→sweep the folders get, scoped to exactly
		// this album's folders, because the caller's folder-level prune
		// cannot reach a track deleted from an album that still exists.
		//
		// It is an id list rather than "parent_id = album_folder_id"
		// because a disc folder is a child and an album's other children
		// are not its own.  That mattered more under the rule this
		// replaced, where the loose-file album's children were the
		// section's *other* albums; a file-album has no children at all.
		std::string fid_list;
		for (auto& [_, fid] : fid_map) {
			if (!fid_list.empty()) fid_list += ",";
			fid_list += std::to_string(fid);
			}
		db_music_.exec(("UPDATE songs SET last_scanned = NULL"
		                " WHERE folder_id IN (" + fid_list + ")").c_str());

		for (auto& sdat : adat.songs) {
			auto fit = fid_map.find(sdat.folder_path);
			int  fid = (fit != fid_map.end()) ? fit->second : album_folder_id;
			upsert_song_with_data(db_music_, sdat, album_id, fid, artist_id,
			                       strip_root(sdat.path),
			                       sdat.cover.empty() ? "" : strip_root(sdat.cover),
			                       chapter_keys);
			// getScanStatus's `count`.  Counted here rather than after the
			// commit below so the number moves while a large album is
			// still being written; an album whose transaction then fails
			// leaves it a little high, which is the right way round for a
			// progress figure.
			scan_items_.fetch_add(1);
			}

		db_music_.exec(("DELETE FROM songs WHERE last_scanned IS NULL"
		                " AND folder_id IN (" + fid_list + ")").c_str());
		if (int n = db_music_.getChanges(); n > 0)
			std::cout << stamp() << "  pruned " << n << " songs from "
			          << fs::path(adat.path).filename().string() << std::endl;

		// Before the album's year is aggregated below, so a year a person
		// typed on a track is the one the album inherits.
		apply_song_meta_overrides(db_music_, album_id);

		// The two ids are different entities and both are wanted: the
		// release is what a client means by an album's MBID, the release
		// group is what getAlbumInfo2 can actually look up.
		apply_album_mbid(db_music_, album_id, "musicbrainz_id",
		                  "musicbrainz_album_id");
		apply_album_mbid(db_music_, album_id, "musicbrainz_releasegroup_id",
		                  "musicbrainz_releasegroup_id");

		if (disc_count > 1) {
			SQLite::Statement upd(db_music_, "UPDATE albums SET disc_count=? WHERE id=?");
			upd.bind(1, disc_count);
			upd.bind(2, album_id);
			upd.exec();
			}
		{
		SQLite::Statement upd(db_music_,
			"UPDATE albums SET year = ("
			"  SELECT year FROM songs WHERE album_id = ? AND year > 0"
			"  ORDER BY disc_number, track_number LIMIT 1"
			") WHERE id = ?");
		upd.bind(1, album_id);
		upd.bind(2, album_id);
		upd.exec();
		}
		{
		// The album's genre is the commonest one among its tracks.
		//
		// upsert_album() could not carry this: it is INSERT OR IGNORE, so
		// after the first scan its insert is silently ignored and the
		// value would never land -- the same trap apply_album_mbid() and
		// track_number document. Both call sites in fact pass "", so
		// albums.genre was empty for every album ever scanned and
		// getAlbumList type=byGenre -- which filters on it -- could match
		// nothing but the empty string.
		//
		// Commonest rather than apply_album_mbid()'s "one distinct value
		// on every track": disagreement there means a wrong MBID and is
		// worth refusing, while an album whose tracks are nine Rock and
		// one Pop is a Rock album, not an ungenred one. Grouped on the
		// folded spelling so "Rock" and "rock" count once, and MIN() picks
		// a representative from within a group whose members differ only
		// in case and padding. Ordering by the fold after the count makes
		// a tie deterministic rather than whatever the query plan left.
		SQLite::Statement upd(db_music_,
			"UPDATE albums SET genre = ("
			"  SELECT MIN(TRIM(s.genre)) FROM songs s"
			"   WHERE s.album_id = ?1 AND TRIM(COALESCE(s.genre,'')) <> ''"
			"   GROUP BY LOWER(TRIM(s.genre))"
			"   ORDER BY COUNT(*) DESC, LOWER(TRIM(s.genre))"
			"   LIMIT 1"
			") WHERE id = ?1");
		upd.bind(1, album_id);
		upd.exec();
		}

		txn.commit();
		scan_times_.albums.fetch_add(1);
		}
	}

void MediaStore::scan_artist_dir(const fs::path& artist_path)
	{
	// SQL `path LIKE ?` queries below compare against the stored-form column,
	// so the prefix must be stored-form too (e.g. "music/Artist Name/%").
	std::string prefix =
		strip_root(artist_path.string()) + "/%";
	bool exists = fs::is_directory(artist_path);

	// The owning root, needed to re-create the root folder row below. A path
	// outside every root cannot be scanned — refuse rather than inventing one.
	const RootRec* root = root_for_abs(artist_path.string());
	if (!root) {
		std::cout << stamp() << "Rescan: ignoring path outside every root: "
		          << artist_path << std::endl;
		return;
		}
	const std::string& root_path = root->cfg.path;

	std::cout << stamp() << "Rescan: " << artist_path.filename().string()
	          << (exists ? "" : " (removed)") << std::endl;

	// ---- Phase 1: walk disk (no lock) ----
	std::vector<AlbumReadData> albums;
	// A candidate disc subdirectory, with the media it was found to hold.
	struct DiscDir { fs::path path; std::vector<fs::path> files; };
	if (exists) {
		PhaseTimer pt(scan_times_.walk);
		for (auto& album_entry : fs::directory_iterator(artist_path)) {
			if (!album_entry.is_directory()) continue;
			if (is_hidden_name(album_entry.path())) continue;

			AlbumReadData adat;
			adat.path  = album_entry.path().string();
			adat.title = album_entry.path().filename().string();
			std::replace(adat.title.begin(), adat.title.end(), '_', ' ');
			adat.cover = find_cover(album_entry.path());

			// A DVD rip is one track per titleset and nothing else.  This has
			// to short-circuit the normal enumeration below: otherwise
			// VIDEO_TS becomes a disc subdirectory and every menu VOB becomes
			// a track of its own.
			if (auto video_ts = dvd_video_ts_dir(album_entry.path());
			        !video_ts.empty()) {
				for (auto& [ts, vobs] : dvd_titlesets(video_ts)) {
					int64_t total = 0;
					int64_t newest = 0;
					std::error_code fec;
					for (auto& v : vobs) {
						total  += static_cast<int64_t>(fs::file_size(v, fec));
						newest  = std::max(newest, mtime_of(v));
						}
					// A few seconds of DVD video is reliably an FBI warning or
					// a studio logo, never something worth a row.
					if (total < 10 * 1024 * 1024) continue;

					SongReadData sdat;
					sdat.path        = vobs.front().string();
					sdat.folder_path = adat.path;   // the album, not VIDEO_TS
					sdat.mtime       = newest;
					sdat.file_size   = total;
					sdat.codec       = "vob";
					sdat.is_video    = true;
					sdat.disc_number = 0;
					// DVD titles carry no names, only numbers.
					sdat.title       = "Title " + std::to_string(ts);
					sdat.track_nr    = ts;
					// This branch builds its own SongReadData and never calls
					// read_song_file(), so anything Phase 1 learns there has
					// to be repeated here -- the gap sdat.cover already has.
					// A concert DVD is exactly the thing someone marks up.
					auto side = read_sidecar_chapters(vobs.front());
					sdat.chapters         = std::move(side.chapters);
					sdat.sidecar_present  = side.present;
					sdat.sidecar_readable = side.readable;
					for (auto& v : vobs) sdat.parts.push_back(v.string());
					adat.songs.push_back(std::move(sdat));
					}
				albums.push_back(std::move(adat));
				continue;
				}

			// A subdirectory is a disc only if it holds media of its own.  An
			// album's Artwork or Scans folder is not disc 1, and counting it
			// pushes every real disc up by one -- which is what a client is told
			// the disc number is, so the headings and the sort order both go
			// wrong.  The media is collected here rather than in the numbering
			// loop below so each directory is still enumerated exactly once.
			std::vector<DiscDir>  disc_dirs;
			std::vector<fs::path> direct_files;
			// is_hidden_name() on both levels: this is where a zip unpacked on
			// macOS puts its "._Track01.mp3" forks, one beside every track, and
			// is_media_file() cannot tell them apart from the tracks.  A
			// __MACOSX subfolder needs no test of its own -- with its forks
			// filtered it holds no media, and the empty() test below drops it.
			for (auto& e : fs::directory_iterator(album_entry.path())) {
				if (is_hidden_name(e.path())) continue;
				if (e.is_regular_file()) {
					if (is_media_file(e.path())) direct_files.push_back(e.path());
					continue;
					}
				if (!e.is_directory()) continue;
				DiscDir dd;
				dd.path = e.path();
				for (auto& te : fs::directory_iterator(e.path())) {
					if (te.is_regular_file() && is_media_file(te.path())
					        && !is_hidden_name(te.path()))
						dd.files.push_back(te.path());
					}
				if (!dd.files.empty()) disc_dirs.push_back(std::move(dd));
				}
			std::sort(disc_dirs.begin(), disc_dirs.end(),
				[](const DiscDir& a, const DiscDir& b) {
					return a.path.filename() < b.path.filename();
					});

			for (auto& dd : disc_dirs)
				adat.disc_paths.push_back(dd.path.string());

			int disc_count = (int)disc_dirs.size();
			for (int dn = 0; dn < disc_count; ++dn) {
				for (auto& f : disc_dirs[dn].files)
					adat.songs.push_back(read_song_file(
						f, disc_dirs[dn].path.string(), dn + 1));
				}
			for (auto& p : direct_files)
				adat.songs.push_back(read_song_file(p, adat.path, 0));

			albums.push_back(std::move(adat));
			}

		// Media files sitting directly in the artist/section folder, with no
		// folder of their own — one documentary, one home video, one clip.
		// **Each is an album of its own, holding that one track**, whose
		// `folders` row is the media file's own path.  A file can never
		// collide with a directory, so folders.path stays UNIQUE and
		// albums.folder_id needs no schema change; the section itself is left
		// as a pure parent with no albums row.
		//
		// What this replaced was Subsonic's and Airsonic's rule — a folder
		// that directly contains media is itself an album — under which every
		// loose file in a section collapsed into one album named after the
		// section.  That made the section its own album *and* the parent of
		// the albums below it, which is where the parenting, prune and
		// flattening special cases all came from, and it left a loose film
		// with no id of its own to star, move or set art on.  setCoverArt is
		// folder-level, so a wrongly matched film could not be given the right
		// poster at all — and setCoverArt is the only remedy for a bad TMDB
		// match.
		for (auto& e : fs::directory_iterator(artist_path)) {
			if (!e.is_regular_file() || !is_media_file(e.path())) continue;
			// "._movie.mp4" is an AppleDouble resource fork, not a film.
			if (is_hidden_name(e.path())) continue;

			AlbumReadData loose;
			loose.loose = true;
			loose.path  = e.path().string();
			loose.title = loose_album_title(e.path());
			// The file's own sidecar image, never find_cover(): a folder cover
			// belongs to the whole section, and find_cover()'s
			// directory_iterator is uncaught, so handing it a file would throw
			// out of Phase 1.
			loose.cover = find_song_cover(e.path());
			loose.songs.push_back(read_song_file(e.path(), loose.path, 0));
			albums.push_back(std::move(loose));
			}
		}

	// ---- Phase 2: brief read lock — identify changed files ----
	// Keyed by the same absolute-path form as sdat.path so the per-song lookup
	// below stays a direct comparison. The DB column is relative; compose
	// absolute via join_root() on the way in.
	std::unordered_map<std::string, KnownSong> known;
	{
	// The lock wait is inside the timing on purpose: queueing behind an API
	// thread is as much a cost of this phase as the query itself.
	PhaseTimer pt(scan_times_.known);
	std::lock_guard<std::mutex> lock(db_mutex_);
	// is_video is asked for here rather than tested in C++ below: a video is
	// never opened with TagLib, so its artist column stays NULL for ever, and
	// without this it would be offered to the back-fill on every scan, skipped,
	// and leave nothing in the log to say why the pass never ends.
	// audio_codec rides along for the second back-fill: an audio row whose form
	// -- container and codec -- has never been read.  Every audio row qualifies,
	// not only the ambiguous containers: most are answered from the extension
	// with no file opened, and leaving them NULL would put them in front of the
	// pass on every scan for ever.  The is_video test is doing the same job it
	// does for `artist`.
	SQLite::Statement q(db_music_,
		"SELECT path, file_modified, artist, is_video, audio_codec"
		" FROM songs WHERE path LIKE ?");
	q.bind(1, prefix);
	while (q.executeStep())
		known[join_root(q.getColumn(0).getString())] =
			{ q.getColumn(1).getInt64(),
			  q.getColumn(2).isNull() && q.getColumn(3).getInt() == 0,
			  q.getColumn(4).isNull() && q.getColumn(3).getInt() == 0 };
	}

	for (auto& adat : albums)
		for (auto& sdat : adat.songs) {
			auto it = known.find(sdat.path);
			sdat.changed = (it == known.end() || it->second.mtime != sdat.mtime);
			sdat.artist_missing = (it != known.end() && it->second.artist_null);
			sdat.audio_form_missing =
				(it != known.end() && it->second.audio_form_null);
			}

	// Which songs under this artist already have chapter rows. One query, so
	// that Phase 4 can leave the other 25,000 alone -- see the note over
	// upsert_song_with_data()'s call to apply_song_chapters().
	const std::set<std::string> chapter_keys = load_chapter_keys(prefix);

	// ---- Phase 3c: identify films and series online (no lock) ----
	// Only under a categories root. A concert or a music video sitting under a
	// performer stays local: both layouts are L1/L2/files and the scanner
	// cannot tell a concert film from a documentary by shape, which is the
	// same reason is_category_folder() exists — pointed the other way.
	//
	// **Started here, before Phase 3, and joined after it.**  This is the only
	// phase whose cost is not the disk's — 250 ms of deliberate pacing plus a
	// round trip, twice per album — so it is the only one that overlapping can
	// hide, and it measured 213 s of a 1660 s scan spent waiting on a socket
	// with the disk idle.
	//
	// It reads what Phase 1 produced and writes adat.tmdb_title, tmdb_year,
	// overview and cover, plus title/year/cover on the loose branch's songs.
	// Phase 3 writes duration, bitrate, dimensions and codecs. The two sets are
	// disjoint — but only because read_song_metadata() returns before its
	// TagLib block for a video, and 3c touches nothing else. **Teaching the
	// video branch to read a title out of the container would break this
	// silently**, so it is said here rather than left to be rediscovered.
	//
	// Not overlapped when Phase 3b is on, and that is not only about the race.
	// 3b and 3c both write sdat.cover, and each skips work the other has
	// already done — so running 3c first does not merely reorder two writes, it
	// changes which tier ends up owning the art on an album holding more than
	// one video. Both tiers are off by default, so the common case overlaps and
	// the configured case keeps exactly the behaviour it had.
	const bool art_on   = video_art_ && video_art_->enabled();
	const bool want_tmdb = tmdb_.configured() && root->cfg.type == "categories";

	std::thread        tmdb_thread;
	std::exception_ptr tmdb_err;
	// Joins however this function leaves, because Phase 3 can throw and a
	// std::thread destroyed while still joinable is std::terminate — a dead
	// server rather than a failed scan.
	struct Joiner {
		std::thread& t;
		~Joiner() { if (t.joinable()) t.join(); }
		} tmdb_joiner{tmdb_thread};

	if (want_tmdb && !art_on)
		tmdb_thread = std::thread([&]{
			// Caught rather than allowed to escape: this is a thread's
			// top-level function, so a contended store_video_meta() would be
			// std::terminate instead of the "Scan aborted" it is today.
			try { lookup_video_meta(*this, tmdb_, albums); }
			catch (...) { tmdb_err = std::current_exception(); }
			});

	// ---- Phase 3: TagLib reads (scan_jobs_ wide, no lock) ----
	// Changed files get the full read.  An unchanged one is opened only to
	// back-fill an ARTIST tag its row has never held, which happens once per
	// file over the life of the library.
	//
	// This is the phase a cold scan is made of — measured at 80% of it — and
	// what it costs is seeks: the open, the tag at the head of the file and the
	// ID3v1/APE trailer at the tail, two or three of them per file with nothing
	// else in flight.  Run one at a time that is a queue one deep, which is the
	// one thing a spinning disk cannot make up for.
	//
	// Flattened to pointers rather than dispatched per album, because an artist
	// is a few albums of a dozen files each and a per-album dispatch would
	// leave every thread but one idle on the last album.  The pointers are
	// stable for the whole phase: **nothing appends to `albums`, or to any
	// adat.songs, between Phase 1 and Phase 4** — that is the invariant this
	// rests on.
	{
	PhaseTimer pt(scan_times_.meta);
	std::vector<SongReadData*> work;
	for (auto& adat : albums)
		for (auto& sdat : adat.songs)
			if (sdat.changed || sdat.artist_missing
			        || sdat.audio_form_missing) work.push_back(&sdat);

	SongReadStats stats{ scan_times_.files,      scan_times_.videos,
	                      scan_times_.meta_audio, scan_times_.meta_video };
	parallel_for(work.size(), scan_jobs_, [&](std::size_t i) {
		read_one_song(*this, *work[i], stats);
		});
	}

	if (tmdb_thread.joinable()) {
		// Timed at the join rather than around the thread's own life, so the
		// figure means "how much of the online tier failed to hide behind the
		// disk". Near zero is the healthy reading; close to the whole scan
		// means Phase 3 had nothing left to do.
		PhaseTimer pt(scan_times_.tmdb);
		tmdb_thread.join();
		}
	if (tmdb_err) std::rethrow_exception(tmdb_err);

	for (auto& adat : albums) renumber_unseasoned(adat.songs);

	// ---- Phase 3b: cover art for videos that have none (no lock) ----
	// enabled(), not just video_art_: with every tier off this phase would
	// still query video_art and ffprobe every video that has no cover, on
	// every scan, to be told each time that there is nothing to take.
	if (video_art_ && video_art_->enabled()) {
		PhaseTimer pt(scan_times_.art);
		make_video_art(*this, *video_art_, albums, prefix);
		}

	// Phase 3c's serial path: reached only when Phase 3b is on, so the two keep
	// the order they have always had. See the note above Phase 3.
	if (want_tmdb && art_on) {
		PhaseTimer pt(scan_times_.tmdb);
		lookup_video_meta(*this, tmdb_, albums);
		}

	// ---- Phase 4: short write txns ----
	// Mark this artist's subfolders as unvisited.  Folders not re-stamped
	// by an album commit below are pruned at the end of the artist.
	{
	// Counted as prune rather than write: it is the mark half of the mark and
	// sweep the prune below is the other half of, and the two share the LIKE
	// that makes them cost what they cost.
	PhaseTimer pt(scan_times_.prune);
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	SQLite::Statement s(db_music_,
		"UPDATE folders SET last_scanned = NULL WHERE path LIKE ?");
	s.bind(1, prefix);
	s.exec();
	txn.commit();
	}

	// Set when an album's commit failed, which makes the prune unsafe: the
	// prune deletes whatever is still marked unvisited, and a failed album is
	// indistinguishable from a deleted one by that mark alone.
	bool album_failed = false;

	if (exists) {
		int root_id, artist_folder_id, artist_id;
		{
		PhaseTimer pt(scan_times_.write);
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		root_id          = upsert_folder(fs::path(root_path), -1);
		artist_folder_id = upsert_folder(artist_path, root_id);
		artist_id        = upsert_artist(artist_path.filename().string());
		txn.commit();
		}

		for (auto& adat : albums) {
			// One album's transaction failing must not cost the rest of the
			// scan.  A full scan of a large library is minutes of work, and
			// the usual cause here is an external writer holding the lock for
			// longer than the busy timeout — momentary, and specific to this
			// commit.  Note the prune at the end of this function is then
			// skipped: a failed album leaves its folder row still marked
			// unvisited, and pruning on incomplete information would delete an
			// album that is present on disk.
			try {
			commit_album(adat, artist_folder_id, artist_id, chapter_keys);
			std::cout << stamp() << "  " << fs::path(adat.path).filename().string() << std::endl;
			}
			catch (const std::exception& e) {
				std::cout << stamp() << "  " << fs::path(adat.path).filename().string()
				          << ": skipped, " << e.what() << std::endl;
				album_failed = true;
				}
			}

		// Now that every album of this artist has been committed, the
		// tag-derived artist id can be settled: it reads the song rows the
		// loop above wrote, which is why it cannot live in the artist
		// transaction further up — that commits before any of them exist.
		//
		// Its own transaction rather than the prune's, because the prune is
		// skipped after a failed album and this is unaffected by that; and its
		// own try, because an artist without an id is a lookup that still
		// works, which is not worth abandoning a scan over.
		try {
			PhaseTimer pt(scan_times_.write);
			std::lock_guard<std::mutex> lock(db_mutex_);
			SQLite::Transaction txn(db_music_);
			apply_artist_mbid(db_music_, artist_id);
			txn.commit();
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "  artist MusicBrainz id not set: "
			          << e.what() << std::endl;
			}
		}

	// Prune stale entries within this artist's subtree.  Anything still
	// last_scanned IS NULL after the album commits above is genuinely gone —
	// but only if every album actually committed.  After a failure the mark
	// cannot distinguish "deleted from disk" from "we could not write it", so
	// pruning would remove an album that is still there.  Leave the subtree
	// alone and let the next scan sort it out.
	if (album_failed) {
		std::cout << stamp() << "  prune skipped: an album failed to commit"
		          << std::endl;
		return;
		}

	// One artist's prune failing must not take the scan with it. It is a
	// single transaction, so a failure part-way puts this subtree back exactly
	// as it was — including rows this pass has already logged as pruned — and
	// the next scan retries it. What must not happen is the exception escaping
	// into scan(), which abandons every root after this one: a stale folder is
	// a wrong listing, an abandoned scan is a library that stops updating.
	try {
	PhaseTimer pt(scan_times_.prune);
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM songs WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " songs" << std::endl;
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM albums WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " albums" << std::endl;
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM album_info_cache WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM artist_info_cache WHERE folder_id IN ("
		"  SELECT id FROM folders WHERE last_scanned IS NULL AND path LIKE ?"
		")");
	s.bind(1, prefix);
	s.exec();
	}
	{
	// Scaled art belonging to the folders about to go. This has to run
	// *before* the folder delete, because it identifies its rows through the
	// same unvisited marks.
	//
	// The reference test is the folder, not somebody's cover_path. An *extra*
	// image (index=1..n) is found by find_extra_images() at request time and
	// recorded in no column at all, so a cover_path test would delete every
	// extra's thumbnails on every scan. An orphan sweep run afterwards does
	// not work either: the artist folder survives and covers every path
	// beneath it, so a deleted album's thumbnails would look reachable for
	// ever. The leading source_key LIKE is what keeps the correlated EXISTS
	// bounded to this artist. Both LIKEs treat _ as a wildcard, which here can
	// only ever keep a row.
	SQLite::Statement s(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key LIKE ?"
		"  AND EXISTS (SELECT 1 FROM folders f"
		"    WHERE f.last_scanned IS NULL AND f.path LIKE ?"
		"      AND (cover_thumbs.source_key = f.path"
		"           OR cover_thumbs.source_key LIKE f.path || '/%'))");
	s.bind(1, prefix);
	s.bind(2, prefix);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM folders WHERE last_scanned IS NULL AND path LIKE ?");
	s.bind(1, prefix);
	s.exec();
	int n = db_music_.getChanges();
	if (n > 0)
		std::cout << stamp() << "  pruned " << n << " folders" << std::endl;
	}
	// **The section folder is a parent and nothing else**, since a loose file
	// is its own album now.  So no song may point at it and it may not carry
	// an albums row, and both deletes are unconditional rather than guarded on
	// anything this scan observed.
	//
	// They are the whole of the upgrade path, and they cannot be reached by
	// the prefix prune above: the mark is `path LIKE '<section>/%'`, which by
	// construction does not match `<section>` itself.  A library scanned under
	// the old rule has one album on every section holding loose files, and its
	// songs have already been re-pointed to their own file-albums by the
	// unchanged branch of upsert_song_with_data() a few lines above — so what
	// is left here is an empty album row and, for any loose file deleted from
	// disk since, its orphaned song.
	std::string artist_rel = strip_root(artist_path.string());
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM songs"
		" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
	s.bind(1, artist_rel);
	s.exec();
	if (int n = db_music_.getChanges(); n > 0)
		std::cout << stamp() << "  pruned " << n << " loose songs" << std::endl;
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM albums"
		" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
	s.bind(1, artist_rel);
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM artists WHERE id NOT IN"
		" (SELECT DISTINCT artist_id FROM album_artists)");
	s.exec();
	}
	if (!exists) {
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM artist_info_cache"
			" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, artist_rel);
		s.exec();
		}
		{
		// And the *album* info cache on the same folder, which is not the
		// same row and not reachable by the prefix prune above. A folder that
		// directly holds media is its own album, so a loose-file section's
		// notes — a film's TMDB plot, or whatever getAlbumInfo cached — are
		// keyed on the section folder itself, and "<section>/%" never matches
		// it. Both caches reference folders(id) with no cascade, so leaving
		// this row behind made the DELETE below fail with FOREIGN KEY
		// constraint failed: the whole prune rolled back, the section came
		// back complete with the song this pass had already reported pruning,
		// and the exception took the rest of the scan with it.
		SQLite::Statement s(db_music_,
			"DELETE FROM album_info_cache"
			" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, artist_rel);
		s.exec();
		}
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM folders WHERE path = ?");
		s.bind(1, artist_rel);
		s.exec();
		if (db_music_.getChanges() > 0)
			std::cout << stamp() << "  pruned artist folder" << std::endl;
		}
		// Both caches are keyed by path, and the two prunes below can only see
		// paths *under* this folder — "video/Road/%" does not match
		// "video/Road". Nothing writes a row on a section folder today, since
		// the loose-file album is looked up per song, but a row that did land
		// there would otherwise outlive the folder with nothing able to reach
		// it again.
		{
		SQLite::Statement s(db_music_, "DELETE FROM video_art WHERE path = ?");
		s.bind(1, artist_rel);
		s.exec();
		}
		{
		SQLite::Statement s(db_music_, "DELETE FROM video_meta WHERE path = ?");
		s.bind(1, artist_rel);
		s.exec();
		}
		{
		// The artist's own portrait, and anything scaled from a loose file
		// sitting directly in this folder. Keyed on the folder itself, which
		// is exactly what the prefix delete above cannot reach: "video/Road/%"
		// does not match "video/Road".
		SQLite::Statement s(db_music_,
			"DELETE FROM cover_thumbs WHERE source_key = ? OR source_key LIKE ?");
		s.bind(1, artist_rel);
		s.bind(2, artist_rel + "/%");
		s.exec();
		}
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM artist_art WHERE folder_path = ?");
		s.bind(1, artist_rel);
		s.exec();
		}
		}

	// The two derived caches go last, and that ordering is the whole of their
	// correctness: both ask "is there still a song (or folder) at this path",
	// so they have to run once every delete above has happened. They used to
	// sit in the middle, where the loose-file album's song and folder rows —
	// which go last, because nothing marks them unvisited — were still there
	// to be found, and a section deleted from disk left its posters and its
	// TMDB answers behind for ever.
	{
	// Keyed on "no song has this path any more" rather than on the folder
	// marks, so it also catches a single file deleted from an album that is
	// still there — the folder-level prune never reaches those.
	SQLite::Statement s(db_music_,
		"DELETE FROM video_art WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, prefix);
	s.exec();
	}
	{
	// video_meta is keyed by *either* an album folder or a song, so unlike
	// video_art it cannot key its prune on songs alone — that would delete
	// every film, all of which are folders.
	SQLite::Statement s(db_music_,
		"DELETE FROM video_meta WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)"
		"  AND path NOT IN (SELECT path FROM folders)");
	s.bind(1, prefix);
	s.exec();
	}
	{
	// The chapter index of a video that has gone. Keyed on songs alone, unlike
	// video_meta above: a chapter row is always a song's, never a folder's.
	// Here with the other derived sweeps for the reason stated above them --
	// it asks whether a song still has this path, so every DELETE FROM songs
	// must already have run.
	SQLite::Statement s(db_music_,
		"DELETE FROM chapters WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, prefix);
	s.exec();
	// Only when it took something. This is the other way an indexed chapter
	// list disappears -- the video itself going missing, briefly or for good,
	// and its markers following the song row out -- and the two are told apart
	// in the log rather than guessed at.
	const int gone = db_music_.getChanges();
	if (gone > 0)
		std::cout << stamp() << "scan: pruned " << gone
		          << " chapter row(s) whose song is gone, under " << prefix
		          << std::endl;
	}
	{
	// The genres of a song that has gone. Here with the other derived sweeps
	// and last for the same reason: it asks whether a song still has this
	// path, so every DELETE FROM songs above must already have run.
	SQLite::Statement s(db_music_,
		"DELETE FROM song_genres WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, prefix);
	s.exec();
	}
	txn.commit();
	}
	}
	catch (const std::exception& e) {
		std::cout << stamp() << "  prune failed, left as it was: " << e.what()
		          << std::endl;
		}
	std::cout << stamp() << "Rescan complete" << std::endl;
	}

// Media files sitting directly in a root, with no section folder above them —
// a root used as one flat library, which is how Plex and Jellyfin organise a
// movie library.  Same rule as the loose files inside a section: the folder
// that directly contains media is itself an album.  Here the root row plays
// the part of both the artist and the album.
void MediaStore::scan_root_files(const RootRec& root)
	{
	// ---- Phase 1: walk the root itself (no lock) ----
	// One album per file, exactly as scan_artist_dir() builds them for a
	// section: a loose file is its own album whatever it is loose in.  The
	// root plays the artist, and the file-albums are its level-1 children.
	std::vector<AlbumReadData> albums;
	std::error_code ec;
	{
	PhaseTimer pt(scan_times_.walk);
	for (auto& e : fs::directory_iterator(root.cfg.path, ec)) {
		if (!e.is_regular_file() || !is_media_file(e.path())) continue;
		if (is_hidden_name(e.path())) continue;
		AlbumReadData adat;
		adat.loose = true;
		adat.path  = e.path().string();
		adat.title = loose_album_title(e.path());
		adat.cover = find_song_cover(e.path());
		adat.songs.push_back(read_song_file(e.path(), adat.path, 0));
		albums.push_back(std::move(adat));
		}
	}
	if (ec) return;   // scan() has already reported the unreadable root

	// A flat view of the same songs, for the phases that do not care which
	// album a file belongs to.  Valid because nothing appends to `albums`, or
	// to any adat.songs, between here and Phase 4 — the same invariant
	// scan_artist_dir()'s Phase 3 work list depends on.
	std::vector<SongReadData*> all_songs;
	for (auto& adat : albums)
		for (auto& sdat : adat.songs) all_songs.push_back(&sdat);

	// ---- Phase 2: brief read lock — identify changed files ----
	// Keyed on the *path shape* — directly in the root and not below a
	// subdirectory — rather than on the folder id, which is what it used to
	// be.  Under the old rule every such song hung off the root's own folder
	// row and "f.path = <root>" found them all; each now hangs off its own
	// file-album instead, so a folder-keyed query would return nothing, take
	// the songs.empty() && known.empty() early return, and never prune a
	// stale file-album again.  The shape covers both spellings, which is what
	// makes an upgrade work.
	std::unordered_map<std::string, KnownSong> known_songs;
	{
	PhaseTimer pt(scan_times_.known);
	std::lock_guard<std::mutex> lock(db_mutex_);
	// See scan_artist_dir(): is_video is filtered here so a video, which has
	// no readable tag and never will, cannot sit in the back-fill set for ever.
	// audio_codec rides along for the second back-fill: an audio row whose form
	// -- container and codec -- has never been read.  Every audio row qualifies,
	// not only the ambiguous containers: most are answered from the extension
	// with no file opened, and leaving them NULL would put them in front of the
	// pass on every scan for ever.  The is_video test is doing the same job it
	// does for `artist`.
	SQLite::Statement q(db_music_,
		"SELECT s.path, s.file_modified, s.artist, s.is_video, s.audio_codec"
		" FROM songs s WHERE s.path LIKE ? AND s.path NOT LIKE ?");
	q.bind(1, root.cfg.name + "/%");
	q.bind(2, root.cfg.name + "/%/%");
	while (q.executeStep())
		known_songs[join_root(q.getColumn(0).getString())] =
			{ q.getColumn(1).getInt64(),
			  q.getColumn(2).isNull() && q.getColumn(3).getInt() == 0,
			  q.getColumn(4).isNull() && q.getColumn(3).getInt() == 0 };
	}
	if (albums.empty() && known_songs.empty()) return;

	for (auto* sdat : all_songs) {
		auto it = known_songs.find(sdat->path);
		sdat->changed = (it == known_songs.end() || it->second.mtime != sdat->mtime);
		sdat->artist_missing = (it != known_songs.end() && it->second.artist_null);
		sdat->audio_form_missing =
			(it != known_songs.end() && it->second.audio_form_null);
		}

	// As in scan_artist_dir(). The prefix is the whole root here, which is
	// wider than the loose files this function owns, but a superset only costs
	// memory and the alternative is a second spelling of the predicate.
	const std::set<std::string> chapter_keys =
		load_chapter_keys(root.cfg.name + "/%");

	// ---- Phase 3: metadata reads (no lock); see scan_artist_dir() ----
	{
	PhaseTimer pt(scan_times_.meta);
	std::vector<SongReadData*> work;
	for (auto* sdat : all_songs)
		if (sdat->changed || sdat->artist_missing
		        || sdat->audio_form_missing) work.push_back(sdat);

	SongReadStats stats{ scan_times_.files,      scan_times_.videos,
	                      scan_times_.meta_audio, scan_times_.meta_video };
	parallel_for(work.size(), scan_jobs_, [&](std::size_t i) {
		read_one_song(*this, *work[i], stats);
		});
	}

	for (auto& adat : albums) renumber_unseasoned(adat.songs);

	// ---- Phase 3b: cover art for videos that have none (no lock) ----
	// The prefix takes in the whole root rather than just its loose files.
	// That over-fetches (path, mtime) pairs for the sections below, which is
	// cheap, and there is no narrower LIKE — "<root>/%" is the entire root,
	// which is the same reason this function exists at all.
	if (video_art_ && video_art_->enabled()) {
		PhaseTimer pt(scan_times_.art);
		make_video_art(*this, *video_art_, albums, root.cfg.name + "/%");
		}

	// ---- Phase 3c: identify films online (no lock) ----
	// No wrapping and no special case left: these *are* albums, and every one
	// of them is a file-album, which is the case lookup_video_meta() now
	// handles as its ordinary one.
	if (tmdb_.configured() && root.cfg.type == "categories") {
		PhaseTimer pt(scan_times_.tmdb);
		lookup_video_meta(*this, tmdb_, albums);
		}

	// ---- Phase 4: write ----
	// One transaction per album, as in scan_artist_dir(), rather than the
	// single one this used to be: commit_album() owns its own, and a
	// contended commit must not take the whole root with it.
	bool album_failed = false;
	int  folder_id    = -1;
	int  artist_id    = -1;
	try {
		PhaseTimer pt(scan_times_.write);
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		folder_id = upsert_folder(fs::path(root.cfg.path), -1);
		artist_id = upsert_artist(root.cfg.name);

		// Two marks, and they cover the two shapes a loose file can be in.
		//
		// The songs one is the upgrade path: under the old rule every loose
		// file in a root hung off the root's own folder row, and after this
		// pass none may.  Whatever is still marked at the end was not
		// re-homed, which means its file is gone.
		//
		// The folders one is the ordinary mark→sweep, restricted to level-1
		// rows that name a media file.  **That test is `folders.path` being a
		// `songs.path`, not "has an albums row"** — the latter is true of a
		// file-album but it is also true of a *section* on a database scanned
		// under the old rule, and marking a section here would sweep a folder
		// that still has children.  folders.parent_id has no ON DELETE
		// CASCADE, so that is FOREIGN KEY constraint failed and a rolled-back
		// prune: the exact failure the album_info_cache note in
		// scan_artist_dir() records.
		db_music_.exec(("UPDATE songs SET last_scanned = NULL WHERE folder_id = "
		                + std::to_string(folder_id)).c_str());
		SQLite::Statement m(db_music_,
			"UPDATE folders SET last_scanned = NULL"
			" WHERE parent_id = ?"
			"   AND EXISTS (SELECT 1 FROM songs s WHERE s.path = folders.path)");
		m.bind(1, folder_id);
		m.exec();
		txn.commit();
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "Scan: loose files in " << root.cfg.name
		          << ": skipped, " << e.what() << std::endl;
		return;
		}

	for (auto& adat : albums) {
		try {
			commit_album(adat, folder_id, artist_id, chapter_keys);
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "  " << fs::path(adat.path).filename().string()
			          << ": skipped, " << e.what() << std::endl;
			album_failed = true;
			}
		}

	// Same rule as scan_artist_dir()'s prune: a failed album leaves rows
	// marked unvisited that are not in fact gone, and pruning on that would
	// delete a file that is sitting right there.
	if (album_failed) {
		std::cout << stamp() << "  prune skipped: an album failed to commit"
		          << std::endl;
		return;
		}

	try {
	PhaseTimer pt(scan_times_.prune);
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);
	std::string fid = std::to_string(folder_id);

	// The file-albums that have gone.  Collected as ids first, because the
	// predicate that identifies them — the folder's path is a song's path —
	// stops holding the moment the songs are deleted.
	std::string dead;
	{
	SQLite::Statement q(db_music_,
		"SELECT id FROM folders WHERE parent_id = ? AND last_scanned IS NULL"
		"   AND EXISTS (SELECT 1 FROM songs s WHERE s.path = folders.path)");
	q.bind(1, folder_id);
	while (q.executeStep()) {
		if (!dead.empty()) dead += ",";
		dead += std::to_string(q.getColumn(0).getInt());
		}
	}
	if (!dead.empty()) {
		// Ordered exactly as scan_artist_dir()'s prefix prune is, and for the
		// same reason: both info caches REFERENCE folders(id) with no cascade,
		// so a leftover row makes the folder delete throw and rolls the whole
		// transaction back.
		db_music_.exec(("DELETE FROM songs WHERE folder_id IN (" + dead + ")").c_str());
		db_music_.exec(("DELETE FROM albums WHERE folder_id IN (" + dead + ")").c_str());
		db_music_.exec(("DELETE FROM album_info_cache WHERE folder_id IN (" + dead + ")").c_str());
		db_music_.exec(("DELETE FROM artist_info_cache WHERE folder_id IN (" + dead + ")").c_str());
		db_music_.exec(("DELETE FROM cover_thumbs WHERE EXISTS ("
		                "  SELECT 1 FROM folders f WHERE f.id IN (" + dead + ")"
		                "    AND (cover_thumbs.source_key = f.path"
		                "      OR cover_thumbs.source_key LIKE f.path || '/%'))").c_str());
		db_music_.exec(("DELETE FROM folders WHERE id IN (" + dead + ")").c_str());
		std::cout << stamp() << "  pruned loose albums from "
		          << root.cfg.name << std::endl;
		}

	// What is left marked on the root's own folder row is a song an old-rule
	// scan put there and this pass did not re-home — its file is gone.  The
	// album row beside it is the old rule's one-album-per-root, which goes as
	// soon as nothing points at it.  Together they are the upgrade.
	db_music_.exec(("DELETE FROM songs WHERE last_scanned IS NULL"
	                " AND folder_id = " + fid).c_str());
	if (int n = db_music_.getChanges(); n > 0)
		std::cout << stamp() << "  pruned " << n << " loose songs from "
		          << root.cfg.name << std::endl;
	db_music_.exec(("DELETE FROM albums WHERE folder_id = " + fid +
	                " AND NOT EXISTS (SELECT 1 FROM songs"
	                " WHERE songs.album_id = albums.id)").c_str());

	// The derived caches go last, after every DELETE FROM songs *and* every
	// DELETE FROM folders above — they ask whether anything still owns a path,
	// so a sweep that ran earlier would keep rows whose owner was about to go.
	// video_meta's folders clause is load-bearing here now, unlike before: a
	// file-album's path is a folders.path as well as a songs.path.
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM video_art WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	}
	{
	SQLite::Statement s(db_music_,
		"DELETE FROM video_meta WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)"
		"  AND path NOT IN (SELECT path FROM folders)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	}
	{
	// The chapter index, as in scan_artist_dir()'s prune above.
	SQLite::Statement s(db_music_,
		"DELETE FROM chapters WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	const int gone = db_music_.getChanges();
	if (gone > 0)
		std::cout << stamp() << "scan: pruned " << gone
		          << " chapter row(s) whose song is gone, under "
		          << root.cfg.name << std::endl;
	}
	{
	// The genres, likewise.
	SQLite::Statement s(db_music_,
		"DELETE FROM song_genres WHERE path LIKE ?"
		"  AND path NOT IN (SELECT path FROM songs)");
	s.bind(1, root.cfg.name + "/%");
	s.exec();
	}
	{
	// Scaled art for a loose file that has gone. Restricted to things sitting
	// *directly* in the root — "root/%" but not "root/%/%" — because the
	// artist prune already owns everything in a subfolder and does it far more
	// precisely, through the unvisited marks.
	//
	// Known cost: an *extra* image loose in the root (index=1..n) appears in
	// no column, so its thumbnails are dropped by each scan and re-made on the
	// next request. That is one decode per scan for a rare arrangement, and it
	// is the price of not having a table of what an extra image is.
	SQLite::Statement s(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key LIKE ?"
		"  AND source_key NOT LIKE ?"
		"  AND source_key NOT IN (SELECT path FROM songs)"
		"  AND source_key NOT IN (SELECT cover_path FROM songs"
		"                          WHERE cover_path <> '')"
		"  AND source_key NOT IN (SELECT cover_path FROM albums"
		"                          WHERE cover_path <> '')");
	s.bind(1, root.cfg.name + "/%");
	s.bind(2, root.cfg.name + "/%/%");
	s.exec();
	}
	txn.commit();
	}
	}
	catch (const std::exception& e) {
		std::cout << stamp() << "Scan: prune of loose files in " << root.cfg.name
		          << " failed, left as it was: " << e.what() << std::endl;
		}
	}

// ---- upsert helpers ---------------------------------------------------

int MediaStore::upsert_folder(const fs::path& path, int parent_id,
                               const std::string& name_override)
	{
	// Caller passes an absolute path (from fs walks); strip to stored form.
	// A root's own path strips to just its name, which is how a root row ends
	// up with path == name and no parent.
	std::string path_str = strip_root(path.string());
	// The name is the last component, except where the caller knows better.
	// A folder row may name a *media file* — a loose file is its own album —
	// and "film.mp4" is nobody's title.  The caller passes the name rather
	// than it being derived here, because only the caller knows whether this
	// path is a file; a directory legitimately named "Best of 1999.mp3" would
	// otherwise be renamed by an extension test.
	std::string name     = name_override.empty()
	                     ? path.filename().string() : name_override;
	if (name.empty()) name = path.string();  // trailing-slash paths

	if (parent_id < 0) {
		SQLite::Statement ins(db_music_,
			"INSERT OR IGNORE INTO folders (path, name, last_scanned)"
			" VALUES (?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.exec();
		}
	else {
		SQLite::Statement ins(db_music_,
			"INSERT OR IGNORE INTO folders (path, name, parent_id, last_scanned)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, path_str);
		ins.bind(2, name);
		ins.bind(3, parent_id);
		ins.exec();
		}

	// Also set parent_id in case the row already existed without it.
	if (parent_id < 0) {
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET parent_id = NULL, last_scanned = CURRENT_TIMESTAMP"
			" WHERE path = ?");
		upd.bind(1, path_str);
		upd.exec();
		}
	else {
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET parent_id = ?, last_scanned = CURRENT_TIMESTAMP"
			" WHERE path = ?");
		upd.bind(1, parent_id);
		upd.bind(2, path_str);
		upd.exec();
		}

	SQLite::Statement sel(db_music_, "SELECT id FROM folders WHERE path = ?");
	sel.bind(1, path_str);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_artist(const std::string& name)
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO artists (name) VALUES (?)");
	ins.bind(1, name);
	ins.exec();

	SQLite::Statement sel(db_music_, "SELECT id FROM artists WHERE name = ?");
	sel.bind(1, name);
	sel.executeStep();
	return sel.getColumn(0).getInt();
	}

int MediaStore::upsert_album(int folder_id, const std::string& title,
                              int artist_id, int year, const std::string& genre)
	{
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO albums (folder_id, title, year, genre, last_scanned)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)");
	ins.bind(1, folder_id);
	ins.bind(2, title);
	ins.bind(3, year);
	ins.bind(4, genre);
	ins.exec();
	}

	SQLite::Statement sel(db_music_, "SELECT id FROM albums WHERE folder_id = ?");
	sel.bind(1, folder_id);
	sel.executeStep();
	int album_id = sel.getColumn(0).getInt();

	// Link album to its artist (by folder convention: the artist dir above).
	SQLite::Statement lnk(db_music_,
		"INSERT OR IGNORE INTO album_artists (album_id, artist_id, role)"
		" VALUES (?, ?, 'albumartist')");
	lnk.bind(1, album_id);
	lnk.bind(2, artist_id);
	lnk.exec();

	return album_id;
	}

// ---- User management --------------------------------------------------

// A username that can safely be a path component and a comparison key.
//
// The uploads tree is <uploads root>/<username>/<batch>/..., so a username is a
// directory name — and nothing validated it. A '/' in one made the join in the
// upload handler write outside the user's area, with no path_is_within_root()
// on it, and silently broke the `parts[1] == uname` ownership tests that
// deleteUpload and moveAlbum use: those do not error when they stop matching,
// they just stop granting.
//
// Checked here rather than only at the endpoint so that `--add-user` is covered
// too — it is the one path that creates the very first account.
bool MediaStore::valid_username(const std::string& u)
	{
	if (u.empty() || u.size() > 64) return false;
	if (u == "." || u == "..") return false;
	for (char c : u)
		if (!std::isalnum(static_cast<unsigned char>(c))
		        && c != '.' && c != '_' && c != '-')
			return false;
	return true;
	}

bool MediaStore::add_user(const std::string& username, const std::string& password,
                           bool is_admin)
	{
	if (!valid_username(username)) return false;
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Refused case-insensitively, though the column collates by bytes: each
	// user's uploads live at <uploads>/<username>, and on a case-insensitive
	// filesystem — macOS is a supported host — "alice" and "Alice" are one
	// directory on disk while every permission check here compares bytes.
	// Two accounts sharing a directory neither may read is not a state worth
	// being able to create.
	{
	SQLite::Statement dup(db_music_,
		"SELECT 1 FROM client.users WHERE username = ? COLLATE NOCASE");
	dup.bind(1, username);
	if (dup.executeStep()) return false;
	}
	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO client.users (username, password_enc, is_admin) VALUES (?,?,?)");
	ins.bind(1, username);
	ins.bind(2, password);
	ins.bind(3, is_admin ? 1 : 0);
	ins.exec();
	return db_music_.getChanges() > 0;
	}

bool MediaStore::has_users()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_, "SELECT COUNT(*) FROM client.users");
	sel.executeStep();
	return sel.getColumn(0).getInt() > 0;
	}

std::optional<MediaStore::UserInfo> MediaStore::get_user(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT username, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed"
		" FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return std::nullopt;
	UserInfo u;
	u.username       = sel.getColumn(0).getString();
	u.email          = sel.getColumn(1).isNull() ? "" : sel.getColumn(1).getString();
	u.is_admin       = sel.getColumn(2).getInt() != 0;
	u.max_bitrate    = sel.getColumn(3).getInt();
	u.upload_allowed = sel.getColumn(4).getInt() != 0;
	u.disabled       = sel.getColumn(5).getInt() != 0;
	u.cast_allowed   = sel.getColumn(6).getInt() != 0;
	return u;
	}

std::vector<MediaStore::UserInfo> MediaStore::list_users()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT username, email, is_admin, max_bitrate, upload_allowed, disabled, cast_allowed"
		" FROM client.users ORDER BY username");
	std::vector<UserInfo> result;
	while (sel.executeStep()) {
		UserInfo u;
		u.username       = sel.getColumn(0).getString();
		u.email          = sel.getColumn(1).isNull() ? "" : sel.getColumn(1).getString();
		u.is_admin       = sel.getColumn(2).getInt() != 0;
		u.max_bitrate    = sel.getColumn(3).getInt();
		u.upload_allowed = sel.getColumn(4).getInt() != 0;
		u.disabled       = sel.getColumn(5).getInt() != 0;
		u.cast_allowed   = sel.getColumn(6).getInt() != 0;
		result.push_back(u);
		}
	return result;
	}

bool MediaStore::update_user(const std::string& username,
                              const std::string& new_password,
                              const std::string& email,
                              bool is_admin,
                              int  max_bitrate,
                              bool upload_allowed,
                              bool disabled,
                              bool cast_allowed)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	if (new_password.empty()) {
		SQLite::Statement upd(db_music_,
			"UPDATE client.users"
			" SET email=?, is_admin=?, max_bitrate=?, upload_allowed=?, disabled=?, cast_allowed=?"
			" WHERE username=?");
		upd.bind(1, email);
		upd.bind(2, is_admin       ? 1 : 0);
		upd.bind(3, max_bitrate);
		upd.bind(4, upload_allowed ? 1 : 0);
		upd.bind(5, disabled       ? 1 : 0);
		upd.bind(6, cast_allowed   ? 1 : 0);
		upd.bind(7, username);
		upd.exec();
		}
	else {
		SQLite::Statement upd(db_music_,
			"UPDATE client.users"
			" SET password_enc=?, email=?, is_admin=?, max_bitrate=?, upload_allowed=?, disabled=?, cast_allowed=?"
			" WHERE username=?");
		upd.bind(1, new_password);
		upd.bind(2, email);
		upd.bind(3, is_admin       ? 1 : 0);
		upd.bind(4, max_bitrate);
		upd.bind(5, upload_allowed ? 1 : 0);
		upd.bind(6, disabled       ? 1 : 0);
		upd.bind(7, cast_allowed   ? 1 : 0);
		upd.bind(8, username);
		upd.exec();
		}
	return db_music_.getChanges() > 0;
	}

// The `p=enc:HEXHEX` spelling of a password, decoded, or the string
// unchanged when it does not carry the prefix; nullopt for a malformed hex
// body. One definition, because two callers need it and they must agree:
// validate_auth() reads it off the wire, and the user-management endpoints
// decode it before *storing* — a spec-compliant client that sent
// password=enc:… to changePassword used to have the literal string stored,
// which validate_auth then decoded on the next login and matched nothing;
// the account was locked out of the password path by its own password
// change.
//
// Decoded by hand rather than with std::stoi, which **throws** on anything
// that is not a hex digit. That made `p=enc:zz` an unauthenticated 500 from
// any caller. A malformed password is a wrong password, not a server error.
std::optional<std::string> MediaStore::decode_enc_password(const std::string& p)
	{
	if (!(p.size() > 4 && p.substr(0, 4) == "enc:")) return p;
	const std::string hex = p.substr(4);
	std::string plain;
	auto nibble = [](char c) -> int {
		if (c >= '0' && c <= '9') return c - '0';
		if (c >= 'a' && c <= 'f') return c - 'a' + 10;
		if (c >= 'A' && c <= 'F') return c - 'A' + 10;
		return -1;
		};
	if ((hex.size() % 2) != 0) return std::nullopt;
	for (size_t i = 0; i + 1 < hex.size(); i += 2) {
		const int hi = nibble(hex[i]), lo = nibble(hex[i + 1]);
		if (hi < 0 || lo < 0) return std::nullopt;
		plain += static_cast<char>((hi << 4) | lo);
		}
	return plain;
	}

bool MediaStore::validate_auth(const std::string& username,
                                const std::string& password,
                                const std::string& token,
                                const std::string& salt)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement sel(db_music_,
		"SELECT password_enc, disabled FROM client.users WHERE username = ?");
	sel.bind(1, username);
	if (!sel.executeStep()) return false;
	std::string stored   = sel.getColumn(0).getString();
	bool        disabled = sel.getColumn(1).getInt() != 0;
	if (disabled) return false;

	// Comparison that does not stop at the first differing byte.
	//
	// A network timing attack on a string compare is a marginal threat and
	// this is a cheap way to stop thinking about it — but the *length* leak is
	// the one worth naming: std::string::operator== compares sizes first and
	// returns immediately when they differ, so the obvious spelling tells an
	// attacker the length of the stored password before anything else.
	auto const_time_eq = [](const std::string& a, const std::string& b) {
		unsigned diff = static_cast<unsigned>(a.size() ^ b.size());
		const size_t n = std::max(a.size(), b.size());
		for (size_t i = 0; i < n; ++i)
			diff |= static_cast<unsigned char>(i < a.size() ? a[i] : 0)
			      ^ static_cast<unsigned char>(i < b.size() ? b[i] : 0);
		return diff == 0;
		};

	bool ok = false;

	if (!password.empty()) {
		// Some clients send p=enc:HEXHEX; see decode_enc_password() above.
		auto decoded = decode_enc_password(password);
		// Not a hex string at all, so it cannot be the password however it
		// is read. Fail rather than comparing a half-decoded prefix.
		if (!decoded) return false;
		ok = const_time_eq(*decoded, stored);
		}
	else if (!token.empty() && !salt.empty()) {
		// The Subsonic spec requires a salt of at least six characters, and
		// nothing checked it. A short salt is what makes the token cheap to
		// attack offline: t is md5(password + salt) with the salt chosen by
		// whoever is asking, so a one-character salt turns a captured token
		// into an ordinary unsalted MD5 that a rainbow table answers.
		if (salt.size() < 6) return false;
		std::string expected = md5_hex(stored + salt);
		std::string tok = token;
		std::transform(tok.begin(), tok.end(), tok.begin(), ::tolower);
		ok = const_time_eq(tok, expected);
		}

	if (ok) {
		// One write per minute per user, not one per request.
		//
		// Every authenticated request came through here, and an album grid is
		// a hundred of them — so loading one page was a hundred writes to a
		// single row, each taking the WAL write lock while still holding
		// db_mutex_, in exactly the window where a scan wants both. The Users
		// list shows this to the minute, so a minute is all the resolution
		// there is to lose.
		//
		// Kept in memory rather than expressed as a WHERE on the column: a
		// statement that matches nothing still opens a write transaction, and
		// this map is consulted under a lock the caller is holding anyway.
		auto now  = std::chrono::steady_clock::now();
		auto seen = last_access_seen_.find(username);
		if (seen == last_access_seen_.end()
		    || now - seen->second >= std::chrono::seconds(60)) {
			SQLite::Statement upd(db_music_,
				"UPDATE client.users SET last_access = CURRENT_TIMESTAMP"
				" WHERE username = ?");
			upd.bind(1, username);
			upd.exec();
			last_access_seen_[username] = now;
			}
		}

	return ok;
	}

// ---- Library browsing ------------------------------------------------

std::string MediaStore::get_folder_name(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT name FROM folders WHERE id = ?");
	q.bind(1, folder_id);
	return q.executeStep() ? q.getColumn(0).getString() : "";
	}

std::string MediaStore::get_folder_path(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, folder_id);
	return q.executeStep() ? q.getColumn(0).getString() : "";
	}

std::optional<MediaStore::CachedArtistInfo> MediaStore::get_cached_artist_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mbid, last_fm_url, biography, image_url, wiki_url, allmusic_url, discogs_url"
		" FROM artist_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	// Cleaned on the way out as well as on the way in, which is not belt and
	// braces: artist_info_cache has no TTL, so every row written before the
	// providers were being cleaned would go on serving whatever they sent,
	// for ever.  Bumping MUSIC_CACHE_VERSION is how this codebase normally
	// drops cache rows that were made under an older rule, and is deliberately
	// not used here — these rows cost the whole paced provider chain per
	// artist to rebuild, which is minutes of network for a real library,
	// against a string pass on read.
	//
	// last_fm_url is composed locally from the artist name and needs nothing;
	// it goes through clean_url anyway rather than being the one field a
	// reader has to check the provenance of.
	CachedArtistInfo a;
	std::string mbid = q.getColumn(0).getString();
	a.mbid         = is_uuid(mbid) ? mbid : "";
	a.last_fm_url  = clean_url(q.getColumn(1).getString());
	a.biography    = clean_prose(q.getColumn(2).getString(), MAX_PROSE_BYTES);
	a.image_url    = clean_url(q.getColumn(3).getString());
	a.wiki_url     = clean_url(q.getColumn(4).getString());
	a.allmusic_url = clean_url(q.getColumn(5).getString());
	a.discogs_url  = clean_url(q.getColumn(6).getString());
	return a;
	}

// The artist row for a folder is reached by name, not by a foreign key: an
// artist folder's basename *is* the artist, which is the same convention
// get_artist_dirs() and upsert_artist() work by.
std::string MediaStore::get_artist_tag_mbid(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT COALESCE(a.musicbrainz_id, '') FROM artists a"
		"  JOIN folders f ON f.name = a.name"
		" WHERE f.id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return "";
	return q.getColumn(0).getString();
	}

// albums.folder_id is UNIQUE, so this is a direct lookup rather than the join
// above.
std::string MediaStore::get_album_tag_releasegroup_mbid(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT COALESCE(musicbrainz_releasegroup_id, '') FROM albums"
		" WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return "";
	return q.getColumn(0).getString();
	}

void MediaStore::cache_artist_info(int folder_id, const CachedArtistInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO artist_info_cache"
		" (folder_id, mbid, last_fm_url, biography, image_url, wiki_url, allmusic_url, discogs_url)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.last_fm_url);
	ins.bind(4, info.biography);
	ins.bind(5, info.image_url);
	ins.bind(6, info.wiki_url);
	ins.bind(7, info.allmusic_url);
	ins.bind(8, info.discogs_url);
	ins.exec();
	}

// One query per artist rather than one per song: the scan asks this before it
// decides which files still need art, exactly as Phase 2 fetches every known
// mtime in one go.
std::unordered_map<std::string, int64_t>
MediaStore::load_video_art_keys(const std::string& path_prefix)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	std::unordered_map<std::string, int64_t> result;
	SQLite::Statement q(db_music_,
		"SELECT path, file_modified FROM video_art WHERE path LIKE ?");
	q.bind(1, path_prefix);
	while (q.executeStep())
		result[q.getColumn(0).getString()] = q.getColumn(1).getInt64();
	return result;
	}

std::set<std::string> MediaStore::load_chapter_keys(
	const std::string& path_prefix)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	std::set<std::string> result;
	SQLite::Statement q(db_music_,
		"SELECT DISTINCT path FROM chapters WHERE path LIKE ?");
	q.bind(1, path_prefix);
	while (q.executeStep()) result.insert(q.getColumn(0).getString());
	return result;
	}

void MediaStore::store_video_art(const std::string& rel_path, int64_t mtime,
                                  const std::string& mime,
                                  const std::string& source,
                                  const std::string& bytes)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO video_art"
		" (path, file_modified, mime, source, image)"
		" VALUES (?, ?, ?, ?, ?)");
	ins.bind(1, rel_path);
	ins.bind(2, mtime);
	ins.bind(3, mime);
	ins.bind(4, source);
	ins.bind(5, bytes.data(), static_cast<int>(bytes.size()));
	ins.exec();

	// Anything scaled from the old blob is now wrong. A better tier winning —
	// a TMDB poster replacing a frame grab — changes the image without the
	// media file's mtime moving, so the stamp check in the serving path
	// cannot see it. Inlined rather than calling drop_cover_thumbs(), which
	// would take db_mutex_ a second time; it is not recursive.
	SQLite::Statement del(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key = ?");
	del.bind(1, rel_path);
	del.exec();
	}

std::string MediaStore::get_video_art_source(const std::string& rel_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT source FROM video_art WHERE path = ?");
	q.bind(1, rel_path);
	if (!q.executeStep()) return "";
	return q.getColumn(0).getString();
	}

std::optional<MediaStore::VideoArtRow>
MediaStore::get_video_art(const std::string& rel_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mime, image, file_modified FROM video_art WHERE path = ?");
	q.bind(1, rel_path);
	if (!q.executeStep()) return std::nullopt;
	auto blob = q.getColumn(1);
	// A zero-length blob would hand assign() a null pointer, and would reach a
	// client as a 200 with an empty body — which renders as a broken image and
	// looks like a missing file rather than like a bad row.
	if (blob.getBytes() <= 0 || blob.getBlob() == nullptr) return std::nullopt;
	VideoArtRow r;
	r.mime = q.getColumn(0).getString();
	r.bytes.assign(static_cast<const char*>(blob.getBlob()),
	               static_cast<size_t>(blob.getBytes()));
	r.file_modified = q.getColumn(2).getInt64();
	return r;
	}

std::optional<MediaStore::ThumbRow>
MediaStore::get_cover_thumb(const std::string& source_key, int size)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT status, mime, image, width, height, source_stamp"
		" FROM cover_thumbs WHERE source_key = ? AND size = ?");
	q.bind(1, source_key);
	q.bind(2, size);
	if (!q.executeStep()) return std::nullopt;
	ThumbRow r;
	r.status       = q.getColumn(0).getString();
	r.mime         = q.getColumn(1).getString();
	r.width        = q.getColumn(3).getInt();
	r.height       = q.getColumn(4).getInt();
	r.source_stamp = q.getColumn(5).getInt64();
	auto blob = q.getColumn(2);
	// An 'unscalable' row carries no image on purpose; anything else with an
	// empty blob is a bad row and is treated as a miss, for the reason
	// get_video_art() gives — a 200 with no body reads as a missing file.
	if (blob.getBytes() > 0 && blob.getBlob() != nullptr)
		r.bytes.assign(static_cast<const char*>(blob.getBlob()),
		               static_cast<size_t>(blob.getBytes()));
	else if (r.status != "unscalable")
		return std::nullopt;
	return r;
	}

void MediaStore::store_cover_thumb(const std::string& source_key, int size,
                                    int64_t source_stamp,
                                    const std::string& status,
                                    const std::string& mime, int width,
                                    int height, const std::string& bytes)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction tx(db_music_);
	{
	// The hygiene delete for a cover replaced on disk. Without it the sizes
	// nobody happens to re-request would keep their superseded images for
	// ever, since only the size being written is replaced below.
	SQLite::Statement del(db_music_,
		"DELETE FROM cover_thumbs"
		" WHERE source_key = ? AND source_stamp <> ?");
	del.bind(1, source_key);
	del.bind(2, source_stamp);
	del.exec();
	}
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO cover_thumbs"
		" (source_key, size, source_stamp, status, mime, width, height, image)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?)");
	ins.bind(1, source_key);
	ins.bind(2, size);
	ins.bind(3, source_stamp);
	ins.bind(4, status);
	ins.bind(5, mime);
	ins.bind(6, width);
	ins.bind(7, height);
	ins.bind(8, bytes.data(), static_cast<int>(bytes.size()));
	ins.exec();
	}
	tx.commit();
	}

void MediaStore::drop_cover_thumbs(const std::string& source_key)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key = ?");
	del.bind(1, source_key);
	del.exec();
	}

std::optional<MediaStore::ArtistArtRow>
MediaStore::get_artist_art(const std::string& folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT status, source, source_url, mime, image, width, height,"
		"       fetched_at FROM artist_art WHERE folder_path = ?");
	q.bind(1, folder_path);
	if (!q.executeStep()) return std::nullopt;
	ArtistArtRow r;
	r.status     = q.getColumn(0).getString();
	r.source     = q.getColumn(1).getString();
	r.source_url = q.getColumn(2).getString();
	r.mime       = q.getColumn(3).getString();
	r.width      = q.getColumn(5).getInt();
	r.height     = q.getColumn(6).getInt();
	r.fetched_at = q.getColumn(7).getInt64();
	auto blob = q.getColumn(4);
	if (blob.getBytes() > 0 && blob.getBlob() != nullptr)
		r.bytes.assign(static_cast<const char*>(blob.getBlob()),
		               static_cast<size_t>(blob.getBytes()));
	// An "ok" row with no image is a bad row; say there is nothing rather than
	// hand the client a 200 with an empty body.
	if (r.status == "ok" && r.bytes.empty()) return std::nullopt;
	return r;
	}

std::optional<MediaStore::ArtistArtState>
MediaStore::get_artist_art_state(const std::string& folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT status, fetched_at FROM artist_art WHERE folder_path = ?");
	q.bind(1, folder_path);
	if (!q.executeStep()) return std::nullopt;
	ArtistArtState s;
	s.status     = q.getColumn(0).getString();
	s.fetched_at = q.getColumn(1).getInt64();
	return s;
	}

void MediaStore::store_artist_art(const std::string& folder_path,
                                   const std::string& name,
                                   const ArtistArtRow& row)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction tx(db_music_);
	{
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO artist_art"
		" (folder_path, name, status, source, source_url, mime, width, height,"
		"  image, fetched_at)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, folder_path);
	ins.bind(2, name);
	ins.bind(3, row.status);
	ins.bind(4, row.source);
	ins.bind(5, row.source_url);
	ins.bind(6, row.mime);
	ins.bind(7, row.width);
	ins.bind(8, row.height);
	ins.bind(9, row.bytes.data(), static_cast<int>(row.bytes.size()));
	ins.exec();
	}
	{
	// Anything scaled from the previous portrait. fetched_at is this row's
	// stamp in cover_thumbs, so a stale thumbnail would miss on its own — but
	// only after the second in which both were written, and a re-fetch is
	// exactly when somebody is looking.
	SQLite::Statement del(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key = ?");
	del.bind(1, folder_path);
	del.exec();
	}
	tx.commit();
	}

std::vector<MediaStore::LookupTarget>
MediaStore::artists_needing_art(int64_t retry_none_before)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// "Needing" means: no row, or a row that says the network failed, or a
	// 'none' old enough to be worth asking about again. An 'ok' is never
	// re-asked — a portrait does not go stale on its own, and the way to
	// replace one is to change what the provider chain finds.
	std::string sql =
		"SELECT f.id, f.name, f.path FROM folders f"
		" WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL"
		"                        AND COALESCE(content_type, 'artists') = 'artists')"
		"   AND NOT EXISTS (SELECT 1 FROM artist_art a"
		"                    WHERE a.folder_path = f.path"
		"                      AND (a.status = 'ok'"
		"                           OR (a.status = 'none' AND a.fetched_at > ?)))";
	sql += not_uploads("f.path");
	sql += " ORDER BY f.name COLLATE NOCASE";

	SQLite::Statement q(db_music_, sql);
	q.bind(1, retry_none_before);
	std::vector<LookupTarget> out;
	while (q.executeStep()) {
		LookupTarget j;
		j.folder_id = q.getColumn(0).getInt();
		j.name      = q.getColumn(1).getString();
		j.path      = q.getColumn(2).getString();
		out.push_back(std::move(j));
		}
	return out;
	}

// The two backfill queries. See the header for why they ask about the *words*
// rather than about whether anything was ever resolved.
//
// Both read a row's emptiness rather than its age, so they are idempotent in
// the only sense that matters here: an entry that acquires a biography or a
// description drops out, and one that does not stays in and is asked again next
// time the button is pressed.
std::vector<MediaStore::LookupTarget> MediaStore::artists_needing_bio()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Same folder predicate as artists_needing_art() -- a level-1 folder of an
	// artists root -- with the test moved from artist_art to the biography.
	std::string sql =
		"SELECT f.id, f.name, f.path FROM folders f"
		" WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL"
		"                        AND COALESCE(content_type, 'artists') = 'artists')"
		"   AND NOT EXISTS (SELECT 1 FROM artist_info_cache c"
		"                    WHERE c.folder_id = f.id AND c.biography <> '')";
	sql += not_uploads("f.path");
	sql += " ORDER BY f.name COLLATE NOCASE";

	SQLite::Statement q(db_music_, sql);
	std::vector<LookupTarget> out;
	while (q.executeStep()) {
		LookupTarget j;
		j.folder_id = q.getColumn(0).getInt();
		j.name      = q.getColumn(1).getString();
		j.path      = q.getColumn(2).getString();
		out.push_back(std::move(j));
		}
	return out;
	}

std::vector<MediaStore::LookupTarget> MediaStore::albums_needing_info()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// An album folder's parent is the artist folder, whose parent is the root.
	// The second arm of the OR is the loose-file album, whose parent *is* the
	// root: one media file sitting directly in a flat library is its own album,
	// and leaving it out would silently skip every such album in the library.
	//
	// The content_type test is on the root in both arms, which is the whole of
	// how video is kept out of this pass -- see the LookupTarget comment.
	std::string sql =
		"SELECT al.folder_id, COALESCE(al.title, f.name), f.path"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" JOIN folders p ON p.id = f.parent_id"
		" WHERE (p.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL"
		"                         AND COALESCE(content_type, 'artists') = 'artists')"
		"        OR (p.parent_id IS NULL"
		"            AND COALESCE(p.content_type, 'artists') = 'artists'))"
		"   AND NOT EXISTS (SELECT 1 FROM album_info_cache c"
		"                    WHERE c.folder_id = al.folder_id AND c.notes <> '')";
	sql += not_uploads("f.path");
	sql += " ORDER BY f.path COLLATE NOCASE";

	SQLite::Statement q(db_music_, sql);
	std::vector<LookupTarget> out;
	while (q.executeStep()) {
		LookupTarget j;
		j.folder_id = q.getColumn(0).getInt();
		j.name      = q.getColumn(1).getString();
		j.path      = q.getColumn(2).getString();
		out.push_back(std::move(j));
		}
	return out;
	}

std::optional<MediaStore::VideoMetaRow>
MediaStore::get_video_meta(const std::string& rel_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT query, media_type, tmdb_id, title, year, overview, status,"
		"       fetched_at, poster_path, genre"
		" FROM video_meta WHERE path = ?");
	q.bind(1, rel_path);
	if (!q.executeStep()) return std::nullopt;
	VideoMetaRow r;
	r.query      = q.getColumn(0).getString();
	r.media_type = q.getColumn(1).getString();
	r.tmdb_id    = q.getColumn(2).isNull() ? 0 : q.getColumn(2).getInt();
	r.title      = q.getColumn(3).isNull() ? "" : q.getColumn(3).getString();
	r.year       = q.getColumn(4).isNull() ? 0 : q.getColumn(4).getInt();
	r.overview   = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
	r.status      = q.getColumn(6).getString();
	r.fetched_at  = q.getColumn(7).getInt64();
	r.poster_path = q.getColumn(8).isNull() ? "" : q.getColumn(8).getString();
	// The NULL is the whole point here and must not be flattened to "": it is
	// what tells tmdb_lookup() this row predates the column and is owed one
	// more question. See VideoMetaRow::genre_known.
	r.genre_known = !q.getColumn(9).isNull();
	r.genre       = r.genre_known ? q.getColumn(9).getString() : "";
	return r;
	}

void MediaStore::store_video_meta(const std::string& rel_path,
                                   const VideoMetaRow& row)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO video_meta"
		" (path, query, media_type, tmdb_id, title, year, overview, status,"
		"  poster_path, genre, fetched_at)"
		" VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, rel_path);
	ins.bind(2, row.query);
	ins.bind(3, row.media_type);
	ins.bind(4, row.tmdb_id);
	ins.bind(5, row.title);
	ins.bind(6, row.year);
	ins.bind(7, row.overview);
	ins.bind(8, row.status);
	ins.bind(9, row.poster_path);
	// Always written, empty included: '' is "asked, TMDB had none", and
	// leaving it NULL would put the film back in the back-fill on every scan
	// for ever -- the same trap songs.artist's bind documents.
	ins.bind(10, row.genre);
	ins.exec();
	}

std::optional<MediaStore::CachedAlbumInfo>
MediaStore::get_cached_album_info(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT mbid, notes, wiki_url, allmusic_url FROM album_info_cache WHERE folder_id = ?");
	q.bind(1, folder_id);
	if (!q.executeStep()) return std::nullopt;
	// On read for the reason get_cached_artist_info() gives.  notes has two
	// independent providers writing it — Wikipedia here and TMDB through
	// apply_album_overview() — which is one more reason not to rely on every
	// write path having remembered.
	CachedAlbumInfo a;
	std::string mbid = q.getColumn(0).getString();
	a.mbid         = is_uuid(mbid) ? mbid : "";
	a.notes        = clean_prose(q.getColumn(1).getString(), MAX_PROSE_BYTES);
	a.wiki_url     = clean_url(q.getColumn(2).getString());
	a.allmusic_url = clean_url(q.getColumn(3).getString());
	return a;
	}

void MediaStore::cache_album_info(int folder_id, const CachedAlbumInfo& info)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement ins(db_music_,
		"INSERT OR REPLACE INTO album_info_cache"
		" (folder_id, mbid, notes, wiki_url, allmusic_url, fetched_at)"
		" VALUES (?, ?, ?, ?, ?, strftime('%s','now'))");
	ins.bind(1, folder_id);
	ins.bind(2, info.mbid);
	ins.bind(3, info.notes);
	ins.bind(4, info.wiki_url);
	ins.bind(5, info.allmusic_url);
	ins.exec();
	}

std::vector<MediaStore::MusicFolder> MediaStore::get_music_folders()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Name is the root's configured name (which is also its stored path), not
	// the directory basename: the basename can change without the library
	// changing, and clients use this to label a folder they may have pinned.
	SQLite::Statement q(db_music_,
		"SELECT id, path, COALESCE(content_type, 'artists')"
		" FROM folders WHERE parent_id IS NULL ORDER BY path");
	std::vector<MusicFolder> result;
	while (q.executeStep()) {
		std::string name = q.getColumn(1).getString();
		// The uploads root is per-user personal space, not a shared library.
		// It gets a root row because scan_dirs() runs over uploaded batches,
		// but it must never be offered as something to browse.
		if (!uploads_prefix_.empty() && name + "/" == uploads_prefix_) continue;
		result.push_back({ q.getColumn(0).getInt(), name,
		                   q.getColumn(2).getString() });
		}
	return result;
	}

std::optional<MediaStore::MusicFolder> MediaStore::music_folder_by_id(int id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// parent_id IS NULL is what makes this a *root* lookup rather than a
	// folder one: without it any album's id would resolve here and become a
	// destination two levels deep.
	SQLite::Statement q(db_music_,
		"SELECT id, path, COALESCE(content_type, 'artists')"
		" FROM folders WHERE id = ? AND parent_id IS NULL");
	q.bind(1, id);
	if (!q.executeStep()) return std::nullopt;
	std::string name = q.getColumn(1).getString();
	// The same exclusion get_music_folders() applies, and for the same reason.
	// Kept here too rather than left to the caller: this is the only other way
	// to reach a root row, so a rule enforced in one of them is not enforced.
	if (!uploads_prefix_.empty() && name + "/" == uploads_prefix_)
		return std::nullopt;
	return MusicFolder{ q.getColumn(0).getInt(), name,
	                    q.getColumn(2).getString() };
	}

std::string MediaStore::get_cover_path(int cover_art_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	if (cover_art_id >= SONG_COVER_ID_BASE) {
		SQLite::Statement q(db_music_,
			"SELECT cover_path FROM songs WHERE id = ?");
		q.bind(1, cover_art_id - SONG_COVER_ID_BASE);
		if (!q.executeStep() || q.getColumn(0).isNull()) return "";
		return q.getColumn(0).getString();
		}
	SQLite::Statement q(db_music_,
		"SELECT cover_path FROM albums WHERE folder_id = ?");
	q.bind(1, cover_art_id);
	if (!q.executeStep() || q.getColumn(0).isNull()) return "";
	return q.getColumn(0).getString();
	}


std::vector<std::string> MediaStore::get_extra_image_paths(int folder_id)
	{
	std::string cover  = get_cover_path(folder_id);
	// Empty for a song's sidecar cover id, which has no folder and no extras.
	std::string folder = get_folder_path(folder_id);
	if (cover.empty() || folder.empty()) return {};

	// A file-album: the folder row names the media file itself, so its extras
	// are the images sitting beside that file rather than the contents of a
	// directory.  Answered explicitly instead of being left to
	// find_extra_images(), whose directory_iterator would throw and be
	// swallowed — the right answer by accident, and only for as long as that
	// catch stays.
	std::string abs_folder = abs_path(folder);
	std::error_code fec;
	if (fs::is_regular_file(abs_folder, fec)) {
		std::vector<std::string> result;
		for (auto& p : sidecars_of(abs_folder)) {
			std::string ext = lower_ext(p);
			if (ext != ".jpg" && ext != ".jpeg" && ext != ".png") continue;
			if (p == abs_path(cover)) continue;
			result.push_back(strip_root(p));
			}
		return result;
		}

	// A folder with subfolders is a section that also holds loose files: the
	// subfolders are its albums, and their images are not this one's extras.
	bool recurse;
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT 1 FROM folders WHERE parent_id = ? LIMIT 1");
	q.bind(1, folder_id);
	recurse = !q.executeStep();
	}

	// find_extra_images works in absolute paths (it walks the filesystem);
	// we strip back to relative on return so the public API stays uniform.
	auto abs_results = find_extra_images(abs_path(folder), abs_path(cover), recurse);
	std::vector<std::string> result;
	result.reserve(abs_results.size());
	for (auto& p : abs_results)
		result.push_back(strip_root(p));
	return result;
	}

int MediaStore::get_image_count(int folder_id)
	{
	if (get_cover_path(folder_id).empty()) return 0;
	return 1 + static_cast<int>(get_extra_image_paths(folder_id).size());
	}

std::optional<MediaStore::SongInfo> MediaStore::get_song(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT id, path, codec, bitrate, duration, file_size, file_modified,"
		"       is_video, width, height, video_codec, audio_codec,"
		"       audio_container"
		" FROM songs WHERE id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;
	SongInfo s;
	s.id            = q.getColumn(0).getInt();
	s.path          = q.getColumn(1).getString();
	s.codec         = q.getColumn(2).isNull() ? "" : q.getColumn(2).getString();
	s.bitrate       = q.getColumn(3).isNull() ? 0  : q.getColumn(3).getInt();
	s.duration      = q.getColumn(4).getDouble();
	s.file_size     = q.getColumn(5).isNull() ? 0  : q.getColumn(5).getInt64();
	s.file_modified = q.getColumn(6).isNull() ? 0  : q.getColumn(6).getInt64();
	s.is_video      = q.getColumn(7).getInt() != 0;
	s.width         = q.getColumn(8).isNull()  ? 0  : q.getColumn(8).getInt();
	s.height        = q.getColumn(9).isNull()  ? 0  : q.getColumn(9).getInt();
	s.video_codec   = q.getColumn(10).isNull() ? "" : q.getColumn(10).getString();
	s.audio_codec   = q.getColumn(11).isNull() ? "" : q.getColumn(11).getString();
	s.audio_container = q.getColumn(12).isNull() ? ""
	                                             : q.getColumn(12).getString();
	return s;
	}

std::vector<MediaStore::ChildEntry> MediaStore::get_videos()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Same shape as the other ChildEntry queries, with the two video columns
	// appended.  COALESCE on the album folder for parent and cover art for the
	// same reason documented in ISSUES.md: a song's own folder_id is the disc
	// (or season) subdirectory, which has no albums row to resolve a cover on.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       s.path, s.width, s.height, s.video_codec, s.audio_codec,"
		"       s.season, COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.is_video = 1" + not_uploads("s.path") +
		" ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE,"
		"          s.disc_number, s.track_number, s.title COLLATE NOCASE");
	std::vector<ChildEntry> result;
	while (q.executeStep()) {
		ChildEntry e;
		e.id           = q.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = q.getColumn(1).getString();
		e.track_number = q.getColumn(2).getInt();
		e.disc_number  = q.getColumn(3).getInt();
		e.year         = q.getColumn(4).getInt();
		e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.duration     = q.getColumn(6).getDouble();
		e.bitrate      = q.getColumn(7).getInt();
		e.file_size    = q.getColumn(8).getInt64();
		e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.parent_id    = q.getColumn(10).getInt();
		e.artist       = q.getColumn(11).getString();
		e.album        = q.getColumn(12).getString();
		e.cover_art_id = q.getColumn(13).getInt();
		e.path         = q.getColumn(14).getString();
		e.width        = q.getColumn(15).isNull() ? 0 : q.getColumn(15).getInt();
		e.height       = q.getColumn(16).isNull() ? 0 : q.getColumn(16).getInt();
		e.video_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
		e.audio_codec  = q.getColumn(18).isNull() ? "" : q.getColumn(18).getString();
		e.season       = q.getColumn(19).getInt();
		e.track_artist = q.getColumn(20).getString();
		result.push_back(std::move(e));
		}
	return result;
	}

std::vector<MediaStore::GenreEntry> MediaStore::get_genres()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Sourced from song_genres, so a film filed as Horror *and* Science
	// Fiction is counted under both. Names are trimmed on write, so the fold
	// here is case alone.
	//
	// The name reported for a group is its commonest spelling, which is a
	// correlated subquery rather than a MIN() because MIN() answers by ASCII
	// order and would report "ROCK" for a library that writes "Rock" nine
	// times out of ten.
	//
	// **Both counts mean "how many things you get if you ask for this
	// genre"**, which is the property the two browse endpoints are then
	// written to satisfy: songCount is what getSongsByGenre returns and
	// albumCount is what getAlbumList type=byGenre returns. So albumCount is
	// albums *having* a song of this genre, matching that filter's EXISTS --
	// counting only albums whose rolled-up albums.genre matched would leave a
	// mostly-Horror film out of Science Fiction, which is the loss this whole
	// table exists to prevent.
	//
	// Ordered by song count because that is what makes the list usable: real
	// libraries have a short head and a tail of one-off tags, and a client
	// that would rather sort alphabetically still can.
	SQLite::Statement q(db_music_,
		"SELECT (SELECT g2.name FROM song_genres g2"
		"         WHERE LOWER(g2.name) = fold"
		+ not_uploads("g2.path") +
		"         GROUP BY g2.name"
		"         ORDER BY COUNT(*) DESC, g2.name LIMIT 1) AS name,"
		"       songs,"
		"       (SELECT COUNT(DISTINCT s.album_id) FROM song_genres g3"
		"          JOIN songs s ON s.path = g3.path"
		"         WHERE LOWER(g3.name) = fold"
		+ not_uploads("g3.path") +
		"       ) AS albums"
		// Joined to songs, not counted off song_genres alone: the counts are
		// a contract with the two browse endpoints, and both of those return
		// rows that exist. A genre row outliving its song is transient -- the
		// scan prune sweeps it -- but between prunes it would inflate a count
		// that is supposed to predict a result set exactly.
		"  FROM (SELECT LOWER(g.name) AS fold,"
		"               COUNT(DISTINCT g.path) AS songs"
		"          FROM song_genres g"
		"          JOIN songs s0 ON s0.path = g.path"
		"         WHERE TRIM(g.name) <> ''"
		+ not_uploads("g.path") +
		"         GROUP BY fold)"
		" ORDER BY songs DESC, fold");

	std::vector<GenreEntry> result;
	while (q.executeStep()) {
		GenreEntry g;
		// The subquery cannot miss -- the fold came from a row it can see --
		// but a NULL here would reach a tinyxml2 attribute, so it is guarded.
		g.name        = q.getColumn(0).isNull() ? "" : q.getColumn(0).getString();
		g.song_count  = q.getColumn(1).getInt();
		g.album_count = q.getColumn(2).getInt();
		if (!g.name.empty()) result.push_back(std::move(g));
		}
	return result;
	}

std::vector<MediaStore::ChildEntry> MediaStore::get_songs_by_genre(
	const std::string& genre, int count, int offset)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Same shape as get_videos(); see the COALESCE note there for why the
	// album's folder answers parent and cover art rather than the song's own.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       s.path, COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE EXISTS (SELECT 1 FROM song_genres g"
		"                WHERE g.path = s.path AND LOWER(g.name) = LOWER(TRIM(?)))"
		+ not_uploads("s.path") +
		" ORDER BY artist COLLATE NOCASE, album COLLATE NOCASE,"
		"          s.disc_number, s.track_number, s.title COLLATE NOCASE"
		" LIMIT ? OFFSET ?");
	q.bind(1, genre);
	q.bind(2, count);
	q.bind(3, offset);

	std::vector<ChildEntry> result;
	while (q.executeStep()) {
		ChildEntry e;
		e.id           = q.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = q.getColumn(1).getString();
		e.track_number = q.getColumn(2).getInt();
		e.disc_number  = q.getColumn(3).getInt();
		e.year         = q.getColumn(4).getInt();
		e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.duration     = q.getColumn(6).getDouble();
		e.bitrate      = q.getColumn(7).getInt();
		e.file_size    = q.getColumn(8).getInt64();
		e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.parent_id    = q.getColumn(10).getInt();
		e.artist       = q.getColumn(11).getString();
		e.album        = q.getColumn(12).getString();
		e.cover_art_id = q.getColumn(13).getInt();
		e.path         = q.getColumn(14).getString();
		e.track_artist = q.getColumn(15).getString();
		e.video_codec  = q.getColumn(16).isNull() ? "" : q.getColumn(16).getString();
		e.audio_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
		e.season       = q.getColumn(18).getInt();
		result.push_back(std::move(e));
		}
	return result;
	}

std::string MediaStore::sidecar_captions(const std::string& abs) const
	{
	// Preference order, not merely a list: a .vtt is what the endpoint answers
	// with, so it is asked for first.  Every one of them still goes through
	// ffmpeg, including the .vtt — the response is then normalised, and a
	// mislabelled file cannot be served verbatim.
	fs::path base = fs::path(abs);
	for (const char* ext : { ".vtt", ".srt", ".ass", ".ssa" }) {
		fs::path cand = base;
		cand.replace_extension(ext);
		std::error_code fec;
		if (fs::exists(cand, fec) && path_is_within_root(cand))
			return cand.string();
		}
	return {};
	}

std::string MediaStore::sidecar_chapters_path(const std::string& abs)
	{
	// Not replace_extension(), unlike sidecar_captions() above: a plain
	// <stem>.txt is exactly what liner notes look like, and getAlbumTexts
	// lists every .txt in an album folder as prose.  The double extension is
	// what keeps the two apart.
	fs::path base = fs::path(abs);
	base.replace_extension("");
	return base.string() + std::string(CHAPTERS_SUFFIX);
	}

std::string MediaStore::sidecar_text_path(const std::string& abs)
	{
	// The liner notes of a file-album.  A plain <stem>.txt, which is exactly
	// what sidecar_chapters_path() goes out of its way *not* to be — see the
	// note there.  The two cannot collide: ".chapters.txt" is a different
	// filename, and getAlbumTexts() excludes it by suffix in the folder case
	// for the same reason.
	fs::path base = fs::path(abs);
	base.replace_extension(".txt");
	return base.string();
	}

std::vector<std::string> MediaStore::sidecars_of(const std::string& abs) const
	{
	// Everything that lives beside a media file and belongs to it, absolute
	// and existing only.  One definition, because CLAUDE.md's rule is that one
	// place knows what "beside" means — and because a mover that misses one
	// leaves a poster or a subtitle track orphaned in the old directory, which
	// nothing afterwards will ever reconnect.
	std::vector<std::string> out;
	auto add = [&](const fs::path& p) {
		std::error_code fec;
		if (!p.empty() && fs::exists(p, fec) && fs::is_regular_file(p, fec))
			out.push_back(p.string());
		};

	// find_song_cover()'s SUFFIXES, in its order.
	fs::path stem = fs::path(abs).parent_path() / fs::path(abs).stem();
	for (const char* suffix : { ".jpg", ".jpeg", ".png",
	                             "-poster.jpg", "-poster.jpeg", "-poster.png" })
		add(fs::path(stem.string() + suffix));

	// The caption formats sidecar_captions() knows, all of them rather than
	// its first hit: a move must carry every one, not the one that would be
	// served.
	for (const char* ext : { ".vtt", ".srt", ".ass", ".ssa" }) {
		fs::path cand = fs::path(abs);
		cand.replace_extension(ext);
		add(cand);
		}

	add(fs::path(sidecar_chapters_path(abs)));
	add(fs::path(sidecar_text_path(abs)));
	return out;
	}

MediaStore::VideoChapters MediaStore::get_chapters(int song_id)
	{
	VideoChapters vc;
	vc.source = "none";

	auto song = get_song(song_id);   // takes db_mutex_ itself; don't hold it here
	if (!song) return vc;

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return vc;

	// The sidecar leads, and does so even when it is empty.  That is the
	// tombstone: without it, clearing the markers of a rip whose container
	// carries its own would make the container's list reappear, and there
	// would be no way at all to say "this film has no chapters".
	std::string side = sidecar_chapters_path(abs);
	std::error_code fec;
	if (fs::exists(side, fec) && path_is_within_root(side)) {
		std::ifstream f(side, std::ios::binary);
		if (f) {
			std::string text((std::istreambuf_iterator<char>(f)),
			                  std::istreambuf_iterator<char>());
			auto p    = parse_chapters(text);
			vc.source = "sidecar";
			vc.chapters = std::move(p.chapters);
			if (vc.chapters.size() > MAX_CHAPTERS)
				vc.chapters.resize(MAX_CHAPTERS);
			return vc;
			}
		}

	// Failing that, whatever the container says.  Read-only: nothing here ever
	// writes chapters back into a media file, because for MP4 that is a track
	// inside the container and so a full rewrite of every byte.
	//
	// **Video only, and this is the one gate that must not be lifted.** The
	// sidecar branch above is a file that is nearly always absent, which costs
	// a failed open; this is an ffprobe. Videos are a small minority of a
	// library, so asking about each one is affordable — asking about every
	// audio track that has no sidecar, which is essentially all of them, is a
	// process spawn per getChapters call. An M4B audiobook is the audio format
	// that genuinely does carry container chapters; see ISSUES.md.
	if (!song->is_video) return vc;

	std::vector<std::string> args = {
		"ffprobe", "-v", "quiet", "-print_format", "json", "-show_chapters", abs
		};
	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return vc;

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return vc;

	try {
		auto j    = nlohmann::json::parse(out);
		auto arr  = j.find("chapters");
		if (arr == j.end() || !arr->is_array()) return vc;
		for (const auto& c : *arr) {
			if (vc.chapters.size() >= MAX_CHAPTERS) break;
			Chapter ch;
			ch.start = probe_num(c, "start_time");
			if (auto tags = c.find("tags"); tags != c.end())
				ch.name = chapter_clean_name(tags->value("title", std::string()));
			vc.chapters.push_back(std::move(ch));
			}
		if (!vc.chapters.empty()) vc.source = "container";
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "get_chapters: cannot read chapters of " << abs
		          << ": " << e.what() << std::endl;
		}
	return vc;
	}

std::vector<MediaStore::AlbumChapterItem>
MediaStore::get_album_chapters(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	std::vector<AlbumChapterItem> out;

	// The same folder predicate get_album()'s flat listing uses -- the album
	// folder, or a disc subdirectory of it -- and the same ordering, so the
	// items come back in the order the tracks are already drawn in.
	//
	// No is_video predicate: the EXISTS below is the real narrowing, and a
	// two-hour DJ set fetched as audio wants this exactly as much as a concert
	// film does.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.duration, s.path"
		" FROM songs s"
		" WHERE (s.folder_id = ?"
		"        OR s.folder_id IN (SELECT id FROM folders WHERE parent_id = ?))"
		"   AND EXISTS (SELECT 1 FROM chapters c WHERE c.path = s.path)"
		" ORDER BY s.disc_number, s.track_number, s.filename");
	q.bind(1, folder_id);
	q.bind(2, folder_id);

	while (q.executeStep()) {
		AlbumChapterItem v;
		v.song_id  = q.getColumn(0).getInt();
		v.title    = q.getColumn(1).getString();
		v.duration = q.getColumn(2).getDouble();
		out.push_back(std::move(v));
		}

	// A second pass rather than a join, because one statement cannot be
	// stepped while another is open on the same connection here without the
	// rows interleaving.
	SQLite::Statement c(db_music_,
		"SELECT start, title FROM chapters WHERE path ="
		" (SELECT path FROM songs WHERE id = ?) ORDER BY idx");
	for (auto& v : out) {
		c.reset();
		c.bind(1, v.song_id);
		while (c.executeStep()) {
			Chapter ch;
			ch.start = c.getColumn(0).getDouble();
			ch.name  = c.getColumn(1).getString();
			v.chapters.push_back(std::move(ch));
			}
		}
	return out;
	}

bool MediaStore::save_chapters(int song_id, const std::vector<Chapter>& chapters)
	{
	auto song = get_song(song_id);
	if (!song) return false;

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return false;

	std::string side = sidecar_chapters_path(abs);
	std::string part = side + ".part";
	// Checked before either is created.  weakly_canonical() is what makes this
	// meaningful for a file that does not exist yet, which is the same reason
	// moveAlbum can validate a destination before moving anything to it.
	if (!path_is_within_root(side) || !path_is_within_root(part)) return false;

	std::string text = format_chapters(chapters);

	// The rename is the only publish, as in TranscodeCache: a reader -- or the
	// person editing this file in vi -- must never see half a save.  The .part
	// name is also what keeps the intermediate out of getAlbumTexts, since that
	// tests for a .txt extension.
	std::error_code fec;
	{
	std::ofstream f(part, std::ios::binary | std::ios::trunc);
	if (!f) {
		std::cout << stamp() << "save_chapters: cannot write " << part
		          << std::endl;
		return false;
		}
	f.write(text.data(), static_cast<std::streamsize>(text.size()));
	f.close();
	if (!f) {
		std::cout << stamp() << "save_chapters: write failed for " << part
		          << std::endl;
		fs::remove(part, fec);
		return false;
		}
	}

	fs::rename(part, side, fec);
	if (fec) {
		std::cout << stamp() << "save_chapters: cannot publish " << side
		          << ": " << fec.message() << std::endl;
		fs::remove(part, fec);
		return false;
		}
	// Keep the browse index in step with the file we just published, so the
	// album view is right immediately rather than after FolderWatcher's
	// debounce. The scanner would repair it eventually -- that is what makes a
	// hand-edited sidecar work -- but "eventually" is the wrong answer to a
	// save the user just pressed. The direct analogue of store_video_art()
	// dropping the thumbnails it invalidated.
	try {
		std::lock_guard<std::mutex> lock(db_mutex_);
		SQLite::Transaction txn(db_music_);
		apply_song_chapters(db_music_, song->path, chapters);
		txn.commit();
		}
	catch (const std::exception& e) {
		// Not a failure of the save: the file is written and is the authority,
		// so the worst case is a browse listing that is one scan out of date.
		std::cout << stamp() << "save_chapters: wrote " << side
		          << " but could not update the index: " << e.what()
		          << std::endl;
		}

	std::cout << stamp() << "save_chapters: wrote " << chapters.size()
	          << " marker(s) to " << side << std::endl;
	return true;
	}

std::string MediaStore::get_captions_vtt(int song_id, int stream_index)
	{
	auto song = get_song(song_id);
	if (!song || !song->is_video) return {};

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return {};

	const CaptionKey key{ song_id, song->file_modified, stream_index };
	{
	std::lock_guard<std::mutex> lk(captions_mutex_);
	auto it = captions_cache_.find(key);
	if (it != captions_cache_.end()) return it->second;
	}

	std::string source = abs;
	if (stream_index < 0) {
		source = sidecar_captions(abs);
		if (source.empty()) return {};
		}

	std::vector<std::string> args = { "ffmpeg", "-v", "quiet", "-i", source };
	if (stream_index >= 0) {
		args.push_back("-map");
		args.push_back("0:" + std::to_string(stream_index));
		}
	args.push_back("-f");
	args.push_back("webvtt");
	args.push_back("pipe:1");

	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return {};

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) out.clear();

	// A failure *is* cached, as an empty entry — the reversal of what this
	// used to say ("ffmpeg not answering is transient, try again"). That
	// reasoning made every failing request a fresh fork: a captionId naming
	// no stream fails identically every time, so an authenticated client
	// could run one ffmpeg per request for ever. An empty entry answers 404
	// like a missing sidecar does, and a genuinely transient failure costs
	// one film's captions until the entry ages out of the bounded queue or
	// the file's mtime changes the key — against an unbounded fork
	// amplifier, that is the right trade.
	{
	std::lock_guard<std::mutex> lk(captions_mutex_);
	if (captions_cache_.emplace(key, out).second) {
		captions_order_.push_back(key);
		// Oldest out first rather than least-recently-used: the access
		// pattern is one film at a time, so age and use order are the same
		// thing here, and a plain queue needs no bookkeeping on the read
		// path — which is the path that has to be fast.
		while (captions_order_.size() > CAPTION_CACHE_MAX) {
			captions_cache_.erase(captions_order_.front());
			captions_order_.erase(captions_order_.begin());
			}
		}
	}
	return out;
	}

MediaStore::VideoStreams MediaStore::get_video_streams(int song_id)
	{
	VideoStreams vs;
	auto song = get_song(song_id);   // takes db_mutex_ itself; don't hold it here
	if (!song || !song->is_video) return vs;

	std::string abs = abs_path(song->path);
	if (!path_is_within_root(abs)) return vs;

	// Before ffprobe, and outside its error paths: a sidecar is a fact about
	// the directory, so a file ffprobe cannot read still has one.  It leads
	// because somebody put it there deliberately, which an embedded track
	// cannot claim.
	if (!sidecar_captions(abs).empty()) {
		CaptionTrack t;
		t.index = SIDECAR_CAPTION_INDEX;
		t.title = "Subtitles";
		vs.captions.push_back(std::move(t));
		}

	std::vector<std::string> args = {
		"ffprobe", "-v", "quiet", "-print_format", "json", "-show_streams", abs
		};
	reproc::process proc;
	reproc::options opts;
	opts.redirect.err.type = reproc::redirect::type::discard;
	if (proc.start(args, opts)) return vs;

	std::string          out;
	reproc::sink::string sink(out);
	auto ec            = reproc::drain(proc, sink, reproc::sink::null);
	auto [status, wec] = proc.wait(reproc::infinite);
	if (ec || wec || status != 0) return vs;

	try {
		auto j       = nlohmann::json::parse(out);
		auto streams = j.find("streams");
		if (streams == j.end() || !streams->is_array()) return vs;
		for (const auto& s : *streams) {
			auto type = s.value("codec_type", std::string());
			if (type != "subtitle" && type != "audio") continue;
			// DVD and Blu-ray subtitles are bitmaps, not text: there is no
			// WebVTT to convert them into, and ffmpeg fails outright if asked.
			// Offering them would mean every DVD's caption list is a list of
			// tracks that 500 when selected.
			auto codec = s.value("codec_name", std::string());
			if (type == "subtitle"
			        && (codec == "dvd_subtitle" || codec == "hdmv_pgs_subtitle"
			            || codec == "dvb_subtitle" || codec == "xsub"))
				continue;
			CaptionTrack t;
			t.index = static_cast<int>(probe_num(s, "index"));
			if (auto tags = s.find("tags"); tags != s.end()) {
				t.language = tags->value("language", std::string());
				t.title    = tags->value("title",    std::string());
				}
			if (t.title.empty()) t.title = s.value("codec_name", std::string());
			(type == "subtitle" ? vs.captions : vs.audio_tracks)
				.push_back(std::move(t));
			}
		}
	catch (const std::exception&) {}
	return vs;
	}

std::vector<MediaStore::ArtistDir> MediaStore::get_artist_dirs(
	const std::string& personal_user, int music_folder_id,
	const std::string& content_type)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// Count albums per artist via the child folders: an album folder is always
	// one level down, a loose file's own album included.  The extra term this
	// used to carry — an album on the folder *itself*, for loose files sitting
	// beside its subdirectories — is gone with the rule that put one there.
	//
	// "IN (…roots…)" rather than "= root": level-1 entries now come from every
	// configured root, and which root a folder belongs to is visible in its
	// path prefix rather than in a column.
	// A root row joins the list only when it carries loose files of its own,
	// which is now visible as a level-1 *child* with an albums row rather than
	// as an albums row on the root.  Its name comes from `path`, not `name`:
	// for a root those differ, and `name` is the directory basename, which is
	// never exposed.
	//
	// **A level-1 folder that has an albums row is excluded**, because under
	// the new rule it is a loose file in a root used as one flat library — an
	// album, not an artist.  No level-1 *directory* can satisfy that test: its
	// files are their own albums and its subdirectories are albums, so it
	// never acquires an albums row of its own.  Without the exclusion such a
	// file appears in the artist list called "Track.mp4".
	const bool all_users = (personal_user == PERSONAL_ALL_USERS);
	std::string sql =
		"SELECT f.id,"
		"       CASE WHEN f.parent_id IS NULL THEN f.path ELSE f.name END AS name,"
		"       COUNT(al.id) AS album_count,"
		// Selected always, used only under all_users. The owner is derived from
		// the path here rather than in a handler because this file is the only
		// place that knows the uploads layout on the read side.
		"       f.path AS path"
		" FROM folders f"
		" LEFT JOIN folders af ON af.parent_id = f.id"
		" LEFT JOIN albums al ON al.folder_id = af.id"
		" WHERE (f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL";
	// Narrow the *root set* by kind rather than the folders themselves: one
	// kind can span several roots, so this must not become an equality on a
	// single parent id.  COALESCE because a root row written before
	// content_type existed defaults to artists.
	if (!content_type.empty())
		sql += " AND COALESCE(content_type, 'artists') = ?";
	sql += ")";
	sql += "        AND NOT EXISTS (SELECT 1 FROM albums own"
	       "                         WHERE own.folder_id = f.id)";
	sql += " OR (f.parent_id IS NULL";
	if (!content_type.empty())
		sql += " AND COALESCE(f.content_type, 'artists') = ?";
	sql += "     AND EXISTS (SELECT 1 FROM folders c JOIN albums a2"
	       "                   ON a2.folder_id = c.id WHERE c.parent_id = f.id)))";
	if (music_folder_id > 0)
		sql += " AND (f.parent_id = ? OR f.id = ?)";
	if (personal_user.empty())
		sql += not_uploads("f.path");
	else
		sql += " AND f.path LIKE ?";
	sql += " GROUP BY f.id ORDER BY name COLLATE NOCASE";

	SQLite::Statement sel(db_music_, sql);
	int idx = 1;
	// Bind order follows the order the fragments were appended above.
	if (!content_type.empty())    sel.bind(idx++, content_type);
	if (!content_type.empty())    sel.bind(idx++, content_type);
	if (music_folder_id > 0)      sel.bind(idx++, music_folder_id);
	if (music_folder_id > 0)      sel.bind(idx++, music_folder_id);
	if (!personal_user.empty())
		sel.bind(idx++, all_users ? uploads_prefix_ + "%"
		                          : uploads_prefix_ + personal_user + "/%");

	std::vector<ArtistDir> result;
	while (sel.executeStep()) {
		ArtistDir d{ sel.getColumn(0).getInt(),
		             sel.getColumn(1).getString(),
		             sel.getColumn(2).getInt(),
		             "" };
		if (all_users) {
			// "<uploads root>/<user>/<batch>/<artist>" — the owner is the
			// component after the prefix. Anything without one cannot be an
			// upload, so it is dropped rather than shown under a blank heading.
			const std::string path = sel.getColumn(3).getString();
			if (uploads_prefix_.empty() || path.size() <= uploads_prefix_.size())
				continue;
			const std::string rest = path.substr(uploads_prefix_.size());
			auto slash = rest.find('/');
			if (slash == std::string::npos || slash == 0) continue;
			d.owner = rest.substr(0, slash);
			}
		result.push_back(std::move(d));
		}
	return result;
	}

std::optional<MediaStore::DirInfo> MediaStore::get_directory(int folder_id,
                                                               bool flat_multi_disc)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch the folder itself. al.id tells us whether this is an album folder.
	// A root is named by its `path`; its `name` is the directory basename,
	// which is never exposed.
	SQLite::Statement fsel(db_music_,
		"SELECT f.id,"
		"       CASE WHEN f.parent_id IS NULL THEN f.path ELSE f.name END,"
		"       f.parent_id,"
		"       al.id AS album_id,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM folders f"
		" LEFT JOIN albums al ON al.folder_id = f.id"
		" WHERE f.id = ?");
	fsel.bind(1, folder_id);
	if (!fsel.executeStep()) return std::nullopt;

	DirInfo dir;
	dir.id           = fsel.getColumn(0).getInt();
	dir.name         = fsel.getColumn(1).getString();
	dir.parent_id    = fsel.getColumn(2).isNull() ? -1 : fsel.getColumn(2).getInt();
	bool is_album    = !fsel.getColumn(3).isNull();
	dir.cover_art_id = fsel.getColumn(4).getInt();

	// Flattening pulls in the songs of every child folder, which is right for
	// an album's disc subdirectories and wrong for a section that holds loose
	// files *and* albums: it would absorb the whole section into one listing
	// and hide the albums themselves.  A disc folder has a folders row but no
	// albums row, which is exactly the distinction needed.
	bool has_child_album = false;
	if (flat_multi_disc && is_album) {
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM folders f JOIN albums al ON al.folder_id = f.id"
			" WHERE f.parent_id = ? LIMIT 1");
		q.bind(1, folder_id);
		has_child_album = q.executeStep();
		}
	bool flatten = flat_multi_disc && is_album && !has_child_album;

	// Child directories — skipped in flat mode for album folders (disc subdirs
	// are absorbed into the song list below).
	if (!flatten) {
		SQLite::Statement dsel(db_music_,
			"SELECT f.id, COALESCE(al.title, f.name) AS title,"
			"       COALESCE(a.name, '') AS artist,"
			"       COALESCE(al.title, f.name) AS album,"
			"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
			"            THEN f.id ELSE -1 END AS cover_art_id,"
			"       COALESCE(al.year, 0) AS year"
			" FROM folders f"
			" LEFT JOIN albums al ON al.folder_id = f.id"
			" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
			" LEFT JOIN artists a ON a.id = aa.artist_id"
			" WHERE f.parent_id = ?"
			" ORDER BY f.name COLLATE NOCASE");
		dsel.bind(1, folder_id);
		while (dsel.executeStep()) {
			ChildEntry e;
			e.id           = dsel.getColumn(0).getInt();
			e.parent_id    = folder_id;
			e.is_dir       = true;
			e.title        = dsel.getColumn(1).getString();
			e.artist       = dsel.getColumn(2).getString();
			e.album        = dsel.getColumn(3).getString();
			e.cover_art_id = dsel.getColumn(4).getInt();
			e.year         = dsel.getColumn(5).getInt();
			dir.children.push_back(std::move(e));
			}
		}

	// Child songs. In flat mode for album folders, also include songs from disc
	// subfolders (one level down), ordered by disc then track. Those songs report
	// the album folder as parent, not the disc subfolder they physically sit in —
	// the listing claims to be the album's, and parent is emitted as albumId.
	const char* song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album, s.width, s.height,"
		"       s.video_codec, s.audio_codec, s.cover_path, s.season,"
		"       COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.folder_id = ?"
		"    OR s.folder_id IN (SELECT id FROM folders WHERE parent_id = ?)"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	const char* song_sql_normal =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album, s.width, s.height,"
		"       s.video_codec, s.audio_codec, s.cover_path, s.season,"
		"       COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	SQLite::Statement ssel(db_music_, flatten ? song_sql_flat : song_sql_normal);
	ssel.bind(1, folder_id);
	if (flatten) ssel.bind(2, folder_id);

	while (ssel.executeStep()) {
		ChildEntry e;
		e.id           = ssel.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = ssel.getColumn(1).getString();
		e.track_number = ssel.getColumn(2).getInt();
		e.disc_number  = ssel.getColumn(3).getInt();
		e.year         = ssel.getColumn(4).getInt();
		e.genre        = ssel.getColumn(5).isNull() ? "" : ssel.getColumn(5).getString();
		e.duration     = ssel.getColumn(6).getDouble();
		e.bitrate      = ssel.getColumn(7).getInt();
		e.file_size    = ssel.getColumn(8).getInt64();
		e.codec        = ssel.getColumn(9).isNull() ? "" : ssel.getColumn(9).getString();
		e.parent_id    = ssel.getColumn(10).getInt();  // album folder in flat mode
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		e.width        = ssel.getColumn(13).isNull() ? 0 : ssel.getColumn(13).getInt();
		e.height       = ssel.getColumn(14).isNull() ? 0 : ssel.getColumn(14).getInt();
		e.video_codec  = ssel.getColumn(15).isNull() ? "" : ssel.getColumn(15).getString();
		e.audio_codec  = ssel.getColumn(16).isNull() ? "" : ssel.getColumn(16).getString();
		// Songs inherit cover art from their parent album folder, unless they
		// have a sidecar image of their own — a loose file's folder cover
		// belongs to the whole section it sits in.
		if (!ssel.getColumn(17).isNull() && ssel.getColumn(17).getString() != "")
			e.cover_art_id = SONG_COVER_ID_BASE + e.id;
		else if (dir.cover_art_id >= 0)
			e.cover_art_id = dir.cover_art_id;
		e.season       = ssel.getColumn(18).getInt();
		e.track_artist = ssel.getColumn(19).getString();
		dir.children.push_back(std::move(e));
		}

	return dir;
	}

// ---- Album list ----------------------------------------------------------

std::vector<MediaStore::AlbumEntry> MediaStore::get_album_list(
	const std::string& type,
	int size, int offset,
	int from_year, int to_year,
	const std::string& genre,
	const std::string& username,
	const std::string& personal_user)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Base SELECT — common to all types. The trailing column is a per-user
	// album-star flag, populated from a LEFT JOIN on client.stars.
	std::string sql =
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		+ ALBUM_VIDEO_COUNT_SQL +
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// Extra joins for play-count-based types.
	bool play_count_join = (type == "frequent" || type == "recent");
	if (play_count_join)
		sql += " LEFT JOIN songs s ON s.album_id = al.id"
		       " LEFT JOIN client.play_counts pc ON pc.song_path = s.path"
		       " AND pc.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// WHERE clause.
	bool has_where = false;
	auto add_where = [&](const std::string& clause) {
		sql += has_where ? " AND " : " WHERE ";
		sql += clause;
		has_where = true;
		};
	if      (type == "byYear")  add_where("al.year BETWEEN ? AND ?");
	// "This album has a song of that genre", not "this album's own genre is
	// that" -- the same rule get_genres() counts albumCount by, so the count
	// and this filter cannot disagree. albums.genre still exists and is still
	// reported as the album's single genre; it is simply not what selects one,
	// because a mostly-Horror film would then be missing from Science Fiction.
	else if (type == "byGenre") add_where(
		"EXISTS (SELECT 1 FROM song_genres g JOIN songs s2 ON s2.path = g.path"
		"         WHERE s2.album_id = al.id AND LOWER(g.name) = LOWER(TRIM(?)))");
	else if (type == "starred") add_where("sa.album_folder_path IS NOT NULL");

	// Braces are load-bearing: without them the else binds to the inner if,
	// and a personal listing would silently become an unfiltered one.
	if (personal_user.empty()) {
		if (!uploads_like_.empty())
			add_where("f.path NOT LIKE '" + uploads_like_ + "'");
		}
	else
		add_where("f.path LIKE ?");

	// GROUP BY needed when aggregating play counts.
	if (play_count_join) sql += " GROUP BY al.id";

	// ORDER BY.
	if      (type == "newest")              sql += " ORDER BY al.created DESC";
	else if (type == "random")              sql += " ORDER BY RANDOM()";
	else if (type == "alphabeticalByName")  sql += " ORDER BY al.title COLLATE NOCASE";
	else if (type == "alphabeticalByArtist")
		sql += " ORDER BY COALESCE(a.name,'') COLLATE NOCASE, al.title COLLATE NOCASE";
	else if (type == "frequent")            sql += " ORDER BY SUM(COALESCE(pc.count,0)) DESC";
	else if (type == "recent")              sql += " ORDER BY MAX(COALESCE(pc.last_played,'')) DESC";
	else if (type == "starred")             sql += " ORDER BY sa.created DESC";
	else if (type == "byYear")              sql += " ORDER BY al.year";
	else if (type == "byGenre")             sql += " ORDER BY al.title COLLATE NOCASE";
	else                                    sql += " ORDER BY al.created DESC"; // fallback

	sql += " LIMIT ? OFFSET ?";

	SQLite::Statement q(db_music_, sql);
	int idx = 1;

	q.bind(idx++, username);                                // album-star LEFT JOIN
	if (play_count_join)
		q.bind(idx++, username);                            // play-count LEFT JOIN
	if (type == "byYear") { q.bind(idx++, from_year); q.bind(idx++, to_year); }
	if (type == "byGenre")  q.bind(idx++, genre);
	if (!personal_user.empty())
		q.bind(idx++, uploads_prefix_ + personal_user + "/%");
	q.bind(idx++, size);
	q.bind(idx++, offset);

	std::vector<AlbumEntry> result;
	while (q.executeStep()) {
		AlbumEntry e;
		e.id           = q.getColumn(0).getInt();
		e.parent_id    = q.getColumn(1).getInt();
		e.title        = q.getColumn(2).getString();
		e.artist       = q.getColumn(3).getString();
		e.cover_art_id = q.getColumn(4).getInt();
		e.song_count   = q.getColumn(5).getInt();
		e.duration     = q.getColumn(6).getInt();
		e.year         = q.getColumn(7).getInt();
		e.genre        = q.getColumn(8).isNull() ? "" : q.getColumn(8).getString();
		e.created      = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.starred      = q.getColumn(10).getString();
		e.video_count  = q.getColumn(11).getInt();
		result.push_back(std::move(e));
		}
	return result;
	}

// ---- Recent songs -----------------------------------------------------------

std::vector<MediaStore::RecentSongEntry> MediaStore::get_recent_songs(
	const std::string& username,
	int size,
	int offset)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate, s.file_size, s.codec,"
		"       al.folder_id,"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title, f.name) AS album,"
		+ SONG_COVER_ART_SQL +
		"       f.parent_id,"
		"       pc.last_played, COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.play_counts pc"
		" JOIN client.users u ON u.id = pc.user_id"
		" JOIN songs s ON s.path = pc.song_path"
		" JOIN albums al ON al.id = s.album_id"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN song_artists sas ON sas.song_id = s.id AND sas.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sas.artist_id"
		" WHERE u.username = ?"
		" ORDER BY pc.last_played DESC"
		" LIMIT ? OFFSET ?");
	q.bind(1, username);
	q.bind(2, size);
	q.bind(3, offset);

	std::vector<RecentSongEntry> result;
	while (q.executeStep()) {
		RecentSongEntry e;
		e.song.id           = q.getColumn(0).getInt();
		e.song.is_dir       = false;
		e.song.title        = q.getColumn(1).getString();
		e.song.track_number = q.getColumn(2).getInt();
		e.song.disc_number  = q.getColumn(3).getInt();
		e.song.year         = q.getColumn(4).getInt();
		e.song.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.song.duration     = q.getColumn(6).getDouble();
		e.song.bitrate      = q.getColumn(7).getInt();
		e.song.file_size    = q.getColumn(8).getInt64();
		e.song.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.song.parent_id    = q.getColumn(10).getInt();
		e.song.artist       = q.getColumn(11).getString();
		e.song.album        = q.getColumn(12).getString();
		e.song.cover_art_id = q.getColumn(13).getInt();
		// column 14 (f.parent_id) unused — parent_id is already the album folder
		e.last_played       = q.getColumn(15).isNull() ? "" : q.getColumn(15).getString();
		e.song.track_artist = q.getColumn(16).getString();
		e.song.video_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
		e.song.audio_codec  = q.getColumn(18).isNull() ? "" : q.getColumn(18).getString();
		e.song.season       = q.getColumn(19).getInt();
		result.push_back(std::move(e));
		}
	return result;
	}

std::optional<MediaStore::ArtistInfo> MediaStore::get_artist(int folder_id,
                                                              const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Look up the artist folder itself.  A root reached this way (it holds
	// loose files, so it is its own artist) is named by its `path`: a root's
	// `name` is the directory basename, which is never exposed.
	SQLite::Statement fsel(db_music_,
		"SELECT id, CASE WHEN parent_id IS NULL THEN path ELSE name END"
		" FROM folders WHERE id = ?");
	fsel.bind(1, folder_id);
	if (!fsel.executeStep()) return std::nullopt;

	ArtistInfo info;
	info.artist.id   = fsel.getColumn(0).getInt();
	info.artist.name = fsel.getColumn(1).getString();

	// Fetch albums whose folder is a direct child of this artist folder, plus
	// an album on the folder itself — the loose files that sit beside the
	// albums rather than inside one.
	// Trailing column is the per-user album-star flag.
	SQLite::Statement asel(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		+ ALBUM_VIDEO_COUNT_SQL +
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)"
		" WHERE f.parent_id = ? OR f.id = ?"
		" ORDER BY al.year, al.title COLLATE NOCASE");
	asel.bind(1, username);
	asel.bind(2, folder_id);
	asel.bind(3, folder_id);

	while (asel.executeStep()) {
		AlbumEntry e;
		e.id           = asel.getColumn(0).getInt();
		e.parent_id    = asel.getColumn(1).getInt();
		e.title        = asel.getColumn(2).getString();
		e.artist       = asel.getColumn(3).getString();
		e.cover_art_id = asel.getColumn(4).getInt();
		e.song_count   = asel.getColumn(5).getInt();
		e.duration     = asel.getColumn(6).getInt();
		e.year         = asel.getColumn(7).getInt();
		e.genre        = asel.getColumn(8).isNull() ? "" : asel.getColumn(8).getString();
		e.created      = asel.getColumn(9).isNull() ? "" : asel.getColumn(9).getString();
		e.starred      = asel.getColumn(10).getString();
		e.video_count  = asel.getColumn(11).getInt();
		info.albums.push_back(std::move(e));
		}

	info.artist.album_count = (int)info.albums.size();
	return info;
	}

std::optional<MediaStore::AlbumInfo> MediaStore::get_album(int folder_id,
                                                             bool flat_multi_disc,
                                                             const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Fetch album metadata. Trailing column is the per-user album-star flag.
	SQLite::Statement msel(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END,"
		"       COALESCE(al.song_count,0),"
		"       CAST(COALESCE(al.duration,0) AS INTEGER),"
		"       COALESCE(al.year,0),"
		"       COALESCE(al.genre,''),"
		"       COALESCE(al.created,''),"
		"       COALESCE(sa.created, '') AS starred"
		+ ALBUM_VIDEO_COUNT_SQL +
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" LEFT JOIN client.stars sa ON sa.album_folder_path = f.path"
		"      AND sa.user_id = (SELECT id FROM client.users WHERE username = ?)"
		" WHERE f.id = ?");
	msel.bind(1, username);
	msel.bind(2, folder_id);
	if (!msel.executeStep()) return std::nullopt;

	AlbumInfo info;
	info.album.id           = msel.getColumn(0).getInt();
	info.album.parent_id    = msel.getColumn(1).getInt();
	info.album.title        = msel.getColumn(2).getString();
	info.album.artist       = msel.getColumn(3).getString();
	info.album.cover_art_id = msel.getColumn(4).getInt();
	info.album.song_count   = msel.getColumn(5).getInt();
	info.album.duration     = msel.getColumn(6).getInt();
	info.album.year         = msel.getColumn(7).getInt();
	info.album.genre        = msel.getColumn(8).isNull() ? "" : msel.getColumn(8).getString();
	info.album.created      = msel.getColumn(9).isNull() ? "" : msel.getColumn(9).getString();
	info.album.starred      = msel.getColumn(10).getString();
	info.album.video_count  = msel.getColumn(11).getInt();

	// Fetch songs, flattening disc subfolders when flat_multi_disc is set.
	// The starred LEFT JOIN is added only when a username is provided.
	const std::string star_col  = username.empty()
		? ", '' AS starred"
		: ", COALESCE(st.created, '') AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// Flat mode absorbs disc-subfolder songs into the album listing, so they report
	// the album folder as parent rather than the disc folder (see get_directory).
	std::string song_sql_flat =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		+ star_col +
		", s.width, s.height, s.video_codec, s.audio_codec, s.cover_path"
		", s.season, COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE s.folder_id = ?"
		"    OR s.folder_id IN (SELECT id FROM folders WHERE parent_id = ?)"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	std::string song_sql_normal =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, s.folder_id,"
		"       COALESCE(a.name, '') AS artist,"
		"       COALESCE(al.title, '') AS album"
		+ star_col +
		", s.width, s.height, s.video_codec, s.audio_codec, s.cover_path"
		", s.season, COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE s.folder_id = ?"
		" ORDER BY s.disc_number, s.track_number, s.filename";

	// As in get_directory(): flattening absorbs the songs of every child
	// folder, and for a loose-file album those children are the section's
	// other albums, not disc subdirectories.
	bool has_child_album = false;
	if (flat_multi_disc) {
		SQLite::Statement q(db_music_,
			"SELECT 1 FROM folders f JOIN albums al ON al.folder_id = f.id"
			" WHERE f.parent_id = ? LIMIT 1");
		q.bind(1, folder_id);
		has_child_album = q.executeStep();
		}
	bool flatten = flat_multi_disc && !has_child_album;

	SQLite::Statement ssel(db_music_, flatten ? song_sql_flat : song_sql_normal);
	int idx = 1;
	if (!username.empty())
		ssel.bind(idx++, username);
	ssel.bind(idx++, folder_id);
	if (flatten) ssel.bind(idx, folder_id);

	while (ssel.executeStep()) {
		ChildEntry e;
		e.id           = ssel.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = ssel.getColumn(1).getString();
		e.track_number = ssel.getColumn(2).getInt();
		e.disc_number  = ssel.getColumn(3).getInt();
		e.year         = ssel.getColumn(4).getInt();
		e.genre        = ssel.getColumn(5).isNull() ? "" : ssel.getColumn(5).getString();
		e.duration     = ssel.getColumn(6).getDouble();
		e.bitrate      = ssel.getColumn(7).getInt();
		e.file_size    = ssel.getColumn(8).getInt64();
		e.codec        = ssel.getColumn(9).isNull() ? "" : ssel.getColumn(9).getString();
		e.parent_id    = folder_id;
		e.artist       = ssel.getColumn(11).getString();
		e.album        = ssel.getColumn(12).getString();
		e.starred      = ssel.getColumn(13).getString();
		e.width        = ssel.getColumn(14).isNull() ? 0 : ssel.getColumn(14).getInt();
		e.height       = ssel.getColumn(15).isNull() ? 0 : ssel.getColumn(15).getInt();
		e.video_codec  = ssel.getColumn(16).isNull() ? "" : ssel.getColumn(16).getString();
		e.audio_codec  = ssel.getColumn(17).isNull() ? "" : ssel.getColumn(17).getString();
		// A sidecar image beats the album cover: on a loose-file album the
		// album cover belongs to the section, not to this file.
		if (!ssel.getColumn(18).isNull() && ssel.getColumn(18).getString() != "")
			e.cover_art_id = SONG_COVER_ID_BASE + e.id;
		else if (info.album.cover_art_id >= 0)
			e.cover_art_id = info.album.cover_art_id;
		e.season       = ssel.getColumn(19).getInt();
		e.track_artist = ssel.getColumn(20).getString();
		info.songs.push_back(std::move(e));
		}

	return info;
	}

// ---- Play queue / bookmarks ------------------------------------------

void MediaStore::save_play_queue(const std::string& username,
                                  const std::vector<std::string>& song_paths,
                                  const std::string& current_path, int64_t offset_ms,
                                  const std::string& client)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_music_);

	SQLite::Statement del(db_music_, "DELETE FROM client.play_queue WHERE user_id = ?");
	del.bind(1, user_id);
	del.exec();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.play_queue (user_id, song_path, position, is_current, offset_ms, client)"
		" VALUES (?, ?, ?, ?, ?, ?)");
	for (int pos = 0; pos < static_cast<int>(song_paths.size()); ++pos) {
		const std::string& sp = song_paths[pos];
		bool is_curr = (!current_path.empty() && sp == current_path);
		ins.bind(1, user_id);
		ins.bind(2, sp);
		ins.bind(3, pos);
		ins.bind(4, is_curr ? 1 : 0);
		ins.bind(5, is_curr ? offset_ms : int64_t(0));
		ins.bind(6, client);
		ins.exec();
		ins.reset();
		}

	txn.commit();
	}

std::optional<MediaStore::PlayQueue> MediaStore::get_play_queue(
	const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       pq.is_current, pq.offset_ms, pq.client, pq.updated,"
		"       COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.play_queue pq"
		" JOIN client.users u ON u.id = pq.user_id"
		" JOIN songs s ON s.path = pq.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ?"
		" ORDER BY pq.position");
	q.bind(1, username);

	PlayQueue pq;
	bool found = false;
	while (q.executeStep()) {
		found = true;
		ChildEntry e;
		e.id           = q.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = q.getColumn(1).getString();
		e.track_number = q.getColumn(2).getInt();
		e.disc_number  = q.getColumn(3).getInt();
		e.year         = q.getColumn(4).getInt();
		e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		e.duration     = q.getColumn(6).getDouble();
		e.bitrate      = q.getColumn(7).getInt();
		e.file_size    = q.getColumn(8).getInt64();
		e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		e.parent_id    = q.getColumn(10).getInt();
		e.artist       = q.getColumn(11).getString();
		e.album        = q.getColumn(12).getString();
		e.cover_art_id = q.getColumn(13).getInt();

		bool is_current = q.getColumn(14).getInt() != 0;
		if (is_current) {
			pq.current_id = e.id;
			pq.offset_ms  = q.getColumn(15).getInt64();
			}
		if (pq.client.empty())  pq.client  = q.getColumn(16).isNull() ? "" : q.getColumn(16).getString();
		if (pq.changed.empty()) pq.changed = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
		e.track_artist = q.getColumn(18).getString();
		e.video_codec  = q.getColumn(19).isNull() ? "" : q.getColumn(19).getString();
		e.audio_codec  = q.getColumn(20).isNull() ? "" : q.getColumn(20).getString();
		e.season       = q.getColumn(21).getInt();

		pq.songs.push_back(std::move(e));
		}

	if (!found) return std::nullopt;
	return pq;
	}

void MediaStore::create_bookmark(const std::string& username,
                                  const std::string& song_path, int64_t position_ms,
                                  const std::string& comment)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.bookmarks (user_id, song_path, position, comment, changed)"
		" VALUES (?, ?, ?, ?, CURRENT_TIMESTAMP)"
		" ON CONFLICT(user_id, song_path) DO UPDATE SET"
		"   position = excluded.position,"
		"   comment  = excluded.comment,"
		"   changed  = CURRENT_TIMESTAMP");
	ins.bind(1, user_id);
	ins.bind(2, song_path);
	ins.bind(3, position_ms);
	ins.bind(4, comment);
	ins.exec();
	}

void MediaStore::scrobble(const std::string& username, const std::string& song_path,
                           bool submission, const std::string& client)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	if (submission) {
		// Completed play — increment count and record timestamp.
		SQLite::Statement ins(db_music_,
			"INSERT INTO client.play_counts (user_id, song_path, count, last_played)"
			" VALUES (?, ?, 1, CURRENT_TIMESTAMP)"
			" ON CONFLICT(user_id, song_path) DO UPDATE SET"
			"   count       = count + 1,"
			"   last_played = CURRENT_TIMESTAMP");
		ins.bind(1, user_id);
		ins.bind(2, song_path);
		ins.exec();
		} else {
		// Now-playing notification — update or replace the single row.
		SQLite::Statement ins(db_music_,
			"INSERT OR REPLACE INTO client.now_playing (user_id, song_path, client, started)"
			" VALUES (?, ?, ?, CURRENT_TIMESTAMP)");
		ins.bind(1, user_id);
		ins.bind(2, song_path);
		ins.bind(3, client);
		ins.exec();
		}
	}

void MediaStore::add_star(const std::string& username,
                          const std::string& song_path,
                          const std::string& album_folder_path,
                          const std::string& artist_folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	// Store NULL for unused kinds. The read queries discriminate song /
	// album / artist stars via `WHERE <col> IS NOT NULL`, which only
	// works if absent kinds are NULL rather than empty strings.
	auto bind_or_null = [](SQLite::Statement& s, int idx, const std::string& v) {
		if (v.empty()) s.bind(idx);   // NULL
		else           s.bind(idx, v);
		};

	SQLite::Statement ins(db_music_,
		"INSERT OR IGNORE INTO client.stars"
		"   (user_id, song_path, album_folder_path, artist_folder_path)"
		" VALUES (?, ?, ?, ?)");
	ins.bind(1, user_id);
	bind_or_null(ins, 2, song_path);
	bind_or_null(ins, 3, album_folder_path);
	bind_or_null(ins, 4, artist_folder_path);
	ins.exec();
	}

void MediaStore::remove_star(const std::string& username,
                             const std::string& song_path,
                             const std::string& album_folder_path,
                             const std::string& artist_folder_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return;
	int user_id = uid_q.getColumn(0).getInt();

	// Build a WHERE that matches the row whose set of non-NULL columns
	// corresponds to the supplied non-empty argument(s). We do not use
	// `IS NULL` blindly for absent args because a star entry has exactly
	// one non-NULL key; matching "all three NULL" would never be a row.
	SQLite::Statement del(db_music_,
		"DELETE FROM client.stars WHERE user_id = ?"
		"   AND COALESCE(song_path,'')          = ?"
		"   AND COALESCE(album_folder_path,'')  = ?"
		"   AND COALESCE(artist_folder_path,'') = ?");
	del.bind(1, user_id);
	del.bind(2, song_path);
	del.bind(3, album_folder_path);
	del.bind(4, artist_folder_path);
	del.exec();
	}

MediaStore::StarredResult MediaStore::get_starred(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	StarredResult result;

	// Starred songs — cover art inherited from album folder if present.
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN songs s ON s.path = st.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ? AND st.song_path IS NOT NULL"
		" ORDER BY st.created DESC");
	sq.bind(1, username);
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		e.track_artist = sq.getColumn(14).getString();
		e.video_codec  = sq.getColumn(15).isNull() ? "" : sq.getColumn(15).getString();
		e.audio_codec  = sq.getColumn(16).isNull() ? "" : sq.getColumn(16).getString();
		e.season       = sq.getColumn(17).getInt();
		result.songs.push_back(std::move(e));
		}

	// Starred albums — stars.album_folder_path stores the album folder path.
	SQLite::Statement aq(db_music_,
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name) AS title,"
		"       COALESCE(a.name,'') AS artist,"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.path = st.album_folder_path"
		" LEFT JOIN albums al ON al.folder_id = f.id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE u.username = ? AND st.album_folder_path IS NOT NULL"
		" ORDER BY st.created DESC");
	aq.bind(1, username);
	while (aq.executeStep()) {
		ChildEntry e;
		e.id           = aq.getColumn(0).getInt();
		e.parent_id    = aq.getColumn(1).getInt();
		e.is_dir       = true;
		e.title        = aq.getColumn(2).getString();
		e.album        = e.title;
		e.artist       = aq.getColumn(3).getString();
		e.cover_art_id = aq.getColumn(4).getInt();
		result.albums.push_back(std::move(e));
		}

	// Starred artists — stars.artist_folder_path stores the artist folder path.
	SQLite::Statement arq(db_music_,
		"SELECT f.id, f.name"
		" FROM client.stars st"
		" JOIN client.users u ON u.id = st.user_id"
		" JOIN folders f ON f.path = st.artist_folder_path"
		" WHERE u.username = ? AND st.artist_folder_path IS NOT NULL"
		" ORDER BY st.created DESC");
	arq.bind(1, username);
	while (arq.executeStep())
		result.artists.push_back({arq.getColumn(0).getInt(),
		                          arq.getColumn(1).getString()});

	return result;
	}

MediaStore::PlaylistInfo MediaStore::create_playlist(const std::string& username,
                                                      const std::string& name,
                                                      const std::vector<std::string>& song_paths)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	SQLite::Statement uid_q(db_music_, "SELECT id FROM client.users WHERE username = ?");
	uid_q.bind(1, username);
	if (!uid_q.executeStep()) return {};
	int user_id = uid_q.getColumn(0).getInt();

	SQLite::Transaction txn(db_music_);

	SQLite::Statement pins(db_music_,
		"INSERT INTO client.playlists (user_id, name) VALUES (?, ?)");
	pins.bind(1, user_id);
	pins.bind(2, name);
	pins.exec();
	int playlist_id = (int)db_music_.getLastInsertRowid();

	SQLite::Statement sins(db_music_,
		"INSERT INTO client.playlist_songs (playlist_id, song_path, position) VALUES (?, ?, ?)");
	for (int pos = 0; pos < (int)song_paths.size(); ++pos) {
		sins.bind(1, playlist_id);
		sins.bind(2, song_paths[pos]);
		sins.bind(3, pos);
		sins.exec();
		sins.reset();
		}

	txn.commit();

	// Read back metadata (timestamps set by SQLite).
	PlaylistInfo pl;
	pl.id        = playlist_id;
	pl.name      = name;
	pl.owner     = username;
	pl.is_public = false;

	SQLite::Statement pmeta(db_music_,
		"SELECT comment, created, updated FROM client.playlists WHERE id = ?");
	pmeta.bind(1, playlist_id);
	if (pmeta.executeStep()) {
		pl.comment = pmeta.getColumn(0).isNull() ? "" : pmeta.getColumn(0).getString();
		pl.created = pmeta.getColumn(1).getString();
		pl.updated = pmeta.getColumn(2).getString();
		}

	// Fetch songs with full metadata, same joins as get_directory.
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement sq(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.playlist_songs ps"
		" JOIN songs s ON s.path = ps.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE ps.playlist_id = ?"
		" ORDER BY ps.position");
	sq.bind(1, playlist_id);
	int total_duration = 0;
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		e.track_artist = sq.getColumn(14).getString();
		e.video_codec  = sq.getColumn(15).isNull() ? "" : sq.getColumn(15).getString();
		e.audio_codec  = sq.getColumn(16).isNull() ? "" : sq.getColumn(16).getString();
		e.season       = sq.getColumn(17).getInt();
		total_duration += (int)e.duration;
		pl.songs.push_back(std::move(e));
		}

	pl.song_count = (int)pl.songs.size();
	pl.duration   = total_duration;
	return pl;
	}

bool MediaStore::delete_playlist(int playlist_id, const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM client.playlists WHERE id = ?"
		" AND user_id = (SELECT id FROM client.users WHERE username = ?)");
	del.bind(1, playlist_id);
	del.bind(2, username);
	del.exec();
	return db_music_.getChanges() > 0;
	}

std::string MediaStore::abs_path(const std::string& rel) const
	{
	return join_root(rel);
	}

std::string MediaStore::rel_path(const std::string& abs) const
	{
	return strip_root(abs);
	}

bool MediaStore::folder_is_album(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT 1 FROM albums WHERE folder_id = ?");
	q.bind(1, folder_id);
	return q.executeStep();
	}

bool MediaStore::is_category_folder(int folder_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// A root row is never a performer either: loose files directly in a root
	// make the root itself the artist, and looking that up would query
	// MusicBrainz for a thing called "movies" — the failure this function
	// exists to prevent.
	{
	SQLite::Statement r(db_music_,
		"SELECT 1 FROM folders WHERE id = ? AND parent_id IS NULL");
	r.bind(1, folder_id);
	if (r.executeStep()) return true;
	}
	SQLite::Statement q(db_music_,
		"SELECT COALESCE(p.content_type, 'artists')"
		" FROM folders f JOIN folders p ON p.id = f.parent_id"
		" WHERE f.id = ? AND p.parent_id IS NULL");
	q.bind(1, folder_id);
	if (!q.executeStep()) return false;
	return q.getColumn(0).getString() == "categories";
	}

// Brings the folders table's root rows into line with the configuration:
// creates a row per configured library root, stamps its content_type, and
// removes any root that is no longer configured along with everything beneath
// it. Without the removal a dropped root would keep appearing in
// getMusicFolders and its stale artists would still be browsable, because
// every query reaches content by path prefix and nothing else would ever
// revisit it.
void MediaStore::sync_roots()
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	std::set<std::string> configured;
	for (const auto& r : roots_) {
		// The uploads root counts as configured — otherwise it would be judged
		// stale below and every user's personal library deleted at startup —
		// but no row is created for it here. Its row appears only when the
		// upload handler scans a batch, and it is filtered out of
		// get_music_folders() so it is never offered as a library to browse.
		configured.insert(r.cfg.name);
		if (r.cfg.type == "uploads") continue;
		int id = upsert_folder(fs::path(r.cfg.path), -1);
		SQLite::Statement upd(db_music_,
			"UPDATE folders SET content_type = ? WHERE id = ?");
		upd.bind(1, r.cfg.type);
		upd.bind(2, id);
		upd.exec();
		}

	std::vector<std::string> stale;
	{
	SQLite::Statement q(db_music_,
		"SELECT path FROM folders WHERE parent_id IS NULL");
	while (q.executeStep()) {
		std::string name = q.getColumn(0).getString();
		if (!configured.count(name)) stale.push_back(name);
		}
	}

	if (stale.empty()) {
		txn.commit();
		return;
		}

	// Foreign keys are deferred to the commit below so the subtree can be torn
	// down in any order. Without it, deleting a parent folder before its
	// children fails, and there is no ordering that satisfies both the
	// folders self-reference and songs.folder_id.
	db_music_.exec("PRAGMA defer_foreign_keys=ON");

	for (const auto& name : stale) {
		std::cout << stamp() << "Root '" << name
		          << "' is no longer configured; removing its entries"
		          << std::endl;
		// Descend the folder tree by id rather than matching a path prefix.
		// A root row is not guaranteed to prefix the paths beneath it — rows
		// written before roots had names store the root's *absolute* path
		// while their descendants are stored relative, so a prefix match finds
		// none of them, leaves them orphaned, and the delete then trips the
		// foreign key. Parentage is the one relationship that is always true.
		static const char* SUBTREE =
			"WITH RECURSIVE sub(id) AS ("
			"  SELECT id FROM folders WHERE path = ?"
			"  UNION ALL"
			"  SELECT f.id FROM folders f JOIN sub ON f.parent_id = sub.id) ";
		for (const char* tail : {
		        "DELETE FROM songs       WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM albums      WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM album_info_cache  WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM artist_info_cache WHERE folder_id IN (SELECT id FROM sub)",
		        "DELETE FROM folders     WHERE id IN (SELECT id FROM sub)" }) {
			SQLite::Statement s(db_music_, std::string(SUBTREE) + tail);
			s.bind(1, name);
			s.exec();
			}
		{
		// cover_thumbs hangs off a path rather than folders.id, so it cannot
		// join the subtree walk above — and the reason that walk exists (rows
		// written before roots had names store an absolute path) cannot apply
		// here, because every source_key was written in stored form by this
		// version or later. A prefix match is therefore exactly right.
		SQLite::Statement s(db_music_,
			"DELETE FROM cover_thumbs WHERE source_key = ? OR source_key LIKE ?");
		s.bind(1, name);
		s.bind(2, name + "/%");
		s.exec();
		}
		{
		SQLite::Statement s(db_music_,
			"DELETE FROM artist_art WHERE folder_path LIKE ?");
		s.bind(1, name + "/%");
		s.exec();
		}
		{
		// Same shape, same reason. Note video_art and video_meta are *not*
		// torn down here and should be -- see ISSUES.md; a de-configured root
		// leaves its posters and TMDB answers behind with nothing able to
		// reach them. Matching that gap rather than fixing it would only have
		// made it wider.
		SQLite::Statement s(db_music_,
			"DELETE FROM chapters WHERE path LIKE ?");
		s.bind(1, name + "/%");
		s.exec();
		}
		{
		// Same shape, same reason.
		SQLite::Statement s(db_music_,
			"DELETE FROM song_genres WHERE path LIKE ?");
		s.bind(1, name + "/%");
		s.exec();
		}
		}

	txn.commit();
	}

std::string MediaStore::not_uploads(const char* col) const
	{
	if (uploads_like_.empty()) return {};
	// The pattern is interpolated rather than bound: these fragments are
	// stitched into larger statements whose bind indices are counted by hand,
	// and a root name is validated at startup to [A-Za-z0-9_-] so it cannot
	// carry a quote.
	return std::string(" AND ") + col + " NOT LIKE '" + uploads_like_ + "'";
	}

const std::vector<MediaStore::Root>& MediaStore::roots() const
	{
	return roots_public_;
	}

const MediaStore::Root* MediaStore::uploads_root() const
	{
	for (const auto& r : roots_)
		if (r.cfg.type == "uploads") return &r.cfg;
	return nullptr;
	}

bool MediaStore::path_is_within_root(const fs::path& candidate) const
	{
	// Within *any* root, not one fixed base.  That generalisation is what
	// makes several roots possible at all, and it does not weaken the check:
	// a symlink inside a root pointing at /etc still fails, because /etc
	// prefixes none of the canonical bases.
	for (const auto& r : roots_)
		if (is_within(candidate, r.canonical)) return true;
	return false;
	}

bool MediaStore::relocate_prefix(const std::string& old_rel,
                                 const std::string& new_rel)
	{
	if (old_rel.empty() || new_rel.empty() || old_rel == new_rel) return false;

	// These are plain UPDATEs, with no per-table merge policy, and that is only
	// safe because of the caller's precondition: nothing exists at new_rel. Half
	// the columns below sit in a UNIQUE or composite primary key (folders.path,
	// songs.path, video_art.path, video_meta.path, artist_art.folder_path,
	// stars, play_counts, bookmarks, cover_thumbs, manual_covers, song_meta),
	// so relocating onto an occupied prefix would raise a constraint error
	// rather than merge.

	// LIKE treats _ and % as wildcards, so a literal prefix has to be escaped.
	// The prunes elsewhere in this file get away without it because a spurious
	// match there can only *keep* a row that was already condemned. Here the
	// statement is an UPDATE: renaming an album called "Vol_2" would rewrite
	// the paths of an unrelated "Vol 2" sitting beside it.
	auto like_escape = [](const std::string& s) {
		std::string r;
		for (char c : s) {
			if (c == '\\' || c == '%' || c == '_') r += '\\';
			r += c;
			}
		return r;
		};
	const std::string like_pat = like_escape(old_rel) + "/%";
	// substr() is 1-based, so the tail of "<old_rel>/rest" starts one past it.
	const int tail_from = (int)old_rel.size() + 1;

	std::lock_guard<std::mutex> lock(db_mutex_);
	try {
		SQLite::Transaction txn(db_music_);
		db_music_.exec("PRAGMA defer_foreign_keys=ON");

		auto rewrite = [&](const char* table, const char* col) {
			SQLite::Statement s(db_music_,
				std::string("UPDATE ") + table + " SET " + col +
				" = ? || substr(" + col + ", ?)"
				" WHERE " + col + " = ? OR " + col + " LIKE ? ESCAPE '\\'");
			s.bind(1, new_rel);
			s.bind(2, tail_from);
			s.bind(3, old_rel);
			s.bind(4, like_pat);
			s.exec();
			};

		// Client tables first. The two databases are both in WAL mode, and
		// SQLite gives no atomic commit across attached databases in WAL — so
		// this transaction can tear on a crash and the order decides which way.
		// A client row naming a path that does not exist yet is invisible and
		// repairable; a music row moved ahead of its client rows orphans them
		// for good.
		rewrite("client.stars",          "song_path");
		rewrite("client.stars",          "album_folder_path");
		rewrite("client.stars",          "artist_folder_path");
		rewrite("client.play_counts",    "song_path");
		rewrite("client.playlist_songs", "song_path");
		rewrite("client.play_queue",     "song_path");
		rewrite("client.now_playing",    "song_path");
		rewrite("client.bookmarks",      "song_path");
		// These two used to move for free: the flag was a column on the albums
		// row and the title was the songs row. Both are paths in a separate
		// WAL file now, so they belong in this list — and a miss is a cover a
		// scan silently overwrites, or a typed title that reverts.
		rewrite("client.manual_covers",  "album_folder_path");
		rewrite("client.song_meta",      "song_path");

		// Music DB. Note what is deliberately absent: video_meta.poster_path is
		// a path on TMDB's servers, not ours, and artists.image_path is a dead
		// column that nothing has ever written.
		rewrite("folders",      "path");
		rewrite("songs",        "path");
		rewrite("songs",        "cover_path");
		rewrite("albums",       "cover_path");
		rewrite("video_art",    "path");
		rewrite("artist_art",   "folder_path");
		rewrite("cover_thumbs", "source_key");
		rewrite("video_meta",   "path");
		rewrite("chapters",     "path");
		rewrite("song_genres",  "path");

		// The three things below are what a following rescan will NOT repair,
		// because every upsert in the scanner is INSERT OR IGNORE and so only
		// ever populates a row that did not exist. Relocating rather than
		// rebuilding is what preserves the row — and therefore also what makes
		// its derived columns our problem.
		// The leaf, minus a media extension: a folder row may name a media
		// file, because a loose file is its own album, and neither
		// "The Third Man.mkv" nor anything else with an extension on it is a
		// title.  Tested on the extension rather than on the filesystem
		// because these two UPDATEs match nothing at all when new_rel names a
		// sidecar — which is exactly what makes moveAlbum's per-sidecar calls
		// safe — so there would be no file to stat in that case anyway.
		std::string leaf = fs::path(new_rel).filename().string();
		if (is_media_file(fs::path(new_rel)))
			leaf = fs::path(new_rel).stem().string();
		{
		// upsert_folder() updates only parent_id and last_scanned on an
		// existing row, so the name would keep its old spelling for ever.
		SQLite::Statement s(db_music_,
			"UPDATE folders SET name = ? WHERE path = ?");
		s.bind(1, leaf);
		s.bind(2, new_rel);
		s.exec();
		}
		{
		// Likewise upsert_album() for the title. An album's title is its
		// folder's leaf name, so this is exactly what the scan would have
		// written had it been inserting the row fresh.
		SQLite::Statement s(db_music_,
			"UPDATE albums SET title = ?"
			" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
		s.bind(1, leaf);
		s.bind(2, new_rel);
		s.exec();
		}
		{
		// And drop the album's artist links. upsert_album()'s link insert is
		// also OR IGNORE, so re-filing an album under a different artist would
		// otherwise *add* a second link and leave the album showing under both
		// names. Deleting them lets the rescan re-establish the one correct
		// link from the folder layout; the album is briefly artist-less, which
		// is why the caller runs that rescan synchronously.
		SQLite::Statement s(db_music_,
			"DELETE FROM album_artists WHERE album_id ="
			" (SELECT id FROM albums WHERE folder_id ="
			"   (SELECT id FROM folders WHERE path = ?))");
		s.bind(1, new_rel);
		s.exec();
		}

		txn.commit();
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "Relocate " << old_rel << " -> " << new_rel
		          << " failed: " << e.what() << std::endl;
		return false;
		}

	std::cout << stamp() << "Relocated DB paths " << old_rel
	          << " -> " << new_rel << std::endl;
	return true;
	}

void MediaStore::forget_prefix(const std::string& rel)
	{
	if (rel.empty()) return;

	// The same escaping relocate_prefix needs, and for a stronger reason. The
	// prunes elsewhere in this file get away without it because a spurious
	// match there can only *keep* a row that was already condemned. These are
	// DELETEs aimed at rows that are alive: without the escape, deleting an
	// album called "Vol_2" would take the stars off an unrelated "Vol 2"
	// sitting beside it.
	auto like_escape = [](const std::string& s) {
		std::string r;
		for (char c : s) {
			if (c == '\\' || c == '%' || c == '_') r += '\\';
			r += c;
			}
		return r;
		};
	const std::string like_pat = like_escape(rel) + "/%";

	std::lock_guard<std::mutex> lock(db_mutex_);
	try {
		SQLite::Transaction txn(db_music_);

		// "= ?" as well as the prefix: stars.album_folder_path holds the album
		// row exactly, with nothing under it to match.
		auto forget = [&](const char* table, const char* col) {
			SQLite::Statement s(db_music_,
				std::string("DELETE FROM ") + table +
				" WHERE " + col + " = ? OR " + col + " LIKE ? ESCAPE '\\'");
			s.bind(1, rel);
			s.bind(2, like_pat);
			s.exec();
			};

		// Client tables only. A following scan_dirs() prunes the music DB and
		// every derived cache keyed on the path; nothing anywhere prunes these.
		forget("client.stars",          "song_path");
		forget("client.stars",          "album_folder_path");
		forget("client.stars",          "artist_folder_path");
		forget("client.play_counts",    "song_path");
		forget("client.playlist_songs", "song_path");
		forget("client.play_queue",     "song_path");
		forget("client.now_playing",    "song_path");
		forget("client.bookmarks",      "song_path");
		// Newly load-bearing. The albums row used to be pruned by the
		// scan_dirs() that follows, taking the flag with it; nothing prunes
		// the client schema. Left behind, a manual_covers row on a deleted
		// album silently suppresses the TMDB poster of whatever is uploaded
		// to that path next.
		forget("client.manual_covers",  "album_folder_path");
		forget("client.song_meta",      "song_path");

		txn.commit();
		}
	catch (const std::exception& e) {
		// Not fatal to the caller: the files are already gone, and a client row
		// left behind is invisible rather than broken. Logged because the one
		// case where it matters — a later batch folding a new file onto this
		// exact path — would otherwise present as a track that is mysteriously
		// already starred.
		std::cout << stamp() << "Forget client rows under " << rel
		          << " failed: " << e.what() << std::endl;
		return;
		}

	std::cout << stamp() << "Forgot client rows under " << rel << std::endl;
	}

std::optional<std::string> MediaStore::song_path_by_id(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM songs WHERE id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<std::string> MediaStore::album_folder_path_by_id(int album_folder_id)
	{
	// External albumId is the folder.id of the album directory.
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, album_folder_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<std::string> MediaStore::artist_folder_path_by_id(int artist_folder_id)
	{
	// External artistId is the folder.id of the artist directory.
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT path FROM folders WHERE id = ?");
	q.bind(1, artist_folder_id);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getString();
	}

std::optional<int> MediaStore::folder_id_by_path(const std::string& rel)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_, "SELECT id FROM folders WHERE path = ?");
	q.bind(1, rel);
	if (!q.executeStep()) return std::nullopt;
	return q.getColumn(0).getInt();
	}

std::optional<MediaStore::ChildEntry> MediaStore::get_song_entry(int song_id)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       s.width, s.height, s.video_codec, s.audio_codec, s.season,"
		"       COALESCE(s.artist,'') AS track_artist"
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE s.id = ?");
	q.bind(1, song_id);
	if (!q.executeStep()) return std::nullopt;

	ChildEntry e;
	e.id           = q.getColumn(0).getInt();
	e.is_dir       = false;
	e.title        = q.getColumn(1).getString();
	e.track_number = q.getColumn(2).getInt();
	e.disc_number  = q.getColumn(3).getInt();
	e.year         = q.getColumn(4).getInt();
	e.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
	e.duration     = q.getColumn(6).getDouble();
	e.bitrate      = q.getColumn(7).getInt();
	e.file_size    = q.getColumn(8).getInt64();
	e.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
	e.parent_id    = q.getColumn(10).getInt();
	e.artist       = q.getColumn(11).getString();
	e.album        = q.getColumn(12).getString();
	e.cover_art_id = q.getColumn(13).getInt();
	e.width        = q.getColumn(14).isNull() ? 0 : q.getColumn(14).getInt();
	e.height       = q.getColumn(15).isNull() ? 0 : q.getColumn(15).getInt();
	e.video_codec  = q.getColumn(16).isNull() ? "" : q.getColumn(16).getString();
	e.audio_codec  = q.getColumn(17).isNull() ? "" : q.getColumn(17).getString();
	e.season       = q.getColumn(18).getInt();
	e.track_artist = q.getColumn(19).getString();
	return e;
	}

MediaStore::SearchResult MediaStore::search(const std::string& query,
                                             int artist_count, int artist_offset,
                                             int album_count,  int album_offset,
                                             int song_count,   int song_offset,
                                             int chapter_count, int chapter_offset,
                                             const std::string& personal_user)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SearchResult result;
	// The client's query is a substring to look for, not a pattern to run:
	// `%` and `_` are LIKE metacharacters, so without escaping them a query of
	// a single "%" matches every row in the library. That is not an injection
	// — the value is still bound — but it is the difference between a search
	// and a full table scan a client can ask for at will, and it is what makes
	// the count clamp in search2/search3 worth having.
	//
	// relocate_prefix() escapes for a related reason and its note explains the
	// other half: a bare `_` "can only ever keep a row" is true of a DELETE
	// aimed at rows already condemned, and false everywhere else.
	auto like_escape = [](const std::string& s) {
		std::string r;
		for (char c : s) {
			if (c == '\\' || c == '%' || c == '_') r += '\\';
			r += c;
			}
		return r;
		};
	std::string pattern = "%" + like_escape(query) + "%";
	std::string path_filter = personal_user.empty()
		? not_uploads("f.path")
		: " AND f.path LIKE ?";
	std::string path_bind = personal_user.empty()
		? ""
		: uploads_prefix_ + personal_user + "/%";
	std::string song_filter = personal_user.empty()
		? not_uploads("s.path")
		: " AND s.path LIKE ?";

	// Artists — folder-level, depth-1 children of the root.
	std::string aq_sql =
		"SELECT f.id, f.name"
		" FROM folders f"
		" WHERE f.parent_id IN (SELECT id FROM folders WHERE parent_id IS NULL)"
		"   AND LOWER(f.name) LIKE LOWER(?) ESCAPE '\\'";
	aq_sql += path_filter;
	aq_sql += " ORDER BY f.name COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement aq(db_music_, aq_sql);
	int aq_i = 1;
	aq.bind(aq_i++, pattern);
	if (!path_bind.empty()) aq.bind(aq_i++, path_bind);
	aq.bind(aq_i++, artist_count);
	aq.bind(aq_i++, artist_offset);
	while (aq.executeStep()) {
		ChildEntry e;
		e.id        = aq.getColumn(0).getInt();
		e.is_dir    = true;
		e.title     = aq.getColumn(1).getString();
		e.artist    = e.title;
		e.parent_id = -1;
		result.artists.push_back(std::move(e));
		}

	// Albums.
	std::string alq_sql =
		"SELECT f.id,"
		+ ALBUM_ARTIST_ID_SQL +
		"       COALESCE(al.title, f.name),"
		"       COALESCE(a.name,''),"
		"       CASE WHEN al.cover_path IS NOT NULL AND al.cover_path != ''"
		"            THEN f.id ELSE -1 END AS cover_art_id"
		" FROM albums al"
		" JOIN folders f ON f.id = al.folder_id"
		" LEFT JOIN album_artists aa ON aa.album_id = al.id AND aa.role = 'albumartist'"
		" LEFT JOIN artists a ON a.id = aa.artist_id"
		" WHERE LOWER(COALESCE(al.title, f.name)) LIKE LOWER(?) ESCAPE '\\'";
	alq_sql += path_filter;
	alq_sql += " ORDER BY al.title COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement alq(db_music_, alq_sql);
	int alq_i = 1;
	alq.bind(alq_i++, pattern);
	if (!path_bind.empty()) alq.bind(alq_i++, path_bind);
	alq.bind(alq_i++, album_count);
	alq.bind(alq_i++, album_offset);
	while (alq.executeStep()) {
		ChildEntry e;
		e.id           = alq.getColumn(0).getInt();
		e.parent_id    = alq.getColumn(1).getInt();
		e.is_dir       = true;
		e.title        = alq.getColumn(2).getString();
		e.album        = e.title;
		e.artist       = alq.getColumn(3).getString();
		e.cover_art_id = alq.getColumn(4).getInt();
		result.albums.push_back(std::move(e));
		}

	// Songs — use al.folder_id as parent so the client can call getAlbum directly.
	// For multi-disc albums songs live in disc subfolders, so s.folder_id would be
	// wrong; al.folder_id is always the album root folder that getAlbum expects.
	std::string sq_sql =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM songs s"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE LOWER(s.title) LIKE LOWER(?) ESCAPE '\\'";
	sq_sql += song_filter;
	sq_sql += " ORDER BY s.title COLLATE NOCASE LIMIT ? OFFSET ?";
	SQLite::Statement sq(db_music_, sq_sql);
	int sq_i = 1;
	sq.bind(sq_i++, pattern);
	if (!path_bind.empty()) sq.bind(sq_i++, path_bind);
	sq.bind(sq_i++, song_count);
	sq.bind(sq_i++, song_offset);
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		e.track_artist = sq.getColumn(14).getString();
		e.video_codec  = sq.getColumn(15).isNull() ? "" : sq.getColumn(15).getString();
		e.audio_codec  = sq.getColumn(16).isNull() ? "" : sq.getColumn(16).getString();
		e.season       = sq.getColumn(17).getInt();
		result.songs.push_back(std::move(e));
		}

	// Chapters. A fourth scan, over a table far smaller than songs — and the
	// cost model is unchanged either way, since there is no index on
	// songs.title and every search here is already a full scan.
	//
	// The uploads filter is on the *video's* path, which is the only path a
	// chapter has: the row is keyed on it.
	if (chapter_count > 0) {
	std::string sql =
		"SELECT c.idx, c.start, c.title, s.id, s.title,"
		"       COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(al.title, ''), COALESCE(a.name, '')"
		" FROM chapters c"
		" JOIN songs s ON s.path = c.path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE LOWER(c.title) LIKE LOWER(?) ESCAPE '\\'";
	if (personal_user.empty()) sql += not_uploads("s.path");
	else                       sql += " AND s.path LIKE ?";
	sql += " ORDER BY c.title COLLATE NOCASE LIMIT ? OFFSET ?";

	SQLite::Statement cq(db_music_, sql);
	int b = 1;
	cq.bind(b++, pattern);
	if (!personal_user.empty())
		cq.bind(b++, uploads_prefix_ + personal_user + "/%");
	cq.bind(b++, chapter_count);
	cq.bind(b++, chapter_offset);

	while (cq.executeStep()) {
		ChapterHit h;
		h.index     = cq.getColumn(0).getInt();
		h.start     = cq.getColumn(1).getDouble();
		h.name      = cq.getColumn(2).getString();
		h.song_id   = cq.getColumn(3).getInt();
		h.track     = cq.getColumn(4).getString();
		h.parent_id = cq.getColumn(5).getInt();
		h.album     = cq.getColumn(6).getString();
		h.artist    = cq.getColumn(7).getString();
		result.chapters.push_back(std::move(h));
		}
	}

	return result;
	}

std::vector<MediaStore::BookmarkInfo> MediaStore::get_bookmarks(
	const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	SQLite::Statement q(db_music_,
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       b.position, COALESCE(b.comment,''), b.created, b.changed,"
		"       u.username, COALESCE(s.artist,'') AS track_artist"
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.bookmarks b"
		" JOIN client.users u ON u.id = b.user_id"
		" JOIN songs s ON s.path = b.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		" WHERE u.username = ?"
		" ORDER BY b.created");
	q.bind(1, username);

	std::vector<BookmarkInfo> result;
	while (q.executeStep()) {
		BookmarkInfo bm;
		bm.entry.id           = q.getColumn(0).getInt();
		bm.entry.is_dir       = false;
		bm.entry.title        = q.getColumn(1).getString();
		bm.entry.track_number = q.getColumn(2).getInt();
		bm.entry.disc_number  = q.getColumn(3).getInt();
		bm.entry.year         = q.getColumn(4).getInt();
		bm.entry.genre        = q.getColumn(5).isNull() ? "" : q.getColumn(5).getString();
		bm.entry.duration     = q.getColumn(6).getDouble();
		bm.entry.bitrate      = q.getColumn(7).getInt();
		bm.entry.file_size    = q.getColumn(8).getInt64();
		bm.entry.codec        = q.getColumn(9).isNull() ? "" : q.getColumn(9).getString();
		bm.entry.parent_id    = q.getColumn(10).getInt();
		bm.entry.artist       = q.getColumn(11).getString();
		bm.entry.album        = q.getColumn(12).getString();
		bm.entry.cover_art_id = q.getColumn(13).getInt();
		bm.position           = q.getColumn(14).getInt64();
		bm.comment            = q.getColumn(15).getString();
		bm.created            = q.getColumn(16).getString();
		bm.changed            = q.getColumn(17).getString();
		bm.username           = q.getColumn(18).getString();
		bm.entry.track_artist = q.getColumn(19).getString();
		bm.entry.video_codec  = q.getColumn(20).isNull() ? "" : q.getColumn(20).getString();
		bm.entry.audio_codec  = q.getColumn(21).isNull() ? "" : q.getColumn(21).getString();
		bm.entry.season       = q.getColumn(22).getInt();
		result.push_back(std::move(bm));
		}
	return result;
	}

bool MediaStore::delete_bookmark(const std::string& username, const std::string& song_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement del(db_music_,
		"DELETE FROM client.bookmarks WHERE song_path = ?"
		" AND user_id = (SELECT id FROM client.users WHERE username = ?)");
	del.bind(1, song_path);
	del.bind(2, username);
	del.exec();
	return db_music_.getChanges() > 0;
	}

bool MediaStore::update_playlist(int playlist_id, const std::string& username,
                                  const std::optional<std::string>& name,
                                  const std::optional<std::string>& comment,
                                  const std::optional<bool>& is_public,
                                  const std::vector<std::string>& song_paths_to_add,
                                  const std::vector<int>& indices_to_remove)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Verify ownership.
	SQLite::Statement own(db_music_,
		"SELECT p.id FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" WHERE p.id = ? AND u.username = ?");
	own.bind(1, playlist_id);
	own.bind(2, username);
	if (!own.executeStep()) return false;

	// Read surviving song_paths in position order, then drop requested indices.
	SQLite::Statement sel(db_music_,
		"SELECT song_path FROM client.playlist_songs WHERE playlist_id = ? ORDER BY position");
	sel.bind(1, playlist_id);
	std::vector<std::string> kept;
	while (sel.executeStep())
		kept.push_back(sel.getColumn(0).getString());

	// Remove in descending index order to avoid shifting.
	std::vector<int> sorted_remove = indices_to_remove;
	std::sort(sorted_remove.rbegin(), sorted_remove.rend());
	for (int idx : sorted_remove)
		if (idx >= 0 && idx < (int)kept.size())
			kept.erase(kept.begin() + idx);

	for (const auto& sp : song_paths_to_add)
		kept.push_back(sp);

	SQLite::Transaction txn(db_music_);

	// Apply metadata changes.
	if (name)      {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET name=? WHERE id=?");
		q.bind(1, *name); q.bind(2, playlist_id); q.exec();
		}
	if (comment)   {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET comment=? WHERE id=?");
		q.bind(1, *comment); q.bind(2, playlist_id); q.exec();
		}
	if (is_public) {
		SQLite::Statement q(db_music_, "UPDATE client.playlists SET is_public=? WHERE id=?");
		q.bind(1, *is_public ? 1 : 0); q.bind(2, playlist_id); q.exec();
		}
	{
	SQLite::Statement q(db_music_, "UPDATE client.playlists SET updated=CURRENT_TIMESTAMP WHERE id=?");
	q.bind(1, playlist_id); q.exec();
	}

	// Replace song list.
	SQLite::Statement del(db_music_, "DELETE FROM client.playlist_songs WHERE playlist_id=?");
	del.bind(1, playlist_id);
	del.exec();

	SQLite::Statement ins(db_music_,
		"INSERT INTO client.playlist_songs (playlist_id, song_path, position) VALUES (?,?,?)");
	for (int pos = 0; pos < (int)kept.size(); ++pos) {
		ins.bind(1, playlist_id);
		ins.bind(2, kept[pos]);
		ins.bind(3, pos);
		ins.exec();
		ins.reset();
		}

	txn.commit();
	return true;
	}

std::vector<MediaStore::PlaylistInfo> MediaStore::get_playlists(const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Song counts and total durations via aggregates; no song rows returned.
	SQLite::Statement q(db_music_,
		"SELECT p.id, p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       COUNT(ps.song_path), COALESCE(SUM(s.duration),0),"
		"       p.created, p.updated"
		" FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" LEFT JOIN client.playlist_songs ps ON ps.playlist_id = p.id"
		" LEFT JOIN songs s ON s.path = ps.song_path"
		" WHERE u.username = ?"
		" GROUP BY p.id"
		" ORDER BY p.name COLLATE NOCASE");
	q.bind(1, username);

	std::vector<PlaylistInfo> result;
	while (q.executeStep()) {
		PlaylistInfo pl;
		pl.id        = q.getColumn(0).getInt();
		pl.name      = q.getColumn(1).getString();
		pl.comment   = q.getColumn(2).getString();
		pl.owner     = q.getColumn(3).getString();
		pl.is_public = q.getColumn(4).getInt() != 0;
		pl.song_count= q.getColumn(5).getInt();
		pl.duration  = q.getColumn(6).getInt();
		pl.created   = q.getColumn(7).getString();
		pl.updated   = q.getColumn(8).getString();
		result.push_back(std::move(pl));
		}
	return result;
	}

std::optional<MediaStore::PlaylistInfo> MediaStore::get_playlist(int playlist_id,
                                                                    const std::string& username)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// The owner predicate is what makes this a *read* of your own playlist
	// rather than of anyone's. Without it the id is the only thing protecting
	// a private playlist, and playlist ids are small sequential integers —
	// get_playlists() hides them from the listing, which made this an
	// enumeration away rather than a link away.
	//
	// `username` used to reach only the starred join below; the empty case is
	// an internal caller that wants no starred column, and it is deliberately
	// *not* a way past this — it selects nothing rather than everything.
	SQLite::Statement pmeta(db_music_,
		"SELECT p.name, COALESCE(p.comment,''), u.username, p.is_public,"
		"       p.created, p.updated"
		" FROM client.playlists p"
		" JOIN client.users u ON u.id = p.user_id"
		" WHERE p.id = ? AND (p.is_public = 1 OR u.username = ?)");
	pmeta.bind(1, playlist_id);
	pmeta.bind(2, username);
	if (!pmeta.executeStep()) return std::nullopt;

	PlaylistInfo pl;
	pl.id        = playlist_id;
	pl.name      = pmeta.getColumn(0).getString();
	pl.comment   = pmeta.getColumn(1).getString();
	pl.owner     = pmeta.getColumn(2).getString();
	pl.is_public = pmeta.getColumn(3).getInt() != 0;
	pl.created   = pmeta.getColumn(4).getString();
	pl.updated   = pmeta.getColumn(5).getString();

	const std::string star_col  = username.empty()
		? ", '' AS starred"
		: ", COALESCE(st.created, '') AS starred";
	const std::string star_join = username.empty()
		? ""
		: " LEFT JOIN client.stars st ON st.song_path = s.path"
		  " AND st.user_id = (SELECT id FROM client.users WHERE username = ?)";

	// al.folder_id, not s.folder_id — songs on a multi-disc album live in disc
	// subfolders, which have no albums row to resolve cover art or getAlbum against.
	std::string pl_sql =
		"SELECT s.id, s.title, s.track_number, s.disc_number,"
		"       s.year, s.genre, s.duration, s.bitrate,"
		"       s.file_size, s.codec, COALESCE(al.folder_id, s.folder_id),"
		"       COALESCE(a.name,'') AS artist,"
		"       COALESCE(al.title,'') AS album,"
		+ SONG_COVER_ART_SQL +
		"       COALESCE(s.artist,'') AS track_artist"
		// After track_artist rather than before it, which is what lets the
		// shared cover fragment above be dropped in unchanged: that fragment
		// ends with a comma, and star_col begins with one.  The reader below
		// numbers them in this order.
		+ star_col
		+ SONG_VIDEO_COLS_SQL +
		" FROM client.playlist_songs ps"
		" JOIN songs s ON s.path = ps.song_path"
		" LEFT JOIN albums al ON al.id = s.album_id"
		" LEFT JOIN song_artists sa ON sa.song_id = s.id AND sa.role = 'artist'"
		" LEFT JOIN artists a ON a.id = sa.artist_id"
		+ star_join +
		" WHERE ps.playlist_id = ?"
		" ORDER BY ps.position";

	SQLite::Statement sq(db_music_, pl_sql);
	int idx = 1;
	if (!username.empty())
		sq.bind(idx++, username);            // star_join user_id subquery
	sq.bind(idx, playlist_id);
	int total_duration = 0;
	while (sq.executeStep()) {
		ChildEntry e;
		e.id           = sq.getColumn(0).getInt();
		e.is_dir       = false;
		e.title        = sq.getColumn(1).getString();
		e.track_number = sq.getColumn(2).getInt();
		e.disc_number  = sq.getColumn(3).getInt();
		e.year         = sq.getColumn(4).getInt();
		e.genre        = sq.getColumn(5).isNull() ? "" : sq.getColumn(5).getString();
		e.duration     = sq.getColumn(6).getDouble();
		e.bitrate      = sq.getColumn(7).getInt();
		e.file_size    = sq.getColumn(8).getInt64();
		e.codec        = sq.getColumn(9).isNull() ? "" : sq.getColumn(9).getString();
		e.parent_id    = sq.getColumn(10).getInt();
		e.artist       = sq.getColumn(11).getString();
		e.album        = sq.getColumn(12).getString();
		e.cover_art_id = sq.getColumn(13).getInt();
		e.track_artist = sq.getColumn(14).getString();
		e.starred      = sq.getColumn(15).getString();
		e.video_codec  = sq.getColumn(16).isNull() ? "" : sq.getColumn(16).getString();
		e.audio_codec  = sq.getColumn(17).isNull() ? "" : sq.getColumn(17).getString();
		e.season       = sq.getColumn(18).getInt();
		total_duration += (int)e.duration;
		pl.songs.push_back(std::move(e));
		}

	pl.song_count = (int)pl.songs.size();
	pl.duration   = total_duration;
	return pl;
	}

bool MediaStore::update_song_meta(int song_id,
                                   const std::optional<std::string>& title,
                                   const std::optional<int>& track_number,
                                   const std::optional<int>& year,
                                   const std::optional<int>& disc_number)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);

	// Verify the song exists before touching anything.
	SQLite::Statement sel(db_music_, "SELECT 1 FROM songs WHERE id = ?");
	sel.bind(1, song_id);
	if (!sel.executeStep()) return false;

	if (title) {
		SQLite::Statement q(db_music_, "UPDATE songs SET title = ? WHERE id = ?");
		q.bind(1, *title);
		q.bind(2, song_id);
		q.exec();
		}
	if (track_number) {
		SQLite::Statement q(db_music_, "UPDATE songs SET track_number = ? WHERE id = ?");
		q.bind(1, *track_number);
		q.bind(2, song_id);
		q.exec();
		}
	if (year) {
		SQLite::Statement q(db_music_, "UPDATE songs SET year = ? WHERE id = ?");
		q.bind(1, *year);
		q.bind(2, song_id);
		q.exec();

		// Propagate to the album: use the same strategy as the scanner —
		// take the year of the first track (disc/track order) with year > 0.
		SQLite::Statement upd(db_music_,
			"UPDATE albums SET year = ("
			"  SELECT s.year FROM songs s"
			"  WHERE s.album_id = (SELECT album_id FROM songs WHERE id = ?)"
			"    AND s.year > 0"
			"  ORDER BY s.disc_number, s.track_number LIMIT 1"
			") WHERE id = (SELECT album_id FROM songs WHERE id = ?)");
		upd.bind(1, song_id);
		upd.bind(2, song_id);
		upd.exec();
		}
	if (disc_number) {
		SQLite::Statement q(db_music_,
			"UPDATE songs SET disc_number = ? WHERE id = ?");
		q.bind(1, *disc_number);
		q.bind(2, song_id);
		q.exec();
		}
	return true;
	}

// The manual_covers row is written here and nowhere else: this is called only
// by setCoverArt, so the row means exactly "a person chose this image". The
// scan reads it to keep a TMDB poster off a hand-picked cover — see
// lookup_video_meta().
bool MediaStore::set_cover_art_path(const std::string& rel_folder,
                                     const std::string& path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Transaction txn(db_music_);

	// The client row first, and unconditionally. Both databases are in WAL
	// mode and SQLite offers no atomic commit across attached databases there,
	// so this transaction can tear and the order decides which way: a
	// manual_covers row whose albums.cover_path has not caught up is repaired
	// by the next scan, whereas the reverse loses the choice for good. Writing
	// it unconditionally also makes it meaningful for a folder the scan has
	// not given an albums row yet.
	SQLite::Statement m(db_music_,
		"INSERT OR REPLACE INTO client.manual_covers (album_folder_path)"
		" VALUES (?)");
	m.bind(1, rel_folder);
	m.exec();

	SQLite::Statement q(db_music_,
		"UPDATE albums SET cover_path = ?"
		" WHERE folder_id = (SELECT id FROM folders WHERE path = ?)");
	q.bind(1, path);
	q.bind(2, rel_folder);
	q.exec();
	bool changed = db_music_.getChanges() > 0;

	// Whatever was scaled from the previous image at this path is now wrong.
	// The mtime moved too, so the serving path's stamp check would catch it —
	// except on a filesystem with one-second timestamps and a second upload
	// inside the same tick, which is precisely the case where somebody is
	// watching to see whether their new cover took.
	SQLite::Statement del(db_music_,
		"DELETE FROM cover_thumbs WHERE source_key = ?");
	del.bind(1, path);
	del.exec();

	txn.commit();
	return changed;
	}

// Keyed on the album's folder path rather than on an id, because the scan asks
// this in Phase 3c — before Phase 4 has upserted the folder and so before any
// id for it exists. Paths are stable across a rescan and rowids are not, which
// is the same reason stars and playlists key on them — and the reason this
// lives in the client DB rather than beside the album it describes.
bool MediaStore::cover_is_manual(const std::string& rel_album_path)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT 1 FROM client.manual_covers WHERE album_folder_path = ?");
	q.bind(1, rel_album_path);
	return q.executeStep();
	}

// A video's edit cannot be written back to the file it describes: the scanner
// takes a video's title, year and episode number from its filename and never
// reads its tags, so a tag written here would be a second copy of a fact that
// nothing reads. Recorded in the client DB instead and re-applied by every
// scan, which is what makes the music DB safe to delete.
//
// Each column is written only when supplied, so editing a title does not blank
// a year edited earlier.
void MediaStore::set_song_meta_override(
                        const std::string& rel_song_path,
                        const std::optional<std::string>& title,
                        const std::optional<int>& track_number,
                        const std::optional<int>& year,
                        const std::optional<int>& disc_number)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"INSERT INTO client.song_meta"
		"   (song_path, title, track_number, year, disc_number, changed)"
		" VALUES (?, ?, ?, ?, ?, CURRENT_TIMESTAMP)"
		" ON CONFLICT(song_path) DO UPDATE SET"
		"   title        = COALESCE(excluded.title,        title),"
		"   track_number = COALESCE(excluded.track_number, track_number),"
		"   year         = COALESCE(excluded.year,         year),"
		"   disc_number  = COALESCE(excluded.disc_number,  disc_number),"
		"   changed      = CURRENT_TIMESTAMP");
	q.bind(1, rel_song_path);
	if (title)        q.bind(2, *title);        else q.bind(2);
	if (track_number) q.bind(3, *track_number); else q.bind(3);
	if (year)         q.bind(4, *year);         else q.bind(4);
	if (disc_number)  q.bind(5, *disc_number);  else q.bind(5);
	q.exec();
	}

std::string MediaStore::get_setting(const std::string& key,
                                    const std::string& default_val)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement q(db_music_,
		"SELECT value FROM client.settings WHERE key = ?");
	q.bind(1, key);
	if (!q.executeStep()) return default_val;
	return q.getColumn(0).getString();
	}

void MediaStore::set_setting(const std::string& key, const std::string& value)
	{
	std::lock_guard<std::mutex> lock(db_mutex_);
	SQLite::Statement s(db_music_,
		"INSERT OR REPLACE INTO client.settings (key, value) VALUES (?, ?)");
	s.bind(1, key);
	s.bind(2, value);
	s.exec();
	}


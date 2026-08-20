#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// Decoding, scaling and re-encoding a cover image, in this process.
//
// What this replaces is a fork of ffmpeg per request.  Every client asks
// getCoverArt for a specific pixel size — 64, 80, 256 and 400 from the web
// client, 144/288/512 from Android, 144/288/800 from iOS — and each of those
// used to spawn ffmpeg, decode the full-size source, scale, encode an MJPEG
// frame and throw the result away.  A folder cover is routinely 3000x3000, so
// an album grid was a few hundred process launches and a few hundred full
// JPEG decodes, repeated on every cold load.
//
// Only JPEG and PNG are handled, which is not a limitation: the scanner
// admits nothing else as cover art (see COVER_FILENAMES, find_song_cover and
// IMG_EXT in mediastore.cc).  The decoders for the formats stb also supports
// are compiled out, because a decoder that cannot be reached is a parser that
// cannot be attacked.
//
// This file knows nothing about the database, the media store or HTTP.  It
// takes bytes and returns bytes, so it can be exercised from the command line
// (--image-scale-test) with no server and no library, which is the question
// every future report on this path will really be asking: can gaindrive
// decode this cover?
namespace imagescale {

// What the magic bytes say a buffer is, regardless of what anyone claimed:
// "image/jpeg", "image/png", or "" for neither.
//
// Both claims available elsewhere are worth less than this.  An upload's
// multipart content_type is whatever the client typed, and getCoverArt did
// not even have that — it labelled every full-size cover image/jpeg whatever
// was on disk.
std::string sniff_mime(std::string_view bytes);

// Dimensions without decoding: stb parses only the header.  This is what
// lets a decompression bomb be refused before anything is allocated for it.
struct Dims
	{
	int width    = 0;
	int height   = 0;
	int channels = 0;
	};
std::optional<Dims> probe(std::string_view bytes);

struct Scaled
	{
	bool        ok     = false;
	int         width  = 0;
	int         height = 0;
	std::string mime;     // image/jpeg, unless from_source
	std::string bytes;
	std::string error;    // stb's reason, or ours; empty when ok

	// True when the input was already small enough and came back untouched.
	// A caller with a cache wants to know: storing a copy of the source under
	// a thumbnail key would duplicate the file in the database to save a stat
	// and a header parse.
	bool        from_source = false;
	};

// Scales so neither edge exceeds max_px, preserving the aspect ratio.
//
// **Never upscales.**  An image already inside max_px is returned unchanged,
// with its own MIME — so a 60x60 cover asked for at size=400 comes back as
// the original 60x60 PNG rather than a blurred JPEG.  That is the same answer
// ffmpeg's force_original_aspect_ratio=decrease gave, and it also means a
// small cover costs no decode at all.
//
// Does not throw and does not log.  Callers are httplib worker threads and
// detached background threads, and on the latter an escaping exception is
// std::terminate; the caller owns the log line, because only the caller knows
// which file this was.
Scaled scale_to_fit(std::string_view bytes, int max_px, int quality = 85);

// The same, reading the file itself.  A file larger than max_bytes is refused
// on its size alone, without being opened.
Scaled scale_file_to_fit(const std::string& path, int max_px, int quality = 85,
                         std::size_t max_bytes = 64u * 1024 * 1024);

}   // namespace imagescale

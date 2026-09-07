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

// Which edge max_px applies to.  There is no default, because the two
// callers want opposite things and a default would hide that.
//
//   Long  — neither edge exceeds max_px.  The bound for *storing* an image
//           whose shape nobody has chosen: the artist portrait normalisation
//           wants "no bigger than 800 either way".
//
//   Short — the *short* edge becomes max_px, so the long one overshoots.
//           The bound for a thumbnail that will be cropped to a square, which
//           is every cover surface in every gaindrive client.  Fitting the
//           long edge there hands back a 2:3 poster as 107x160 for a 160
//           request, and the client then upscales it to fill the square — the
//           picture is blurred by the *client* however sharp what we sent was.
enum class Fit { Long, Short };

// How far past max_px the long edge may run under Fit::Short.  Fit::Long
// bounds the output at max_px squared; Fit::Short bounds it at max_px squared
// times the aspect ratio, which is bounded by nothing but MAX_PIXELS — an
// 8000x1000 gatefold scan asked for at 800 would otherwise come back as five
// megapixels of "thumbnail".  Past 4:1 a square crop is showing an eighth of
// the picture and nobody is judging its sharpness, so the clamp costs only
// what the long-edge rule cost anyway.
constexpr int LONG_EDGE_LIMIT = 4;

// Scales to max_px on the edge Fit names, preserving the aspect ratio.
//
// **Never upscales.**  An image already inside max_px on that edge is
// returned unchanged, with its own MIME — so a 60x60 cover asked for at
// size=400 comes back as the original 60x60 PNG rather than a blurred JPEG,
// and it also means a small cover costs no decode at all.
//
// Under Fit::Short the long edge is additionally capped at LONG_EDGE_LIMIT
// times max_px, so the result can be shorter than max_px on its short edge.
//
// Does not throw and does not log.  Callers are httplib worker threads and
// detached background threads, and on the latter an escaping exception is
// std::terminate; the caller owns the log line, because only the caller knows
// which file this was.
Scaled scale_to_fit(std::string_view bytes, int max_px, Fit fit,
                    int quality = 85);

// The same, reading the file itself.  A file larger than max_bytes is refused
// on its size alone, without being opened.
Scaled scale_file_to_fit(const std::string& path, int max_px, Fit fit,
                         int quality = 85,
                         std::size_t max_bytes = 64u * 1024 * 1024);

}   // namespace imagescale

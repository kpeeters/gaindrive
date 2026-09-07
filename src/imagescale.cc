#include "imagescale.hh"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <system_error>
#include <vector>

// The implementation macros live here and nowhere else — one translation unit
// owns the code these headers generate.
//
// Only the JPEG and PNG decoders are compiled.  The scanner admits nothing
// else as cover art (COVER_FILENAMES, find_song_cover, IMG_EXT in
// mediastore.cc), so the rest are unreachable, and an unreachable parser of
// hostile input is worth removing rather than carrying.
//
// STBI_MAX_DIMENSIONS matters as much as either.  stb's default is 1<<24, so
// a 30000x30000 PNG header — a few hundred bytes on the wire — would ask an
// HTTP worker thread to allocate several gigabytes before anything noticed.
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO
#define STBI_ONLY_JPEG
#define STBI_ONLY_PNG
#define STBI_FAILURE_USERMSG
#define STBI_MAX_DIMENSIONS 16384
#include <stb_image.h>

#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include <stb_image_resize2.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include <stb_image_write.h>

namespace imagescale {

namespace {

// A second bound below STBI_MAX_DIMENSIONS, on the product rather than either
// edge: 16384x16384 is inside the per-edge limit and is still 800 MB decoded.
constexpr long long MAX_PIXELS = 64'000'000;

const unsigned char PNG_MAGIC[8] = {0x89, 'P', 'N', 'G', 0x0d, 0x0a, 0x1a, 0x0a};

// ---------------------------------------------------------------- EXIF ----
//
// ffmpeg applies EXIF orientation and stb does not, so without this a phone
// photo used as a cover would start appearing sideways after this change —
// the only way the new path could be worse than the fork it replaces.
//
// Everything here is parsing bytes a stranger wrote, so every read is bounds
// checked against the segment it came from and an unparsable tag is simply
// orientation 1.

uint16_t rd16(const unsigned char* p, bool le)
	{
	return le ? static_cast<uint16_t>(p[0] | (p[1] << 8))
	          : static_cast<uint16_t>((p[0] << 8) | p[1]);
	}

uint32_t rd32(const unsigned char* p, bool le)
	{
	uint32_t v = 0;
	for (int i = 0; i < 4; ++i)
		v |= static_cast<uint32_t>(p[le ? i : 3 - i]) << (8 * i);
	return v;
	}

// The IFD0 Orientation tag (0x0112), or 1 when there is none.
int exif_orientation(std::string_view b)
	{
	const auto* d = reinterpret_cast<const unsigned char*>(b.data());
	size_t      n = b.size();
	if (n < 4 || d[0] != 0xFF || d[1] != 0xD8) return 1;   // not a JPEG

	size_t i = 2;
	while (i + 4 <= n) {
		if (d[i] != 0xFF) { ++i; continue; }               // resync past fill
		unsigned char m = d[i + 1];
		if (m == 0xFF) { ++i; continue; }
		// Standalone markers carry no length; SOS means the entropy-coded
		// data starts and no more headers follow.
		if (m == 0xD8 || m == 0x01 || (m >= 0xD0 && m <= 0xD7)) { i += 2; continue; }
		if (m == 0xD9 || m == 0xDA) break;
		if (i + 4 > n) break;
		size_t seg = rd16(d + i + 2, false);
		if (seg < 2 || i + 2 + seg > n) break;

		if (m == 0xE1 && seg >= 8 && std::memcmp(d + i + 4, "Exif\0\0", 6) == 0) {
			const unsigned char* t = d + i + 10;           // TIFF header
			size_t               tn = seg - 8;
			if (tn < 8) return 1;
			bool le;
			if      (t[0] == 'I' && t[1] == 'I') le = true;
			else if (t[0] == 'M' && t[1] == 'M') le = false;
			else return 1;
			if (rd16(t + 2, le) != 42) return 1;
			uint32_t ifd = rd32(t + 4, le);
			if (ifd + 2 > tn) return 1;
			uint16_t count = rd16(t + ifd, le);
			// 12 bytes per entry; the count itself is bounded by the segment.
			if (static_cast<uint64_t>(ifd) + 2 + 12ull * count > tn) return 1;
			for (uint16_t e = 0; e < count; ++e) {
				const unsigned char* ent = t + ifd + 2 + 12 * e;
				if (rd16(ent, le) != 0x0112) continue;
				if (rd16(ent + 2, le) != 3) return 1;       // must be SHORT
				int v = rd16(ent + 8, le);
				return (v >= 1 && v <= 8) ? v : 1;
				}
			return 1;
			}
		i += 2 + seg;
		}
	return 1;
	}

// Rewrites `px` (3 channels) in place for `orient`, swapping w/h for the four
// transposing cases.  Orientation 1 never gets here.
void apply_orientation(std::vector<unsigned char>& px, int& w, int& h, int orient)
	{
	if (orient <= 1 || orient > 8) return;

	const bool swaps = (orient >= 5);
	const int  dw    = swaps ? h : w;
	const int  dh    = swaps ? w : h;

	std::vector<unsigned char> out(static_cast<size_t>(dw) * dh * 3);
	for (int y = 0; y < dh; ++y) {
		for (int x = 0; x < dw; ++x) {
			int sx = 0, sy = 0;
			switch (orient) {
				case 2: sx = w - 1 - x; sy = y;             break;  // mirror H
				case 3: sx = w - 1 - x; sy = h - 1 - y;     break;  // 180
				case 4: sx = x;         sy = h - 1 - y;     break;  // mirror V
				case 5: sx = y;         sy = x;             break;  // transpose
				case 6: sx = y;         sy = h - 1 - x;     break;  // 90 CW
				case 7: sx = w - 1 - y; sy = h - 1 - x;     break;  // transverse
				case 8: sx = w - 1 - y; sy = x;             break;  // 90 CCW
				default: sx = x;        sy = y;             break;
				}
			const unsigned char* s = &px[(static_cast<size_t>(sy) * w + sx) * 3];
			unsigned char*       o = &out[(static_cast<size_t>(y) * dw + x) * 3];
			o[0] = s[0]; o[1] = s[1]; o[2] = s[2];
			}
		}
	px.swap(out);
	w = dw;
	h = dh;
	}

void write_cb(void* ctx, void* data, int size)
	{
	auto* s = static_cast<std::string*>(ctx);
	s->append(static_cast<const char*>(data), static_cast<size_t>(size));
	}

Scaled fail(std::string why)
	{
	Scaled s;
	s.error = std::move(why);
	return s;
	}

}   // namespace

std::string sniff_mime(std::string_view b)
	{
	const auto* d = reinterpret_cast<const unsigned char*>(b.data());
	if (b.size() >= 3 && d[0] == 0xFF && d[1] == 0xD8 && d[2] == 0xFF)
		return "image/jpeg";
	if (b.size() >= 8 && std::memcmp(d, PNG_MAGIC, 8) == 0)
		return "image/png";
	return "";
	}

std::optional<Dims> probe(std::string_view b)
	{
	if (b.empty()) return std::nullopt;
	Dims d;
	if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(b.data()),
	                           static_cast<int>(b.size()),
	                           &d.width, &d.height, &d.channels))
		return std::nullopt;
	return d;
	}

Scaled scale_to_fit(std::string_view b, int max_px, Fit fit, int quality)
	{
	if (b.empty())  return fail("empty input");
	if (max_px < 1) return fail("bad target size");
	quality = std::clamp(quality, 1, 100);

	const auto* data = reinterpret_cast<const stbi_uc*>(b.data());
	const int   len  = static_cast<int>(b.size());

	int sw = 0, sh = 0, comp = 0;
	if (!stbi_info_from_memory(data, len, &sw, &sh, &comp)) {
		const char* r = stbi_failure_reason();
		return fail(r ? r : "unrecognised image");
		}
	if (static_cast<long long>(sw) * sh > MAX_PIXELS)
		return fail("image too large: " + std::to_string(sw) + "x"
		            + std::to_string(sh));

	const int orient = exif_orientation(b);

	// Already small enough and the right way up: hand back what we were given.
	// Re-encoding here would cost a decode and lose quality to buy nothing,
	// and it is what keeps a small PNG cover a PNG.
	//
	// The edge tested has to be the edge Fit names, or the two disagree about
	// what "small enough" means.  Under Fit::Short a 1000x100 banner asked for
	// at 160 cannot reach 160 on its short edge without being enlarged, so it
	// is returned untouched — which is what the no-upscale rule says anyway.
	const int fitted = (fit == Fit::Short) ? std::min(sw, sh) : std::max(sw, sh);
	if (orient == 1 && fitted <= max_px) {
		Scaled s;
		s.ok          = true;
		s.from_source = true;
		s.width       = sw;
		s.height      = sh;
		s.mime        = sniff_mime(b);
		s.bytes.assign(b);
		if (s.mime.empty()) s.mime = "application/octet-stream";
		return s;
		}

	// 2- and 4-channel sources are the ones with alpha; everything else is
	// asked for as RGB, which also expands grayscale for free.
	const int req = (comp == 2 || comp == 4) ? 4 : 3;
	std::unique_ptr<stbi_uc, void (*)(void*)> raw(
		stbi_load_from_memory(data, len, &sw, &sh, &comp, req), stbi_image_free);
	if (!raw) {
		const char* r = stbi_failure_reason();
		return fail(r ? r : "decode failed");
		}

	std::vector<unsigned char> px(static_cast<size_t>(sw) * sh * 3);
	if (req == 4) {
		// Flatten onto white.  stb does not composite, so asking it for 3
		// channels would hand back the raw colour beneath a transparent
		// pixel — usually black, which turns a transparent logo into a black
		// square.  ffmpeg's mjpeg path dropped alpha too, so this is a small
		// improvement rather than a change of behaviour.
		const unsigned char* s = raw.get();
		for (size_t i = 0, n = static_cast<size_t>(sw) * sh; i < n; ++i) {
			const int a = s[i * 4 + 3];
			for (int c = 0; c < 3; ++c)
				px[i * 3 + c] = static_cast<unsigned char>(
					(s[i * 4 + c] * a + 255 * (255 - a)) / 255);
			}
		}
	else {
		std::memcpy(px.data(), raw.get(), px.size());
		}
	raw.reset();

	apply_orientation(px, sw, sh, orient);

	// stbi_load_from_memory re-reads the dimensions, and apply_orientation
	// swaps them for a sideways EXIF tag, so the edge has to be picked again
	// here rather than reused from the early-return test above.
	double f = std::min(1.0, static_cast<double>(max_px)
	                         / ((fit == Fit::Short) ? std::min(sw, sh)
	                                                : std::max(sw, sh)));
	// See LONG_EDGE_LIMIT in the header: Fit::Short is bounded by the aspect
	// ratio and so by nothing, unless the long edge is bounded too.
	if (fit == Fit::Short)
		f = std::min(f, static_cast<double>(LONG_EDGE_LIMIT) * max_px
		                / std::max(sw, sh));

	const int    dw = std::max(1, static_cast<int>(std::lround(sw * f)));
	const int    dh = std::max(1, static_cast<int>(std::lround(sh * f)));

	const unsigned char* src = px.data();
	std::vector<unsigned char> dst;
	if (dw != sw || dh != sh) {
		dst.resize(static_cast<size_t>(dw) * dh * 3);
		// The _srgb entry point, not _linear: cover art is sRGB-encoded, and
		// averaging gamma-encoded values without linearising first darkens
		// every downscale visibly.
		if (!stbir_resize_uint8_srgb(px.data(), sw, sh, 0,
		                             dst.data(), dw, dh, 0, STBIR_RGB))
			return fail("resize failed");
		src = dst.data();
		}

	Scaled out;
	out.bytes.reserve(static_cast<size_t>(dw) * dh / 4);
	if (!stbi_write_jpg_to_func(write_cb, &out.bytes, dw, dh, 3, src, quality)
	    || out.bytes.empty())
		return fail("jpeg encode failed");

	out.ok     = true;
	out.width  = dw;
	out.height = dh;
	out.mime   = "image/jpeg";
	return out;
	}

Scaled scale_file_to_fit(const std::string& path, int max_px, Fit fit,
                         int quality, std::size_t max_bytes)
	{
	namespace fs = std::filesystem;
	std::error_code ec;
	auto            sz = fs::file_size(path, ec);
	if (ec)                 return fail("cannot stat " + path);
	if (sz == 0)            return fail("empty file");
	if (sz > max_bytes)     return fail("file too large: " + std::to_string(sz));

	std::ifstream f(path, std::ios::binary);
	if (!f) return fail("cannot open " + path);
	std::string buf(static_cast<size_t>(sz), '\0');
	f.read(buf.data(), static_cast<std::streamsize>(sz));
	if (static_cast<size_t>(f.gcount()) != sz) return fail("short read on " + path);

	return scale_to_fit(buf, max_px, fit, quality);
	}

}   // namespace imagescale

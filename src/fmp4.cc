#include "fmp4.hh"

#include <cmath>

namespace {

struct Box {
	std::string_view type;
	size_t           body;   // offset of the payload
	size_t           end;    // offset one past the box
	};

uint32_t be32(std::string_view b, size_t o)
	{
	return (uint32_t(uint8_t(b[o])) << 24) | (uint32_t(uint8_t(b[o + 1])) << 16)
	     | (uint32_t(uint8_t(b[o + 2])) << 8) | uint32_t(uint8_t(b[o + 3]));
	}

uint64_t be64(std::string_view b, size_t o)
	{
	return (uint64_t(be32(b, o)) << 32) | be32(b, o + 4);
	}

void put_be32(std::string& b, size_t o, uint32_t v)
	{
	for (int i = 3; i >= 0; --i) { b[o + i] = char(v & 0xff); v >>= 8; }
	}

void put_be64(std::string& b, size_t o, uint64_t v)
	{
	for (int i = 7; i >= 0; --i) { b[o + i] = char(v & 0xff); v >>= 8; }
	}

// Reads the box at `off` inside [.., end).  False at the end or on a size that
// does not fit, which callers treat as "stop" - a truncated tail is the only
// way that happens with ffmpeg's output.
bool read_box(std::string_view b, size_t off, size_t end, Box& out)
	{
	if (off + 8 > end) return false;
	uint64_t size = be32(b, off);
	size_t   hdr  = 8;
	if (size == 1) {
		if (off + 16 > end) return false;
		size = be64(b, off + 8);
		hdr  = 16;
		}
	else if (size == 0)
		size = end - off;
	if (size < hdr || size > end - off) return false;
	out = { b.substr(off + 4, 4), off + hdr, off + size };
	return true;
	}

template <typename F>
void for_each_box(std::string_view b, size_t off, size_t end, F&& f)
	{
	Box box;
	while (read_box(b, off, end, box)) {
		f(box);
		off = box.end;
		}
	}

// mvhd and mdhd share the timescale's position: after version/flags and the
// two creation/modification times, which are 64-bit in version 1.
uint32_t header_timescale(std::string_view b, const Box& box)
	{
	size_t o = box.body + (b[box.body] == 1 ? 20 : 12);
	return o + 4 <= box.end ? be32(b, o) : 0;
	}

}

std::optional<Fmp4Run> fmp4_parse(std::string_view file)
	{
	Fmp4Run  run;
	uint32_t movie_ts = 0;
	bool     have_moof = false;

	for_each_box(file, 0, file.size(), [&](const Box& top) {
		if (top.type == "moof" && !have_moof) {
			run.first_moof = top.body - 8;
			have_moof      = true;
			}
		if (top.type != "moov") return;
		for_each_box(file, top.body, top.end, [&](const Box& mb) {
			if (mb.type == "mvhd") movie_ts = header_timescale(file, mb);
			if (mb.type != "trak") return;

			uint32_t id = 0, ts = 0;
			bool     video = false;
			uint64_t empty = 0;
			int64_t  media = 0;
			for_each_box(file, mb.body, mb.end, [&](const Box& tb) {
				if (tb.type == "tkhd") {
					size_t o = tb.body + (file[tb.body] == 1 ? 20 : 12);
					if (o + 4 <= tb.end) id = be32(file, o);
					}
				if (tb.type == "edts")
					for_each_box(file, tb.body, tb.end, [&](const Box& eb) {
						if (eb.type != "elst" || eb.body + 8 > eb.end) return;
						bool     v1  = file[eb.body] == 1;
						uint32_t n   = be32(file, eb.body + 4);
						size_t   o   = eb.body + 8;
						size_t   len = v1 ? 20 : 12;
						for (uint32_t i = 0; i < n && o + len <= eb.end; ++i, o += len) {
							uint64_t dur = v1 ? be64(file, o) : be32(file, o);
							int64_t  mt  = v1 ? int64_t(be64(file, o + 8))
							                  : int32_t(be32(file, o + 4));
							if (mt == -1) { empty += dur; continue; }
							media = mt;
							break;
							}
						});
				if (tb.type == "mdia")
					for_each_box(file, tb.body, tb.end, [&](const Box& db) {
						if (db.type == "mdhd") ts = header_timescale(file, db);
						if (db.type == "hdlr" && db.body + 12 <= db.end)
							video = file.substr(db.body + 8, 4) == "vide";
						});
				});
			if (id == 0 || ts == 0) return;
			// Only the ratio matters; the movie timescale is known by now
			// because mvhd precedes every trak.
			int64_t start = movie_ts
			    ? int64_t(empty * ts / movie_ts) - media : -media;
			run.tracks[id] = { ts, start };
			if (video && run.video_track == 0) run.video_track = id;
			});
		});

	if (!have_moof || run.tracks.empty() || movie_ts == 0)
		return std::nullopt;
	return run;
	}

double fmp4_video_start(const Fmp4Run& run)
	{
	auto it = run.tracks.find(run.video_track);
	if (it == run.tracks.end()) return 0.0;
	return double(it->second.start) / it->second.timescale;
	}

std::optional<std::string> fmp4_media(std::string_view file, const Fmp4Run& run,
                                      double shift)
	{
	std::string out(file.substr(run.first_moof));
	std::string_view view(out);
	bool ok = true;

	for_each_box(view, 0, view.size(), [&](const Box& top) {
		if (top.type != "moof") return;
		for_each_box(view, top.body, top.end, [&](const Box& traf) {
			if (traf.type != "traf") return;
			const Fmp4Run::Track* track = nullptr;
			for_each_box(view, traf.body, traf.end, [&](const Box& b) {
				if (b.type == "tfhd" && b.body + 8 <= b.end) {
					auto it = run.tracks.find(be32(view, b.body + 4));
					track = it == run.tracks.end() ? nullptr : &it->second;
					}
				if (b.type != "tfdt") return;
				if (!track) { ok = false; return; }
				bool    v1  = view[b.body] == 1;
				int64_t add = track->start
				            + std::llround(shift * track->timescale);
				int64_t t   = int64_t(v1 ? be64(view, b.body + 4)
				                         : be32(view, b.body + 4)) + add;
				// Only ever the first samples of a run that starts at zero with
				// audio priming or a source whose audio leads its video: a few
				// milliseconds that cannot be placed before the film's start.
				if (t < 0) t = 0;
				if (v1) put_be64(out, b.body + 4, uint64_t(t));
				else if (t <= int64_t(UINT32_MAX)) put_be32(out, b.body + 4, uint32_t(t));
				else ok = false;
				});
			});
		});

	if (!ok) return std::nullopt;
	return out;
	}

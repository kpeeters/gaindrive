#include "fmp4.hh"

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

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
	// id -> duration, size, flags, from mvex; merged in once every trak is read.
	std::map<uint32_t, std::array<uint32_t, 3>> trex;

	for_each_box(file, 0, file.size(), [&](const Box& top) {
		if (top.type == "moof" && !have_moof) {
			run.first_moof = top.body - 8;
			have_moof      = true;
			}
		if (top.type != "moov") return;
		for_each_box(file, top.body, top.end, [&](const Box& mb) {
			if (mb.type == "mvhd") movie_ts = header_timescale(file, mb);
			if (mb.type == "mvex")
				for_each_box(file, mb.body, mb.end, [&](const Box& xb) {
					if (xb.type != "trex" || xb.body + 24 > xb.end) return;
					trex[be32(file, xb.body + 4)] = { be32(file, xb.body + 12),
					                                  be32(file, xb.body + 16),
					                                  be32(file, xb.body + 20) };
					});
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
	for (auto& [id, t] : run.tracks)
		if (auto it = trex.find(id); it != trex.end()) {
			t.duration = it->second[0];
			t.size     = it->second[1];
			t.flags    = it->second[2];
			}
	return run;
	}

double fmp4_video_start(const Fmp4Run& run)
	{
	auto it = run.tracks.find(run.video_track);
	if (it == run.tracks.end()) return 0.0;
	return double(it->second.start) / it->second.timescale;
	}

bool fmp4_complete(std::string_view file)
	{
	size_t end  = 0;
	bool   mdat = false;
	for_each_box(file, 0, file.size(), [&](const Box& b) {
		end  = b.end;
		mdat = mdat || b.type == "mdat";
		});
	return mdat && end == file.size();
	}

namespace {

struct Sample {
	uint32_t duration = 0;
	uint32_t size     = 0;
	uint32_t flags    = 0;
	int32_t  cto      = 0;
	size_t   data     = 0;   // offset of its bytes in the file
	};

struct Traf {
	uint32_t            track = 0;
	int64_t             dts   = 0;   // of the first sample, already moved
	std::vector<Sample> samples;
	};

// One traf's samples, with every field resolved: from the trun, else the tfhd,
// else the trex.  Data offsets count from the moof, which is what
// +default_base_moof makes them; a tfhd naming a base of its own is honoured.
bool read_traf(std::string_view file, const Box& traf, size_t moof,
               const Fmp4Run& run, Traf& out)
	{
	const Fmp4Run::Track* track = nullptr;
	uint32_t def_dur = 0, def_size = 0, def_flags = 0;
	size_t   base = moof, next = moof;
	bool     ok = true, have_tfdt = false;
	uint64_t tfdt = 0;

	for_each_box(file, traf.body, traf.end, [&](const Box& b) {
		if (!ok) return;
		if (b.type == "tfhd") {
			if (b.body + 8 > b.end) { ok = false; return; }
			uint32_t fl = be32(file, b.body) & 0xffffff;
			out.track   = be32(file, b.body + 4);
			auto it = run.tracks.find(out.track);
			if (it == run.tracks.end()) { ok = false; return; }
			track     = &it->second;
			def_dur   = track->duration;
			def_size  = track->size;
			def_flags = track->flags;
			size_t o = b.body + 8;
			auto field = [&](uint32_t bit, size_t len, auto set) {
				if (!(fl & bit)) return;
				if (o + len > b.end) { ok = false; return; }
				set(o);
				o += len;
				};
			field(0x01, 8, [&](size_t p) { base = next = size_t(be64(file, p)); });
			field(0x02, 4, [](size_t) {});
			field(0x08, 4, [&](size_t p) { def_dur   = be32(file, p); });
			field(0x10, 4, [&](size_t p) { def_size  = be32(file, p); });
			field(0x20, 4, [&](size_t p) { def_flags = be32(file, p); });
			}
		if (b.type == "tfdt") {
			bool v1 = file[b.body] == 1;
			if (b.body + (v1 ? 12 : 8) > b.end) { ok = false; return; }
			tfdt      = v1 ? be64(file, b.body + 4) : be32(file, b.body + 4);
			have_tfdt = true;
			}
		if (b.type == "trun") {
			if (!track || b.body + 8 > b.end) { ok = false; return; }
			uint32_t fl = be32(file, b.body) & 0xffffff;
			uint32_t n  = be32(file, b.body + 4);
			size_t   o  = b.body + 8;
			if (fl & 0x01) {
				if (o + 4 > b.end) { ok = false; return; }
				next = base + size_t(int32_t(be32(file, o)));
				o += 4;
				}
			uint32_t first_flags = def_flags;
			bool     have_first  = false;
			if (fl & 0x04) {
				if (o + 4 > b.end) { ok = false; return; }
				first_flags = be32(file, o);
				have_first  = true;
				o += 4;
				}
			for (uint32_t i = 0; i < n; ++i) {
				Sample s{ def_dur, def_size, def_flags, 0, next };
				if (i == 0 && have_first) s.flags = first_flags;
				for (uint32_t bit : { 0x100u, 0x200u, 0x400u, 0x800u }) {
					if (!(fl & bit)) continue;
					if (o + 4 > b.end) { ok = false; return; }
					uint32_t v = be32(file, o);
					o += 4;
					if (bit == 0x100) s.duration = v;
					if (bit == 0x200) s.size     = v;
					if (bit == 0x400) s.flags    = v;
					// Signed in version 1, which is what
					// +negative_cts_offsets writes; version 0's are
					// small enough to read the same way.
					if (bit == 0x800) s.cto      = int32_t(v);
					}
				if (s.data + s.size > file.size()) { ok = false; return; }
				next += s.size;
				out.samples.push_back(s);
				}
			}
		});
	if (!ok || !track || !have_tfdt) return false;

	int64_t t = int64_t(tfdt) + track->start;
	out.dts = t;
	return true;
	}

void add32(std::string& o, uint32_t v)
	{
	for (int i = 3; i >= 0; --i) o += char((v >> (8 * i)) & 0xff);
	}

void add64(std::string& o, uint64_t v)
	{
	add32(o, uint32_t(v >> 32));
	add32(o, uint32_t(v));
	}

// Opens a box and returns where its size goes; close_box() fills it in.
size_t open_box(std::string& o, const char* type, uint32_t version_flags,
                bool full)
	{
	size_t at = o.size();
	add32(o, 0);
	o.append(type, 4);
	if (full) add32(o, version_flags);
	return at;
	}

void close_box(std::string& o, size_t at)
	{
	put_be32(o, at, uint32_t(o.size() - at));
	}

}

std::optional<std::string> fmp4_media(std::string_view file, const Fmp4Run& run,
                                      double shift, double end)
	{
	std::string       out;
	bool              ok = true;
	uint32_t          sequence = 0;
	std::map<uint32_t, bool> ended;

	for_each_box(file, run.first_moof, file.size(), [&](const Box& top) {
		if (!ok || top.type != "moof") return;
		const size_t moof = top.body - 8;

		std::vector<Traf> trafs;
		for_each_box(file, top.body, top.end, [&](const Box& b) {
			if (!ok) return;
			if (b.type == "mfhd" && b.body + 8 <= b.end)
				sequence = be32(file, b.body + 4);
			if (b.type != "traf") return;
			Traf t;
			if (!read_traf(file, b, moof, run, t)) { ok = false; return; }
			trafs.push_back(std::move(t));
			});
		if (!ok) return;

		// Move each traf to the film's time, and keep its samples up to the
		// first one presented at or after `end`.  Decode order, so a
		// B-frame after the cut is lost with the keyframe it depends on.
		for (auto& t : trafs) {
			const auto& tr = run.tracks.at(t.track);
			t.dts += std::llround(shift * tr.timescale);
			// Only ever the first samples of a run that starts at zero with
			// audio priming, or of a source whose audio leads its video: a
			// few milliseconds that cannot be placed before the film's start.
			if (t.dts < 0) t.dts = 0;
			int64_t dts  = t.dts;
			size_t  keep = 0;
			for (const auto& s : t.samples) {
				if (ended[t.track]) break;
				if (double(dts + s.cto) / tr.timescale >= end) {
					ended[t.track] = true;
					break;
					}
				dts += s.duration;
				++keep;
				}
			t.samples.resize(keep);
			}
		trafs.erase(std::remove_if(trafs.begin(), trafs.end(),
		                           [](const Traf& t) { return t.samples.empty(); }),
		            trafs.end());
		if (trafs.empty()) return;

		// moof: mfhd, then per track a tfhd naming only the track (so the
		// data offsets count from this moof), a version 1 tfdt, and one
		// version 1 trun (signed composition offsets) spelling out every
		// sample.
		std::string moof_box;
		size_t      moof_at = open_box(moof_box, "moof", 0, false);
		size_t      mfhd_at = open_box(moof_box, "mfhd", 0, true);
		add32(moof_box, sequence);
		close_box(moof_box, mfhd_at);
		std::vector<size_t> offset_at;
		for (const auto& t : trafs) {
			size_t traf_at = open_box(moof_box, "traf", 0, false);
			size_t tfhd_at = open_box(moof_box, "tfhd", 0x020000, true);
			add32(moof_box, t.track);
			close_box(moof_box, tfhd_at);
			size_t tfdt_at = open_box(moof_box, "tfdt", 0x01000000, true);
			add64(moof_box, uint64_t(t.dts));
			close_box(moof_box, tfdt_at);
			size_t trun_at = open_box(moof_box, "trun", 0x01000f01, true);
			add32(moof_box, uint32_t(t.samples.size()));
			offset_at.push_back(moof_box.size());
			add32(moof_box, 0);
			for (const auto& s : t.samples) {
				add32(moof_box, s.duration);
				add32(moof_box, s.size);
				add32(moof_box, s.flags);
				add32(moof_box, uint32_t(s.cto));
				}
			close_box(moof_box, trun_at);
			close_box(moof_box, traf_at);
			}
		close_box(moof_box, moof_at);

		// mdat: each traf's samples in turn, where the offsets now say.
		size_t data = moof_box.size() + 8;
		for (size_t i = 0; i < trafs.size(); ++i) {
			put_be32(moof_box, offset_at[i], uint32_t(data));
			for (const auto& s : trafs[i].samples) data += s.size;
			}
		out += moof_box;
		size_t mdat_at = open_box(out, "mdat", 0, false);
		for (const auto& t : trafs)
			for (const auto& s : t.samples)
				out.append(file.data() + s.data, s.size);
		close_box(out, mdat_at);
		});

	if (!ok || out.empty()) return std::nullopt;
	return out;
	}

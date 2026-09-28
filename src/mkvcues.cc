#include "mkvcues.hh"

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <functional>
#include <map>

namespace {

constexpr uint32_t EBML_HEADER = 0x1A45DFA3;
constexpr uint32_t SEGMENT     = 0x18538067;
constexpr uint32_t SEEK_HEAD   = 0x114D9B74;
constexpr uint32_t SEEK        = 0x4DBB;
constexpr uint32_t SEEK_ID     = 0x53AB;
constexpr uint32_t SEEK_POS    = 0x53AC;
constexpr uint32_t INFO        = 0x1549A966;
constexpr uint32_t TS_SCALE    = 0x2AD7B1;
constexpr uint32_t TRACKS      = 0x1654AE6B;
constexpr uint32_t TRACK_ENTRY = 0xAE;
constexpr uint32_t TRACK_NUM   = 0xD7;
constexpr uint32_t TRACK_TYPE  = 0x83;
constexpr uint32_t CUES        = 0x1C53BB6B;
constexpr uint32_t CUE_POINT   = 0xBB;
constexpr uint32_t CUE_TIME    = 0xB3;
constexpr uint32_t CUE_POS     = 0xB7;
constexpr uint32_t CUE_TRACK   = 0xF7;
constexpr uint32_t CLUSTER     = 0x1F43B675;

constexpr uint64_t UNKNOWN_SIZE = ~uint64_t(0);
// Cues at one point per keyframe are a few MB for the longest film; anything
// much larger is not an index this was written to read.
constexpr uint64_t MAX_ELEMENT  = 64ull << 20;
// A file with no SeekHead entry for its Cues is walked cluster by cluster to
// find them.  One small read each, but bounded all the same.
constexpr int      MAX_WALK     = 200000;

// An EBML variable-length integer.  IDs keep their length marker, sizes lose it,
// and a size of all ones is "unknown", which only a live stream writes.
bool vint(const uint8_t* p, size_t avail, bool id, uint64_t& val, size_t& len)
	{
	if (avail == 0 || p[0] == 0) return false;
	uint8_t mask = 0x80;
	len = 1;
	while (!(p[0] & mask)) { mask >>= 1; ++len; }
	if (len > avail || (id && len > 4)) return false;
	bool ones = (p[0] & (mask - 1)) == (mask - 1);
	val = id ? p[0] : (p[0] & (mask - 1));
	for (size_t i = 1; i < len; ++i) {
		val  = (val << 8) | p[i];
		ones = ones && p[i] == 0xff;
		}
	if (!id && ones) val = UNKNOWN_SIZE;
	return true;
	}

uint64_t read_uint(const uint8_t* p, size_t n)
	{
	uint64_t v = 0;
	for (size_t i = 0; i < n && i < 8; ++i) v = (v << 8) | p[i];
	return v;
	}

// Calls f(id, data, size) for each child in a body held in memory.
void children(const std::string& body,
              const std::function<void(uint32_t, const uint8_t*, size_t)>& f)
	{
	auto   p   = reinterpret_cast<const uint8_t*>(body.data());
	size_t off = 0;
	while (off < body.size()) {
		uint64_t id, size;
		size_t   il, sl;
		if (!vint(p + off, body.size() - off, true, id, il)) return;
		if (!vint(p + off + il, body.size() - off - il, false, size, sl)) return;
		off += il + sl;
		if (size > body.size() - off) return;
		f(uint32_t(id), p + off, size_t(size));
		off += size;
		}
	}

std::string sub(const uint8_t* p, size_t n)
	{
	return std::string(reinterpret_cast<const char*>(p), n);
	}

struct Reader {
	std::ifstream in;
	uint64_t      file_size = 0;

	struct Head { uint32_t id; uint64_t size; uint64_t body; };

	bool head(uint64_t pos, Head& h)
		{
		uint8_t buf[12];
		if (pos >= file_size) return false;
		size_t n = size_t(std::min<uint64_t>(sizeof(buf), file_size - pos));
		in.clear();
		in.seekg(std::streamoff(pos));
		if (!in.read(reinterpret_cast<char*>(buf), std::streamsize(n))) return false;
		uint64_t id, size;
		size_t   il, sl;
		if (!vint(buf, n, true, id, il) || !vint(buf + il, n - il, false, size, sl))
			return false;
		h = { uint32_t(id), size, pos + il + sl };
		return true;
		}

	bool body(const Head& h, std::string& out)
		{
		if (h.size == UNKNOWN_SIZE || h.size > MAX_ELEMENT
		    || h.body + h.size > file_size) return false;
		out.resize(size_t(h.size));
		in.clear();
		in.seekg(std::streamoff(h.body));
		return bool(in.read(out.data(), std::streamsize(h.size)));
		}
	};

}

MkvKeyframes mkv_video_keyframes(const std::string& path)
	{
	MkvKeyframes result;
	Reader       r;
	r.in.open(path, std::ios::binary);
	if (!r.in) return result;
	r.in.seekg(0, std::ios::end);
	r.file_size = uint64_t(r.in.tellg());

	Reader::Head h;
	if (!r.head(0, h) || h.id != EBML_HEADER || h.size == UNKNOWN_SIZE) return result;
	if (!r.head(h.body + h.size, h) || h.id != SEGMENT) return result;
	const uint64_t seg_data = h.body;
	const uint64_t seg_end  = h.size == UNKNOWN_SIZE
	                        ? r.file_size : std::min(r.file_size, h.body + h.size);

	// Element bodies by ID, and where a SeekHead says the others are.
	std::map<uint32_t, std::string> found;
	std::map<uint32_t, uint64_t>    seek;
	auto read_seek_head = [&](const std::string& b) {
		children(b, [&](uint32_t id, const uint8_t* p, size_t n) {
			if (id != SEEK) return;
			uint32_t target = 0;
			uint64_t pos    = UNKNOWN_SIZE;
			children(sub(p, n), [&](uint32_t cid, const uint8_t* cp, size_t cn) {
				if (cid == SEEK_ID)  target = uint32_t(read_uint(cp, cn));
				if (cid == SEEK_POS) pos    = read_uint(cp, cn);
				});
			if (target && pos != UNKNOWN_SIZE && !seek.count(target))
				seek[target] = seg_data + pos;
			});
		};
	auto wanted = [&](uint32_t id) {
		return id == INFO || id == TRACKS || id == CUES;
		};
	auto have_all = [&] {
		return found.count(INFO) && found.count(TRACKS) && found.count(CUES);
		};

	// The top-level elements before the first Cluster, where the SeekHead,
	// Info and Tracks always are.  Cues usually come after the media, and the
	// SeekHead says where.
	uint64_t pos  = seg_data;
	int      walk = 0;
	bool     past_media = false;
	while (pos < seg_end && !have_all() && walk++ < MAX_WALK) {
		if (!r.head(pos, h) || h.size == UNKNOWN_SIZE) break;
		if (h.id == CLUSTER && !past_media) {
			past_media = true;
			if (seek.count(CUES)) break;
			}
		std::string b;
		if (h.id == SEEK_HEAD && r.body(h, b)) read_seek_head(b);
		else if (wanted(h.id) && !found.count(h.id) && r.body(h, b))
			found[h.id] = std::move(b);
		pos = h.body + h.size;
		}

	// Anything still missing, from where a SeekHead points, including a
	// second SeekHead at the end of the file.
	if (seek.count(SEEK_HEAD)) {
		std::string b;
		if (r.head(seek[SEEK_HEAD], h) && h.id == SEEK_HEAD && r.body(h, b))
			read_seek_head(b);
		}
	for (uint32_t id : { INFO, TRACKS, CUES }) {
		if (found.count(id) || !seek.count(id)) continue;
		std::string b;
		if (r.head(seek[id], h) && h.id == id && r.body(h, b))
			found[id] = std::move(b);
		}
	if (!have_all()) return result;

	uint64_t scale = 1000000;
	children(found[INFO], [&](uint32_t id, const uint8_t* p, size_t n) {
		if (id == TS_SCALE) scale = read_uint(p, n);
		});
	if (scale == 0) return result;

	uint64_t video = 0;
	children(found[TRACKS], [&](uint32_t id, const uint8_t* p, size_t n) {
		if (id != TRACK_ENTRY || video) return;
		uint64_t num = 0, type = 0;
		children(sub(p, n), [&](uint32_t cid, const uint8_t* cp, size_t cn) {
			if (cid == TRACK_NUM)  num  = read_uint(cp, cn);
			if (cid == TRACK_TYPE) type = read_uint(cp, cn);
			});
		if (type == 1) video = num;
		});
	if (!video) return result;

	result.tick = double(scale) * 1e-9;
	children(found[CUES], [&](uint32_t id, const uint8_t* p, size_t n) {
		if (id != CUE_POINT) return;
		uint64_t time = UNKNOWN_SIZE;
		bool     ours = false;
		children(sub(p, n), [&](uint32_t cid, const uint8_t* cp, size_t cn) {
			if (cid == CUE_TIME) time = read_uint(cp, cn);
			if (cid != CUE_POS) return;
			children(sub(cp, cn), [&](uint32_t tid, const uint8_t* tp, size_t tn) {
				if (tid == CUE_TRACK && read_uint(tp, tn) == video) ours = true;
				});
			});
		if (ours && time != UNKNOWN_SIZE)
			result.times.push_back(double(time) * result.tick);
		});
	std::sort(result.times.begin(), result.times.end());
	result.times.erase(std::unique(result.times.begin(), result.times.end()),
	                   result.times.end());
	return result;
	}

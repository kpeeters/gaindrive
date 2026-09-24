#include "videoart.hh"
#include "stamp.hh"

#include <iostream>

#include <nlohmann/json.hpp>
#include <reproc++/reproc.hpp>
#include <reproc++/drain.hpp>

// A frame grab that has not finished in this long is not going to.  Generous,
// because the seek is an input seek on a file that may live on a slow disk, but
// bounded, because this runs inside a scan and a corrupt file must not stall it.
static constexpr reproc::milliseconds ART_TIMEOUT(60 * 1000);

// Where in the film to look.  Ten per cent is far enough past studio logos and
// opening titles to be about the film, and early enough that the seek is cheap.
static constexpr double SEEK_FRACTION = 0.10;

// How many frames the thumbnail filter scores before picking one.  It buffers
// them all, so this is a memory cost as well as a decode cost.
static constexpr int THUMBNAIL_FRAMES = 50;

// Image codecs we are willing to lift out of a container as-is.  Anything else
// falls through to the frame grab rather than being stored as something no
// browser will render.
static std::string mime_for_codec(const std::string& codec)
	{
	if (codec == "mjpeg" || codec == "jpeg") return "image/jpeg";
	if (codec == "png")                      return "image/png";
	return "";
	}

// ffprobe reports numbers as JSON strings in some fields and as numbers in
// others; the same problem probe_video() has in mediastore.cc.
static double num_of(const nlohmann::json& j, const char* key)
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

// Caps the long edge without ever scaling up.  The commas inside min() are
// escaped because a bare comma separates filters in a filtergraph - unescaped,
// ffmpeg reads "min(640" as a whole filter name and refuses the argument.
static std::string scale_filter(int max_px)
	{
	std::string m = std::to_string(max_px);
	return "scale=min(" + m + "\\,iw):-2";
	}

VideoArt::VideoArt(int max_px, bool allow_frames, bool allow_embedded)
	: max_px_(max_px > 0 ? max_px : 640), allow_frames_(allow_frames),
	  allow_embedded_(allow_embedded)
	{
	}

std::optional<std::string> VideoArt::run(const std::vector<std::string>& argv)
	{
	reproc::process proc;
	reproc::options opts;
	// Nothing here drains stderr, so it must not be a pipe: a chatty ffmpeg
	// would block forever once the pipe buffer filled.
	opts.redirect.err.type = reproc::redirect::type::discard;
	opts.deadline          = ART_TIMEOUT;

	if (auto ec = proc.start(argv, opts)) {
		std::cout << stamp() << "video art: launch failed: " << ec.message()
		          << std::endl;
		return std::nullopt;
		}

	std::string          out;
	reproc::sink::string sink(out);
	// drain() checks the error code itself.  Reading by hand needs
	// `n == 0 || err`, because reproc wraps a negative return into size_t, a
	// known reproc++ pitfall.
	auto ec = reproc::drain(proc, sink, reproc::sink::null);
	if (ec) {
		proc.kill();
		proc.wait(reproc::infinite);
		std::cout << stamp() << "video art: read failed: " << ec.message()
		          << std::endl;
		return std::nullopt;
		}

	auto [status, wec] = proc.wait(reproc::infinite);
	if (wec || status != 0 || out.empty()) return std::nullopt;
	return out;
	}

std::optional<VideoArt::Probe> VideoArt::probe(const std::string& input) const
	{
	auto out = run({ "ffprobe", "-v", "quiet", "-print_format", "json",
	                 "-show_format", "-show_streams", input });
	if (!out) return std::nullopt;

	Probe p;
	try {
		auto j = nlohmann::json::parse(*out);

		if (auto f = j.find("format"); f != j.end())
			p.duration = num_of(*f, "duration");

		auto streams = j.find("streams");
		if (streams == j.end() || !streams->is_array()) return std::nullopt;

		for (const auto& s : *streams) {
			auto type  = s.value("codec_type", std::string());
			auto codec = s.value("codec_name", std::string());
			int  index = static_cast<int>(num_of(s, "index"));

			// An embedded cover is a video stream flagged attached_pic - the
			// same flag probe_video() checks so it does not describe an m4a
			// cover as the movie.  A Matroska cover attachment arrives here
			// too; see the note in videoart.hh.
			if (type != "video" || p.attached_pic >= 0) continue;
			int attached = 0;
			if (auto d = s.find("disposition"); d != s.end())
				attached = d->value("attached_pic", 0);
			if (attached && !mime_for_codec(codec).empty()) {
				p.attached_pic    = index;
				p.attached_codec  = codec;
				p.attached_width  = static_cast<int>(num_of(s, "width"));
				p.attached_height = static_cast<int>(num_of(s, "height"));
				}
			}

		// Some containers put the duration on the stream rather than on the
		// format, and a frame grab without one has nowhere to seek to.  The
		// cover stream is skipped: a still picture reports a duration of one
		// frame, which would send the seek to the very start of the film.
		if (p.duration <= 0)
			for (const auto& s : *streams) {
				if (s.value("codec_type", std::string()) != "video") continue;
				if (static_cast<int>(num_of(s, "index")) == p.attached_pic)
					continue;
				p.duration = num_of(s, "duration");
				if (p.duration > 0) break;
				}
		}
	catch (const std::exception& e) {
		std::cout << stamp() << "video art: unreadable ffprobe output for "
		          << input << ": " << e.what() << std::endl;
		return std::nullopt;
		}

	return p;
	}

// An embedded cover is passed through untouched when it is already a sensible
// size - it was chosen by whoever made the file and re-encoding it can only
// lose.  An oversized one is scaled, because these blobs live in the database
// and a 3000x3000 poster per film is not a cache, it is a liability.
std::optional<VideoArtResult> VideoArt::from_embedded(const std::string& input,
                                                       const Probe& p) const
	{
	bool rescale = p.attached_width > max_px_ || p.attached_height > max_px_;

	std::vector<std::string> argv = {
		"ffmpeg", "-v", "quiet", "-i", input,
		"-map", "0:" + std::to_string(p.attached_pic), "-frames:v", "1"
		};
	if (rescale)
		argv.insert(argv.end(), { "-vf", scale_filter(max_px_),
		                          "-q:v", "4", "-f", "mjpeg" });
	else
		argv.insert(argv.end(), { "-c", "copy", "-f", "image2pipe" });
	argv.push_back("pipe:1");

	auto out = run(argv);
	if (!out) return std::nullopt;
	return VideoArtResult{ std::move(*out),
	                       rescale ? "image/jpeg"
	                               : mime_for_codec(p.attached_codec),
	                       "embedded" };
	}

std::optional<VideoArtResult> VideoArt::from_frame(const std::string& input,
                                                    double duration) const
	{
	std::vector<std::string> argv = { "ffmpeg", "-v", "quiet" };
	// -ss before -i is an input seek, which is what keeps this cheap on a
	// multi-gigabyte file.  With no duration there is nowhere to seek to, so
	// take the opening and accept that it may be a logo.
	if (duration > 0)
		argv.insert(argv.end(),
			{ "-ss", std::to_string(duration * SEEK_FRACTION) });
	argv.insert(argv.end(), { "-i", input });
	// 0:V:0 is the first video stream *excluding* attached pictures, so a file
	// that has both does not yield its embedded cover a second time here.
	argv.insert(argv.end(), { "-map", "0:V:0", "-an", "-sn", "-dn" });
	argv.insert(argv.end(), { "-vf",
		"thumbnail=" + std::to_string(THUMBNAIL_FRAMES) + ","
		+ scale_filter(max_px_) });
	argv.insert(argv.end(),
		{ "-frames:v", "1", "-q:v", "4", "-f", "mjpeg", "pipe:1" });

	auto out = run(argv);
	if (!out) return std::nullopt;
	return VideoArtResult{ std::move(*out), "image/jpeg", "frame" };
	}

std::optional<VideoArtResult> VideoArt::generate(
	const std::string& path, const std::string& ffmpeg_input) const
	{
	const std::string& input = ffmpeg_input.empty() ? path : ffmpeg_input;

	// Before the probe, which is the cost. With every tier off this class has
	// nothing to say about any file, and asking it per video per scan is the
	// ffprobe run the caller is trying to avoid.
	if (!enabled()) return std::nullopt;

	auto p = probe(input);
	if (!p) {
		std::cout << stamp() << "video art: ffprobe failed for " << path
		          << std::endl;
		return std::nullopt;
		}

	// Falling through to a frame grab when the embedded cover fails to extract
	// is deliberate: a frame is a worse cover than the one the file came with,
	// but it beats no cover at all.
	if (allow_embedded_ && p->attached_pic >= 0)
		if (auto art = from_embedded(input, *p)) return art;

	// Both tiers are off unless asked for. See videoart.hh: a frame belongs
	// after an online lookup rather than instead of one, and an embedded cover
	// is rare enough in practice that probing every file to find one costs
	// more than it returns.
	if (!allow_frames_) return std::nullopt;

	return from_frame(input, p->duration);
	}

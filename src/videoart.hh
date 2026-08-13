#pragma once

#include <optional>
#include <string>
#include <vector>

// Cover art for a video file, derived from the file itself.
//
// Video containers carry no equivalent of an ID3 or Vorbis tag that anything
// actually writes, so the art has to be manufactured.  Two local tiers, in
// descending order of fidelity: a cover image already embedded in the file,
// and failing that a representative frame.  Neither needs a network round
// trip, an API key or a correctly named file.
//
// **The frame tier is off by default** (`allow_frames`).  A frame is a fine
// cover for a home video and a poor one for a film or a documentary, where
// what is wanted is the poster — so it belongs *after* an online lookup, as
// the thing that runs only when nothing else could identify the file at all.
// Until that lookup exists, a frame grab would be the only tier reached for
// most of a collection, which is worse than showing no art. The code stays
// because that last-resort position is where it is going.
//
// There is deliberately no third tier for Matroska cover *attachments*.  They
// look like a separate mechanism (a whole file carried in the container, which
// ffmpeg dumps with -dump_attachment rather than maps as a stream) but ffmpeg
// presents an attachment carrying an image as an ordinary video stream with
// attached_pic set, exactly like an MP4 covr atom — verified against ffmpeg
// 6.1.  The muxer also refuses to write an image attachment with no mimetype,
// so the ambiguous case cannot exist in a file.  Tier 1 therefore already
// covers Matroska, and -dump_attachment would only add a temp file and a
// misleading exit status (it writes the file and *then* fails, complaining
// that no output file was given).
//
// This class knows nothing about the database, the media store or HTTP.  It
// takes a path and returns bytes, so the tier logic can be changed, tested from
// the command line (--video-art-test) or replaced by an online lookup without
// touching anything else.
struct VideoArtResult
	{
	std::string bytes;    // the encoded image
	std::string mime;     // "image/jpeg" or "image/png"
	std::string source;   // which tier produced it; stored so a bad batch of
	                      // one kind can be found and regenerated
	};

class VideoArt
	{
	public:
		// max_px bounds the long edge of a generated image.  An embedded cover
		// is passed through untouched unless it exceeds it: it was chosen by
		// whoever made the file and re-encoding it can only lose.
		//
		// allow_frames enables the frame-grab tier — see the note above for
		// why it defaults off.
		explicit VideoArt(int max_px = 640, bool allow_frames = false);

		bool frames_allowed() const { return allow_frames_; }

		// Empty when every enabled tier failed, which is never fatal — the
		// caller just has no cover, exactly as before.  Never throws.
		//
		// ffmpeg_input differs from `path` only for a DVD titleset, where the
		// caller passes dvd_input(first_vob) so the whole concat: list is read
		// rather than the first 1 GB fragment.
		std::optional<VideoArtResult> generate(
			const std::string& path,
			const std::string& ffmpeg_input = "") const;

	private:
		// What one ffprobe run tells us about where art might come from.
		struct Probe
			{
			int         attached_pic    = -1;  // stream index, -1 = none
			std::string attached_codec;
			int         attached_width  = 0;
			int         attached_height = 0;
			double      duration        = 0;
			};

		std::optional<Probe> probe(const std::string& input) const;

		std::optional<VideoArtResult> from_embedded(const std::string& input,
		                                             const Probe& p) const;
		std::optional<VideoArtResult> from_frame(const std::string& input,
		                                          double duration) const;

		// Runs argv and returns its stdout.  Empty output is reported as
		// failure: a zero-length image is indistinguishable from a broken one
		// downstream and would render as a broken image rather than as no art.
		static std::optional<std::string> run(
			const std::vector<std::string>& argv);

		int  max_px_;
		bool allow_frames_;
	};

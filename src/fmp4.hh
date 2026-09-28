#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>

// One ffmpeg run's fragmented-MP4 output, as the HLS path produces it: ftyp,
// then a moov written after the first packets (+delay_moov, so its edit lists
// know where each track really starts), then moof/mdat pairs.
//
// Every run starts its tracks' decode times at zero, whatever part of the film
// it covers - ffmpeg's mov muxer rebases each track to its own first packet and
// records the difference only in the edit list.  Players on the HLS side do not
// read edit lists reliably (MSE ignores empty edits), so the offset is moved
// into the tfdt boxes instead, which every player reads.
struct Fmp4Run {
	struct Track {
		uint32_t timescale = 0;
		// Presentation time of media time zero, in this track's timescale:
		// the empty edits minus the media_time of the first real edit.
		int64_t  start     = 0;
		};
	size_t                    first_moof  = 0;
	uint32_t                  video_track = 0;
	std::map<uint32_t, Track> tracks;
	};

// Empty when the bytes are not the layout above.
std::optional<Fmp4Run> fmp4_parse(std::string_view file);

// Where the run's first video sample is presented, in seconds of the run's own
// timeline.  Relies on +negative_cts_offsets, which gives that sample a
// composition offset of zero.
double fmp4_video_start(const Fmp4Run& run);

// The media part (from the first moof on) with every tfdt moved by the track's
// start plus `shift` seconds.  `shift` is the same for all tracks - ffmpeg
// rebases a whole run at once - and is how the caller puts the run's first
// video sample at the time it knows that sample has in the film.  Empty on a
// structure this does not understand.
std::optional<std::string> fmp4_media(std::string_view file, const Fmp4Run& run,
                                      double shift);

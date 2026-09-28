#pragma once

#include <string>
#include <vector>

// The keyframe index a Matroska file already carries.
struct MkvKeyframes {
	// Presentation times in seconds of the first video track's cue points,
	// ascending.  mkvmerge and ffmpeg both cue every video keyframe; a file
	// cued more sparsely still lists only keyframes, it just has fewer places
	// to cut.
	std::vector<double> times;
	// One tick of the file's TimestampScale, in seconds.  Every timestamp in
	// the file is a whole number of these, which is what lets a caller cut
	// "before time E" without floating-point doubt.
	double              tick = 0.001;
	};

// Reads the SeekHead, Info, Tracks and Cues elements - a few hundred KB at the
// ends of the file, never the clusters in between - so this costs milliseconds
// where a keyframe scan with ffprobe would read the whole film.  Empty `times`
// when the file is not Matroska, has no video track or no Cues, or anything
// about it is not understood: the caller then encodes instead of copying.
MkvKeyframes mkv_video_keyframes(const std::string& path);

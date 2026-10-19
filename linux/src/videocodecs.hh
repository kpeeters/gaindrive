#pragma once

#include <string>

// Whether this machine's GStreamer demuxes the container and decodes both
// codecs, so that the stored file can be asked for as it is. The container is
// the file's suffix, the codecs ffprobe's names as the server reports them;
// anything unknown is a no, and the server converts it instead.
bool gst_plays(const std::string& container, const std::string& video_codec,
               const std::string& audio_codec);

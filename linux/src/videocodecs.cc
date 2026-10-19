#include "videocodecs.hh"
#include "stamp.hh"

#include <gst/gst.h>

#include <iostream>
#include <map>

namespace {
	struct Name {
		const char *name;   // ffprobe's, or a suffix
		const char *gst;    // caps, or an element
	};

	// Caps as the demuxers announce them, so a decoder that accepts these is
	// one playbin would pick.
	const Name video_caps[] = {
		{"h264",       "video/x-h264"},
		{"hevc",       "video/x-h265"},
		{"vp8",        "video/x-vp8"},
		{"vp9",        "video/x-vp9"},
		{"av1",        "video/x-av1"},
		{"mpeg1video", "video/mpeg, mpegversion=(int)1"},
		{"mpeg2video", "video/mpeg, mpegversion=(int)2"},
		{"mpeg4",      "video/mpeg, mpegversion=(int)4"},
		{"msmpeg4v3",  "video/x-msmpeg, msmpegversion=(int)43"},
		{"wmv3",       "video/x-wmv, wmvversion=(int)3"},
		{"vc1",        "video/x-wmv, wmvversion=(int)3, format=(string)WVC1"},
		{"theora",     "video/x-theora"},
	};
	const Name audio_caps[] = {
		{"aac",    "audio/mpeg, mpegversion=(int)4"},
		{"mp3",    "audio/mpeg, mpegversion=(int)1, layer=(int)3"},
		{"mp2",    "audio/mpeg, mpegversion=(int)1, layer=(int)2"},
		{"ac3",    "audio/x-ac3"},
		{"eac3",   "audio/x-eac3"},
		{"dts",    "audio/x-dts"},
		{"truehd", "audio/x-true-hd"},
		{"opus",   "audio/x-opus"},
		{"vorbis", "audio/x-vorbis"},
		{"flac",   "audio/x-flac"},
		{"wmav2",  "audio/x-wma, wmaversion=(int)2"},
	};
	const Name demuxers[] = {
		{"mkv",  "matroskademux"},
		{"webm", "matroskademux"},
		{"mp4",  "qtdemux"},
		{"m4v",  "qtdemux"},
		{"mov",  "qtdemux"},
		{"avi",  "avidemux"},
		{"mpg",  "mpegpsdemux"},
		{"mpeg", "mpegpsdemux"},
		{"wmv",  "asfdemux"},
	};

	bool has_decoder(const char *caps_string)
		{
		GstCaps *caps = gst_caps_from_string(caps_string);
		GList *all = gst_element_factory_list_get_elements(
			GST_ELEMENT_FACTORY_TYPE_DECODER, GST_RANK_MARGINAL);
		GList *fit = gst_element_factory_list_filter(all, caps, GST_PAD_SINK, FALSE);
		bool found = fit!=nullptr;
		gst_plugin_feature_list_free(fit);
		gst_plugin_feature_list_free(all);
		gst_caps_unref(caps);
		return found;
		}

	// Asked of the registry once per codec: it does not change while we run.
	bool decodes(const Name *table, size_t n, const std::string& codec)
		{
		static std::map<std::string, bool> known;
		if(auto it = known.find(codec); it!=known.end()) return it->second;
		bool ok = false;
		for(size_t i=0; i<n; ++i)
			if(codec==table[i].name)
				ok = has_decoder(table[i].gst);
		// Raw PCM needs no decoder, only the demuxer.
		if(codec.rfind("pcm_", 0)==0) ok = true;
		std::cout << stamp() << "GStreamer " << (ok ? "decodes " : "does not decode ")
		          << codec << std::endl;
		known[codec] = ok;
		return ok;
		}
}

bool gst_plays(const std::string& container, const std::string& video_codec,
               const std::string& audio_codec)
	{
	if(video_codec.empty()) return false;
	bool demux = false;
	for(const auto& d: demuxers)
		if(container==d.name) {
			GstElementFactory *f = gst_element_factory_find(d.gst);
			demux = f!=nullptr;
			if(f) gst_object_unref(f);
			}
	return demux
	    && decodes(video_caps, std::size(video_caps), video_codec)
	    && (audio_codec.empty()
	        || decodes(audio_caps, std::size(audio_caps), audio_codec));
	}

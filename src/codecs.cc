#include "codecs.hh"

std::optional<Target> target_for(std::string_view name)
	{
	for (const auto& t : TARGETS)
		if (t.name == name) return t;
	return std::nullopt;
	}

std::optional<VideoTarget> video_target_for(std::string_view ext)
	{
	for (const auto& t : VIDEO_TARGETS)
		if (t.name == ext) return t;
	return std::nullopt;
	}

bool is_video_ext(std::string_view ext)
	{
	return video_target_for(ext).has_value();
	}

bool container_declarable(std::string_view ext)
	{
	return is_video_ext(ext) && ext != "vob";
	}

AudioForm implied_audio_form(std::string_view ext)
	{
	if (ext == "mp3")  return { "mpeg", "mp3"   };
	if (ext == "flac") return { "flac", "flac"  };
	if (ext == "opus") return { "ogg",  "opus"  };
	if (ext == "aac")  return { "adts", "aac"   };
	if (ext == "wav")  return { "riff", "pcm"   };
	if (ext == "wma")  return { "asf",  "wma"   };
	return {};
	}

bool audio_form_needs_read(std::string_view ext)
	{
	return ext == "m4a" || ext == "ogg" || ext == "oga";
	}

std::string_view codec_to_mime(std::string_view codec)
	{
	if (auto t = target_for(codec))       return t->mime;
	if (auto v = video_target_for(codec)) return v->mime;
	return std::string_view("application/octet-stream");
	}

bool browser_video_codec(std::string_view c)
	{
	return c == "h264" || c == "vp8" || c == "vp9" || c == "av1";
	}

bool browser_audio_codec(std::string_view c)
	{
	return c == "aac" || c == "mp3" || c == "opus" || c == "vorbis"
	    || c == "flac";
	}

bool browser_container(std::string_view ext)
	{
	return ext == "mp4" || ext == "m4v" || ext == "webm";
	}

bool video_seeks_natively(std::string_view video_codec,
                          std::string_view audio_codec)
	{
	return browser_video_codec(video_codec)
	    && (audio_codec.empty() || browser_audio_codec(audio_codec));
	}

bool webm_codecs(std::string_view video_codec,
                 std::string_view audio_codec)
	{
	return (video_codec == "vp8" || video_codec == "vp9"
	        || video_codec == "av1")
	    && (audio_codec.empty() || audio_codec == "vorbis"
	        || audio_codec == "opus");
	}

bool video_direct_playable(std::string_view container,
                           std::string_view video_codec,
                           std::string_view audio_codec)
	{
	if (browser_container(container)
	        && video_seeks_natively(video_codec, audio_codec))
		return true;
	return container == "mkv" && webm_codecs(video_codec, audio_codec);
	}

bool video_direct_playable_for(std::string_view container,
                               std::string_view video_codec,
                               std::string_view audio_codec,
                               const Playable& client)
	{
	if (video_direct_playable(container, video_codec, audio_codec)) return true;
	if (!container_declarable(container))       return false;
	if (client.find(container) == client.end()) return false;
	return video_seeks_natively(video_codec, audio_codec);
	}

bool audio_declared(const AudioForm& form, const Playable& client)
	{
	if (form.container.empty() || form.codec.empty()) return false;
	std::string token(form.container);
	token += '/';
	token += form.codec;
	return client.find(token) != client.end();
	}

CastTier cast_tier_for(std::string_view container,
                       std::string_view video_codec,
                       std::string_view audio_codec)
	{
	// Audio is byte-ranged off disk: no format and no ceiling means
	// needs_transcode is false and serve() never reaches a transcode at all.
	// The film-soundtrack case is not this — it puts `format` on the URL, and
	// so is decided by cast_load_song() rather than by the codec pair.
	if (!is_video_ext(container)) return CastTier::Direct;
	if (!video_seeks_natively(video_codec, audio_codec))
		return CastTier::Encode;
	return video_direct_playable(container, video_codec, audio_codec)
	     ? CastTier::Direct : CastTier::Remux;
	}

std::string_view cast_tier_name(CastTier t)
	{
	return t == CastTier::Direct ? std::string_view("direct")
	     : t == CastTier::Remux  ? std::string_view("remux")
	                             : std::string_view("encode");
	}

std::string_view cast_mime_for(std::string_view container,
                               std::string_view video_codec,
                               std::string_view audio_codec)
	{
	if (!is_video_ext(container)) return codec_to_mime(container);
	if (cast_tier_for(container, video_codec, audio_codec) != CastTier::Direct)
		return VIDEO_MP4_MIME;
	if (container == "mkv" && webm_codecs(video_codec, audio_codec))
		return std::string_view("video/webm");
	return codec_to_mime(container);
	}

std::optional<Target> audio_copy_target(std::string_view audio_codec)
	{
	std::string_view name;
	if      (audio_codec == "aac")    name = "m4a";
	else if (audio_codec == "mp3")    name = "mp3";
	else if (audio_codec == "flac")   name = "flac";
	else if (audio_codec == "opus")   name = "opus";
	else if (audio_codec == "vorbis") name = "ogg";
	else return std::nullopt;
	return target_for(name);
	}

bool audio_only_request(bool is_video, std::string_view format,
                        std::string_view audio_codec)
	{
	if (!is_video || audio_codec.empty())  return false;
	if (format.empty() || format == "raw") return false;
	auto t = target_for(format);
	return t && !t->encoder.empty();
	}

std::string cast_soundtrack_format(std::string_view audio_codec)
	{
	auto t = audio_copy_target(audio_codec);
	std::string fmt = t ? std::string(t->name)
	                    : std::string(CAST_AUDIO_ONLY_FORMAT);
	return audio_only_request(true, fmt, audio_codec) ? fmt : std::string();
	}

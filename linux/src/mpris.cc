#include "mpris.hh"
#include "coverstore.hh"
#include "md5.hh"
#include "playback.hh"
#include "stamp.hh"

#include <gio/gio.h>
#include <glibmm/miscutils.h>
#include <sigc++/adaptors/track_obj.h>

#include <algorithm>
#include <filesystem>
#include <iostream>

namespace {
	const char *bus_name    = "org.mpris.MediaPlayer2.gaindrive";
	const char *object_path = "/org/mpris/MediaPlayer2";
	const char *root_iface   = "org.mpris.MediaPlayer2";
	const char *player_iface = "org.mpris.MediaPlayer2.Player";

	const char *introspection = R"(
<node>
  <interface name="org.mpris.MediaPlayer2">
    <method name="Raise"/>
    <method name="Quit"/>
    <property name="CanQuit" type="b" access="read"/>
    <property name="CanRaise" type="b" access="read"/>
    <property name="HasTrackList" type="b" access="read"/>
    <property name="Identity" type="s" access="read"/>
    <property name="DesktopEntry" type="s" access="read"/>
    <property name="SupportedUriSchemes" type="as" access="read"/>
    <property name="SupportedMimeTypes" type="as" access="read"/>
  </interface>
  <interface name="org.mpris.MediaPlayer2.Player">
    <method name="Next"/>
    <method name="Previous"/>
    <method name="Pause"/>
    <method name="PlayPause"/>
    <method name="Stop"/>
    <method name="Play"/>
    <method name="Seek"><arg direction="in" name="Offset" type="x"/></method>
    <method name="SetPosition">
      <arg direction="in" name="TrackId" type="o"/>
      <arg direction="in" name="Position" type="x"/>
    </method>
    <method name="OpenUri"><arg direction="in" name="Uri" type="s"/></method>
    <signal name="Seeked"><arg name="Position" type="x"/></signal>
    <property name="PlaybackStatus" type="s" access="read"/>
    <property name="Rate" type="d" access="readwrite"/>
    <property name="Metadata" type="a{sv}" access="read"/>
    <property name="Volume" type="d" access="readwrite"/>
    <property name="Position" type="x" access="read"/>
    <property name="MinimumRate" type="d" access="read"/>
    <property name="MaximumRate" type="d" access="read"/>
    <property name="CanGoNext" type="b" access="read"/>
    <property name="CanGoPrevious" type="b" access="read"/>
    <property name="CanPlay" type="b" access="read"/>
    <property name="CanPause" type="b" access="read"/>
    <property name="CanSeek" type="b" access="read"/>
    <property name="CanControl" type="b" access="read"/>
  </interface>
</node>)";

	// MPRIS speaks microseconds.
	gint64 usec(double seconds)
		{
		return gint64(seconds*1e6);
		}

	std::filesystem::path art_dir()
		{
		return std::filesystem::path(Glib::get_user_cache_dir()) / "gaindrive-player" / "mpris";
		}

	// Big enough for a shell's now-playing card.
	const int art_px = 300;
}

Mpris::Mpris(Playback& playback, CoverStore& covers, std::function<void()> raise)
	: playback_(playback), covers_(covers), raise_(std::move(raise))
	{
	GError *err = nullptr;
	info_ = g_dbus_node_info_new_for_xml(introspection, &err);
	if(!info_) {
		std::cout << stamp() << "MPRIS: " << err->message << std::endl;
		g_error_free(err);
		return;
		}
	owner_ = g_bus_own_name(G_BUS_TYPE_SESSION, bus_name, G_BUS_NAME_OWNER_FLAGS_NONE,
	                        &Mpris::bus_acquired, nullptr, &Mpris::name_lost, this, nullptr);

	playback_.signal_track.connect(sigc::mem_fun(*this, &Mpris::track_changed));
	playback_.signal_state.connect(sigc::track_obj([this](bool) { changed({"PlaybackStatus"}); }, *this));
	playback_.signal_seeked.connect(sigc::track_obj([this](double seconds) {
		if(conn_)
			g_dbus_connection_emit_signal(conn_, nullptr, object_path, player_iface, "Seeked",
			                              g_variant_new("(x)", usec(seconds)), nullptr);
		}, *this));
	// Tracked, so none of these outlives this object.
	auto volume = sigc::track_obj([this]() { changed({"Volume"}); }, *this);
	playback_.caster().signal_volume.connect(volume);
	playback_.caster().signal_session.connect(volume);
	covers_.signal_ready.connect(sigc::track_obj(
		[this](const std::string& key, const Glib::RefPtr<Gdk::Texture>& tex) {
			if(key==cover_key_) save_art(tex->gobj());
			}, *this));
	}

Mpris::~Mpris()
	{
	if(conn_) {
		if(root_id_)   g_dbus_connection_unregister_object(conn_, root_id_);
		if(player_id_) g_dbus_connection_unregister_object(conn_, player_id_);
		}
	if(owner_) g_bus_unown_name(owner_);
	if(info_)  g_dbus_node_info_unref(info_);
	}

void Mpris::bus_acquired(GDBusConnection *conn, const char *, void *self)
	{
	auto *m = static_cast<Mpris *>(self);
	GError *err = nullptr;
	m->root_id_ = g_dbus_connection_register_object(conn, object_path, m->info_->interfaces[0],
	                                                vtable(), m, nullptr, &err);
	if(m->root_id_)
		m->player_id_ = g_dbus_connection_register_object(conn, object_path, m->info_->interfaces[1],
		                                                  vtable(), m, nullptr, &err);
	if(err) {
		std::cout << stamp() << "MPRIS: " << err->message << std::endl;
		g_error_free(err);
		return;
		}
	m->conn_ = conn;
	}

void Mpris::name_lost(GDBusConnection *conn, const char *, void *)
	{
	// No session bus, or another copy of the player holds the name: media
	// keys then reach that one.
	if(!conn) std::cout << stamp() << "MPRIS: no session bus" << std::endl;
	else      std::cout << stamp() << "MPRIS: " << bus_name << " is taken" << std::endl;
	}

void Mpris::method_call(GDBusConnection *, const char *, const char *, const char *iface,
                        const char *method, GVariant *params, GDBusMethodInvocation *inv, void *self)
	{
	auto *m = static_cast<Mpris *>(self);
	auto& pb = m->playback_;
	std::string name = method;
	if(std::string(iface)==root_iface) {
		if(name=="Raise") m->raise_();
		g_dbus_method_invocation_return_value(inv, nullptr);
		return;
		}
	const Song *s = pb.current();
	if(name=="Next")
		pb.next();
	else if(name=="Previous")
		pb.prev();
	else if(name=="PlayPause")
		pb.toggle();
	else if(name=="Play") {
		if(!pb.playing()) pb.toggle();
		}
	// Stop as a pause: Playback::stop() also ends a Cast session, which is
	// not what a media key is asking for.
	else if(name=="Pause" || name=="Stop") {
		if(pb.playing()) pb.toggle();
		}
	else if(name=="Seek" && s) {
		gint64 offset = 0;
		g_variant_get(params, "(x)", &offset);
		double to = pb.position() + offset/1e6;
		// Past the end means the next track, as the specification asks.
		if(s->duration>0 && to>=s->duration) pb.next();
		else                                 pb.seek(std::max(0.0, to));
		}
	else if(name=="SetPosition" && s) {
		const char *track = nullptr;
		gint64 pos = 0;
		g_variant_get(params, "(&ox)", &track, &pos);
		// Ignored for a track that is no longer the current one.
		if(m->track_path()==track && pos>=0 && (s->duration<=0 || pos<=usec(s->duration)))
			pb.seek(pos/1e6);
		}
	g_dbus_method_invocation_return_value(inv, nullptr);
	}

GVariant *Mpris::get_property(GDBusConnection *, const char *, const char *, const char *iface,
                              const char *prop, GError **, void *self)
	{
	auto *m = static_cast<Mpris *>(self);
	std::string p = prop;
	if(std::string(iface)==player_iface) return m->player_property(p);
	if(p=="CanQuit" || p=="HasTrackList") return g_variant_new_boolean(false);
	if(p=="CanRaise")     return g_variant_new_boolean(true);
	if(p=="Identity")     return g_variant_new_string("GainDrive");
	if(p=="DesktopEntry") return g_variant_new_string(GD_APP_ID);
	return g_variant_new_strv(nullptr, 0);
	}

int Mpris::set_property(GDBusConnection *, const char *, const char *, const char *,
                        const char *prop, GVariant *value, GError **, void *self)
	{
	auto *m = static_cast<Mpris *>(self);
	// A receiver's volume only moves in single steps from its own buttons,
	// so a desktop's volume slider cannot take an amplifier to full.
	if(std::string(prop)=="Volume" && !m->playback_.caster().active()) {
		m->playback_.set_volume(g_variant_get_double(value));
		m->changed({"Volume"});
		}
	return true;
	}

GVariant *Mpris::player_property(const std::string& p)
	{
	auto& pb = playback_;
	const Song *s = pb.current();
	if(p=="PlaybackStatus")
		return g_variant_new_string(!s ? "Stopped" : pb.playing() ? "Playing" : "Paused");
	if(p=="Metadata") return metadata();
	if(p=="Position") return g_variant_new_int64(s ? usec(pb.position()) : 0);
	if(p=="Volume") {
		auto& c = pb.caster();
		if(c.active()) return g_variant_new_double(c.volume_known() ? c.volume() : 1.0);
		return g_variant_new_double(pb.volume());
		}
	if(p=="Rate" || p=="MinimumRate" || p=="MaximumRate") return g_variant_new_double(1.0);
	if(p=="CanGoNext")
		return g_variant_new_boolean(s && pb.index()+1<long(pb.queue().size()));
	if(p=="CanControl") return g_variant_new_boolean(true);
	// Previous restarts the track, so it is always possible with one.
	return g_variant_new_boolean(s!=nullptr);
	}

std::string Mpris::track_path() const
	{
	return "/org/gaindrive/track/" + std::to_string(track_);
	}

GVariant *Mpris::metadata()
	{
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	const Song *s = playback_.current();
	if(!s) {
		// The specification's way of saying "no track".
		g_variant_builder_add(&b, "{sv}", "mpris:trackid",
		                      g_variant_new_object_path("/org/mpris/MediaPlayer2/TrackList/NoTrack"));
		return g_variant_builder_end(&b);
		}
	g_variant_builder_add(&b, "{sv}", "mpris:trackid", g_variant_new_object_path(track_path().c_str()));
	if(s->duration>0)
		g_variant_builder_add(&b, "{sv}", "mpris:length", g_variant_new_int64(usec(s->duration)));
	g_variant_builder_add(&b, "{sv}", "xesam:title", g_variant_new_string(s->title.c_str()));
	if(!s->artist.empty()) {
		const char *artists[] = { s->artist.c_str(), nullptr };
		g_variant_builder_add(&b, "{sv}", "xesam:artist", g_variant_new_strv(artists, 1));
		}
	if(!s->album.empty())
		g_variant_builder_add(&b, "{sv}", "xesam:album", g_variant_new_string(s->album.c_str()));
	if(!art_path_.empty()) {
		std::string uri = "file://" + art_path_;
		g_variant_builder_add(&b, "{sv}", "mpris:artUrl", g_variant_new_string(uri.c_str()));
		}
	return g_variant_builder_end(&b);
	}

void Mpris::track_changed()
	{
	// Also fired when only the queue after it changed.
	const Song *s = playback_.current();
	std::string id = s ? s->ref.server + "|" + s->ref.id : "";
	if(id!=song_id_) {
		song_id_ = id;
		++track_;
		art_path_.clear();
		cover_key_.clear();
		if(s && !s->cover_art.empty()) {
			auto tex = covers_.get(s->cover_art, art_px, cover_key_);
			if(tex) save_art(tex->gobj());
			}
		}
	changed({"Metadata", "PlaybackStatus", "CanGoNext", "CanGoPrevious", "CanPlay",
	         "CanPause", "CanSeek"});
	}

void Mpris::save_art(GdkTexture *tex)
	{
	// A new name per cover: a shell that has shown a file once may not read
	// the same name again.
	std::error_code ec;
	std::filesystem::create_directories(art_dir(), ec);
	auto path = (art_dir() / (md5_hex(cover_key_) + ".png")).string();
	if(!gdk_texture_save_to_png(tex, path.c_str())) {
		std::cout << stamp() << "MPRIS: cannot write " << path << std::endl;
		return;
		}
	if(!art_path_.empty() && art_path_!=path)
		std::filesystem::remove(art_path_, ec);
	art_path_ = path;
	changed({"Metadata"});
	}

void Mpris::changed(std::initializer_list<const char *> props)
	{
	if(!conn_) return;
	GVariantBuilder b;
	g_variant_builder_init(&b, G_VARIANT_TYPE("a{sv}"));
	for(const char *p: props)
		g_variant_builder_add(&b, "{sv}", p, player_property(p));
	g_dbus_connection_emit_signal(conn_, nullptr, object_path, "org.freedesktop.DBus.Properties",
	                              "PropertiesChanged",
	                              g_variant_new("(sa{sv}as)", player_iface, &b, nullptr), nullptr);
	}

const GDBusInterfaceVTable *Mpris::vtable()
	{
	static const GDBusInterfaceVTable v = { &Mpris::method_call, &Mpris::get_property,
	                                        &Mpris::set_property, {} };
	return &v;
	}

#pragma once

#include <functional>
#include <initializer_list>
#include <string>

#include <sigc++/trackable.h>

typedef struct _GDBusConnection GDBusConnection;
typedef struct _GDBusNodeInfo   GDBusNodeInfo;
typedef struct _GVariant        GVariant;
typedef struct _GdkTexture      GdkTexture;
typedef struct _GDBusInterfaceVTable GDBusInterfaceVTable;

class CoverStore;
class Playback;

// MPRIS2 on the session bus, so media keys, the desktop's now-playing
// widget and playerctl see and control what plays. Main thread only, which
// is where GDBus delivers the calls.
class Mpris : public sigc::trackable {
	public:
		Mpris(Playback& playback, CoverStore& covers, std::function<void()> raise);
		~Mpris();

	private:
		static const GDBusInterfaceVTable *vtable();
		static void bus_acquired(GDBusConnection *conn, const char *name, void *self);
		static void name_lost(GDBusConnection *conn, const char *name, void *self);
		static void method_call(GDBusConnection *conn, const char *sender, const char *path,
		                        const char *iface, const char *method, GVariant *params,
		                        struct _GDBusMethodInvocation *inv, void *self);
		static GVariant *get_property(GDBusConnection *conn, const char *sender, const char *path,
		                              const char *iface, const char *prop, struct _GError **error,
		                              void *self);
		static int set_property(GDBusConnection *conn, const char *sender, const char *path,
		                        const char *iface, const char *prop, GVariant *value,
		                        struct _GError **error, void *self);

		GVariant *player_property(const std::string& prop);
		GVariant *metadata();
		void      track_changed();
		void      save_art(GdkTexture *tex);
		// PropertiesChanged on the Player interface, for these names.
		void      changed(std::initializer_list<const char *> props);
		std::string track_path() const;

		Playback&             playback_;
		CoverStore&           covers_;
		std::function<void()> raise_;
		unsigned              owner_=0;
		GDBusConnection      *conn_=nullptr;
		GDBusNodeInfo        *info_=nullptr;
		unsigned              root_id_=0, player_id_=0;

		// Bumped per track, for mpris:trackid.
		unsigned              track_=0;
		std::string           song_id_;
		// The cover as a file, for the shell: a getCoverArt URL would hand
		// it the account's credentials.
		std::string           cover_key_, art_path_;
};

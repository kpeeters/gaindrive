#include "playlistpicker.hh"
#include "apiclient.hh"
#include "jsonread.hh"
#include "model.hh"

#include <gtkmm/box.h>
#include <gtkmm/button.h>
#include <gtkmm/entry.h>
#include <gtkmm/label.h>
#include <gtkmm/popover.h>
#include <gtkmm/separator.h>
#include <gtkmm/spinner.h>

void pick_playlist(Gtk::Widget& anchor, std::shared_ptr<ApiClient> api, const Song& song,
                   std::function<void(const ApiResult&)> failed)
	{
	if(!api) return;
	auto pop = Gtk::make_managed<Gtk::Popover>();
	auto box = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL, 4);
	auto heading = Gtk::make_managed<Gtk::Label>("Add to playlist");
	heading->add_css_class("heading");
	box->append(*heading);
	auto lists = Gtk::make_managed<Gtk::Box>(Gtk::Orientation::VERTICAL);
	auto spinner = Gtk::make_managed<Gtk::Spinner>();
	spinner->set_spinning(true);
	lists->append(*spinner);
	box->append(*lists);
	box->append(*Gtk::make_managed<Gtk::Separator>());
	auto name = Gtk::make_managed<Gtk::Entry>();
	name->set_placeholder_text("New playlist");
	box->append(*name);
	pop->set_child(*box);
	pop->set_parent(anchor);

	// The popover may be closed, and so destroyed, before a reply arrives.
	auto alive = std::make_shared<bool>(true);
	pop->signal_closed().connect([pop, alive]() {
		*alive = false;
		// Unparenting drops the last reference, which destroys it.
		pop->unparent();
		});
	auto done = [pop, alive, failed](const ApiResult& r) {
		if(!r.ok) failed(r);
		if(*alive) pop->popdown();
		};

	std::string song_id = song.ref.id;
	name->signal_activate().connect([api, name, song_id, done]() {
		if(name->get_text().empty()) return;
		api->call("createPlaylist", {{"name", name->get_text()}, {"songId", song_id}}, done);
		});

	api->call("getPlaylists", {}, [api, lists, spinner, alive, song_id, done](const ApiResult& r) {
		if(!*alive) return;
		lists->remove(*spinner);
		const auto& arr = jsub(jsub(r.body, "playlists"), "playlist");
		for(size_t i=0; i<arr.size(); ++i) {
			const auto& pl = jidx(arr, i);
			// Only the owner may add to a playlist.
			if(jstr(pl, "owner")!=api->session().user) continue;
			std::string id = jstr(pl, "id");
			auto b = Gtk::make_managed<Gtk::Button>(jstr(pl, "name"));
			b->add_css_class("flat");
			b->signal_clicked().connect([api, id, song_id, done]() {
				api->call("updatePlaylist", {{"playlistId", id}, {"songIdToAdd", song_id}}, done);
				});
			lists->append(*b);
			}
		});
	pop->popup();
	}

#pragma once

#include <functional>
#include <memory>

namespace Gtk { class Widget; }
class ApiClient;
struct ApiResult;
struct Song;

// A popover on `anchor` listing the user's playlists, plus a field for a new
// one, adding `song` to whichever is picked. It removes itself when closed.
void pick_playlist(Gtk::Widget& anchor, std::shared_ptr<ApiClient> api, const Song& song,
                   std::function<void(const ApiResult&)> failed);

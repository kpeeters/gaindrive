#include "merge.hh"

#include <glibmm/ustring.h>

#include <map>

namespace {
	std::string album_key(const Album& a)
		{
		Glib::ustring out;
		for(auto c: Glib::ustring(a.artist + "\n" + a.title).casefold())
			if(g_unichar_isalnum(c) || c=='\n') out += c;
		return out;
		}

	bool on_server(const ItemRef& ref, const std::vector<ItemRef>& also, const std::string& server)
		{
		if(ref.server==server) return true;
		for(const auto& r: also)
			if(r.server==server) return true;
		return false;
		}

	// `fold` adds the second's numbers to the first.
	template<class T, class Key, class Fold>
	std::vector<T> merge(const std::vector<std::vector<T>>& lists, Key key, Fold fold)
		{
		std::vector<T> out;
		std::multimap<std::string, size_t> seen;
		for(const auto& list: lists)
			for(const auto& item: list) {
				auto k = key(item);
				bool folded = false;
				auto [from, to] = seen.equal_range(k);
				for(auto it=from; it!=to && !folded; ++it) {
					T& into = out[it->second];
					if(on_server(into.ref, into.also, item.ref.server)) continue;
					into.also.push_back(item.ref);
					fold(into, item);
					folded = true;
					}
				if(folded) continue;
				seen.emplace(k, out.size());
				out.push_back(item);
				}
		return out;
		}
}

std::string artist_key(const std::string& name)
	{
	auto from = name.find_first_not_of(" \t");
	auto to   = name.find_last_not_of(" \t");
	if(from==std::string::npos) return "";
	return Glib::ustring(name.substr(from, to-from+1)).casefold();
	}

std::vector<Artist> merge_artists(const std::vector<std::vector<Artist>>& lists)
	{
	return merge(lists, [](const Artist& a) { return artist_key(a.name); },
	             [](Artist& into, const Artist& a) { into.album_count += a.album_count; });
	}

std::vector<Album> merge_albums(const std::vector<std::vector<Album>>& lists)
	{
	return merge(lists, album_key, [](Album&, const Album&) {});
	}

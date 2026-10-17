#include "serverconfig.hh"
#include "configfile.hh"
#include "jsonread.hh"

#include <nlohmann/json.hpp>

std::vector<ServerConfig> load_servers()
	{
	std::vector<ServerConfig> out;
	auto j = read_config("servers.json");
	const auto& arr = jsub(j, "servers");
	for(size_t i=0; i<arr.size(); ++i) {
		const auto& s = jidx(arr, i);
		ServerConfig c{jstr(s, "key"), jstr(s, "url"), jstr(s, "user")};
		if(!c.key.empty() && !c.url.empty())
			out.push_back(c);
		}
	return out;
	}

void save_servers(const std::vector<ServerConfig>& servers)
	{
	nlohmann::json arr = nlohmann::json::array();
	for(const auto& s: servers)
		arr.push_back({{"key", s.key}, {"url", s.url}, {"user", s.user}});
	write_config("servers.json", {{"servers", arr}});
	}

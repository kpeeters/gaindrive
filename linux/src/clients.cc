#include "clients.hh"
#include "apiclient.hh"

#include <algorithm>

std::shared_ptr<ApiClient> Clients::get(const std::string& server_key) const
	{
	for(const auto& c: list_)
		if(c->session().server_key==server_key) return c;
	return {};
	}

std::shared_ptr<ApiClient> Clients::first() const
	{
	return list_.empty() ? nullptr : list_.front();
	}

const std::vector<std::shared_ptr<ApiClient>>& Clients::all() const
	{
	return list_;
	}

bool Clients::empty() const
	{
	return list_.empty();
	}

void Clients::set(std::shared_ptr<ApiClient> api, size_t pos)
	{
	for(auto& c: list_)
		if(c->session().server_key==api->session().server_key) {
			c = std::move(api);
			return;
			}
	list_.insert(list_.begin()+long(std::min(pos, list_.size())), std::move(api));
	}

void Clients::drop(const std::string& server_key)
	{
	std::erase_if(list_, [&](const std::shared_ptr<ApiClient>& c) {
		return c->session().server_key==server_key;
		});
	}

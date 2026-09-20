#include "jsonread.hh"

const nlohmann::json& jsub(const nlohmann::json& j,
                           const std::string& key)
	{
	static const nlohmann::json none;
	if (!j.is_object()) return none;
	auto it = j.find(key);
	return it == j.end() ? none : *it;
	}

const nlohmann::json& jidx(const nlohmann::json& j, size_t i)
	{
	static const nlohmann::json none;
	if (!j.is_array() || i >= j.size()) return none;
	return j[i];
	}

std::string jstr(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_string() ? v.get<std::string>() : std::string();
	}

double jnum(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_number() ? v.get<double>() : 0.0;
	}

int jint(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_number_integer() ? v.get<int>() : 0;
	}

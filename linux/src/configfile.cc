#include "configfile.hh"
#include "stamp.hh"

#include <glibmm/miscutils.h>
#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>

namespace {
	std::filesystem::path config_file(const std::string& name)
		{
		return std::filesystem::path(Glib::get_user_config_dir()) / "gaindrive-player" / name;
		}
}

nlohmann::json read_config(const std::string& name)
	{
	std::ifstream in(config_file(name));
	if(!in) return nullptr;
	auto j = nlohmann::json::parse(in, nullptr, false);
	if(j.is_discarded()) {
		std::cout << stamp() << name << " is not valid JSON, ignoring it" << std::endl;
		return nullptr;
		}
	return j;
	}

void write_config(const std::string& name, const nlohmann::json& j)
	{
	auto file = config_file(name);
	std::error_code ec;
	std::filesystem::create_directories(file.parent_path(), ec);
	// Written aside and renamed, so a crash mid-write cannot leave a
	// truncated file that loses its contents.
	auto tmp = file;
	tmp += ".tmp";
	{
	std::ofstream out(tmp);
	out << j.dump(1, '\t') << "\n";
	if(!out) {
		std::cout << stamp() << "cannot write " << tmp << std::endl;
		return;
		}
	}
	std::filesystem::rename(tmp, file, ec);
	if(ec)
		std::cout << stamp() << "cannot replace " << file << ": " << ec.message() << std::endl;
	}

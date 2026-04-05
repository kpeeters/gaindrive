#include <iostream>
#include <fstream>
#include <string>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "gaindrive.hh"

int main(int argc, char* argv[])
	{
	cxxopts::Options options("gaindrive", "Subsonic-compatible music server");
	options.add_options()
		("port",   "Port to listen on",      cxxopts::value<int>()->default_value("4040"))
		("config", "Path to config file",    cxxopts::value<std::string>()->default_value("/etc/gaindrive.conf"))
		("h,help", "Show help")
		;

	auto args = options.parse(argc, argv);
	if(args.count("help")) {
		std::cout << options.help() << "\n";
		return 0;
		}

	std::string host = "127.0.0.1";
	int         port = args["port"].as<int>();

	// Read config file; missing file is not fatal, just use defaults.
	std::string config_path = args["config"].as<std::string>();
	std::ifstream cfg_file(config_path);
	if(cfg_file) {
		try {
			nlohmann::json cfg = nlohmann::json::parse(cfg_file);
			if(cfg.contains("host")) host = cfg["host"];
			if(cfg.contains("port") && !args.count("port")) port = cfg["port"];
			}
		catch(const std::exception& e) {
			std::cerr << "Warning: failed to parse " << config_path << ": " << e.what() << "\n";
			}
		}

	GainDrive gd;
	gd.listen(host, port);

	return 0;
	}

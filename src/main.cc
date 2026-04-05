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
		("port",       "Port to listen on",      cxxopts::value<int>()->default_value("4040"))
		("config",     "Path to config file",    cxxopts::value<std::string>()->default_value("/etc/gaindrive.conf"))
		("db",         "Path to database file",  cxxopts::value<std::string>()->default_value("/var/lib/gaindrive/gaindrive.db"))
		("music-root", "Root music directory",   cxxopts::value<std::string>()->default_value("/music"))
		("no-scan",    "Skip startup filesystem scan")
		("h,help",     "Show help")
		;

	auto args = options.parse(argc, argv);
	if (args.count("help")) {
		std::cout << options.help() << "\n";
		return 0;
		}

	std::string host       = "127.0.0.1";
	int         port       = args["port"].as<int>();
	std::string db_path    = args["db"].as<std::string>();
	std::string music_root = args["music-root"].as<std::string>();
	bool        no_scan    = args.count("no-scan") > 0;

	// Read config file; missing file is not fatal, just use defaults.
	std::string config_path = args["config"].as<std::string>();
	std::ifstream cfg_file(config_path);
	if (cfg_file) {
		try {
			nlohmann::json cfg = nlohmann::json::parse(cfg_file);
			if (cfg.contains("host"))       host       = cfg["host"];
			if (cfg.contains("db_path"))    db_path    = cfg["db_path"];
			if (cfg.contains("music_root")) music_root = cfg["music_root"];
			// CLI flags take precedence over config for port.
			if (cfg.contains("port") && !args.count("port")) port = cfg["port"];
			}
		catch (const std::exception& e) {
			std::cerr << "Warning: failed to parse " << config_path << ": " << e.what() << "\n";
			}
		}

	GainDrive gd(db_path, music_root, no_scan);
	gd.listen(host, port);

	return 0;
	}

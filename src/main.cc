#include <iostream>
#include <fstream>
#include <string>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "gaindrive.hh"
#include "mediastore.hh"

int main(int argc, char* argv[])
	{
	cxxopts::Options options("gaindrive", "Subsonic-compatible music server");
	options.add_options()
		("host",       "Host interface to listen on", cxxopts::value<std::string>()->default_value("127.0.0.1") )
		("port",       "Port to listen on",           cxxopts::value<int>()->default_value("4040"))
		("config",     "Path to config file",         cxxopts::value<std::string>()->default_value("/etc/gaindrive.conf"))
		("db",         "Path to database file",       cxxopts::value<std::string>()->default_value("/var/lib/gaindrive/gaindrive.db"))
		("music-root", "Root music directory",        cxxopts::value<std::string>()->default_value("/music"))
		("upload-dir", "Directory for uploaded archives", cxxopts::value<std::string>()->default_value("/tmp/gaindrive-uploads"))
		("no-scan",    "Skip startup filesystem scan")
		("debug",      "Print all API responses to stdout")
		("add-user",   "Create a user and exit",      cxxopts::value<std::string>())
		("password",   "Password for --add-user",     cxxopts::value<std::string>())
		("h,help",     "Show help")
		;

	auto args = options.parse(argc, argv);
	if (args.count("help")) {
		std::cout << options.help() << "\n";
		return 0;
		}

	std::string host       = args["host"].as<std::string>();
	int         port       = args["port"].as<int>();
	std::string db_path    = args["db"].as<std::string>();
	std::string music_root  = args["music-root"].as<std::string>();
	std::string upload_dir  = args["upload-dir"].as<std::string>();
	bool        no_scan          = args.count("no-scan") > 0;
	bool        debug            = args.count("debug")   > 0;
	bool        flat_multi_disc  = true;

	// Read config file; missing file is not fatal, just use defaults.
	std::string config_path = args["config"].as<std::string>();
	std::ifstream cfg_file(config_path);
	if (cfg_file) {
		try {
			nlohmann::json cfg = nlohmann::json::parse(cfg_file);
			if (cfg.contains("host"))       host       = cfg["host"];
			if (cfg.contains("db_path"))    db_path    = cfg["db_path"];
			if (cfg.contains("music_root"))  music_root  = cfg["music_root"];
			if (cfg.contains("upload_dir"))  upload_dir  = cfg["upload_dir"];
			// CLI flags take precedence over config for port.
			if (cfg.contains("port") && !args.count("port")) port = cfg["port"];
			if (cfg.contains("flat_multi_disc")) flat_multi_disc = cfg["flat_multi_disc"].get<bool>();
			}
		catch (const std::exception& e) {
			std::cerr << "Warning: failed to parse " << config_path << ": " << e.what() << "\n";
			}
		}

	// --add-user: create a user in the DB and exit without starting the server.
	if (args.count("add-user")) {
		if (!args.count("password")) {
			std::cerr << "Error: --password is required with --add-user.\n";
			return 1;
			}
		std::string username = args["add-user"].as<std::string>();
		std::string password = args["password"].as<std::string>();
		MediaStore store(db_path, music_root);
		bool ok = store.add_user(username, password, true /* is_admin */);
		std::cout << (ok ? "User '" + username + "' created."
		               : "User '" + username + "' already exists.") << "\n";
		return ok ? 0 : 1;
		}

	GainDrive gd(db_path, music_root, upload_dir, no_scan, debug, flat_multi_disc);
	gd.listen(host, port);

	return 0;
	}

#include <iostream>
#include <fstream>
#include <string>

#include <httplib.h>
#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

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

	httplib::Server server;

	// Catch-all handler for /rest/* — individual endpoints come later.
	server.Get("/rest/:endpoint", [](const httplib::Request&, httplib::Response& res) {
		res.set_content(
			R"(<?xml version="1.0" encoding="UTF-8"?>)"
			R"(<subsonic-response xmlns="http://subsonic.org/restapi" status="failed" version="1.16.1">)"
			R"(<error code="0" message="not implemented"/>)"
			R"(</subsonic-response>)",
			"application/xml");
		});

	std::cout << "Listening on " << host << ":" << port << "\n";
	server.listen(host, port);

	return 0;
	}

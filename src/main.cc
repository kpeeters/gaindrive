#include <cctype>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "gaindrive.hh"
#include "mediastore.hh"

// Parses one "name=path" root argument. Splits on the FIRST '=' so a path
// containing '=' still works; a name never can, since it is restricted below.
static bool parse_root(const std::string& arg, const std::string& type,
                       std::vector<MediaStore::Root>& out, std::string& err)
	{
	auto eq = arg.find('=');
	if (eq == std::string::npos || eq == 0 || eq + 1 >= arg.size()) {
		err = "expected name=path, got '" + arg + "'";
		return false;
		}
	out.push_back({ arg.substr(0, eq), type, arg.substr(eq + 1) });
	return true;
	}

// Rejects anything that would make the library unreadable later rather than
// starting up degraded. A root's name is embedded in every path stored for it,
// so a bad or duplicated name is not a cosmetic problem: it silently detaches
// content from the rows that reference it.
static bool validate_roots(const std::vector<MediaStore::Root>& roots,
                           bool need_library, std::string& err)
	{
	int uploads = 0, library = 0;
	std::set<std::string> names;
	for (const auto& r : roots) {
		if (r.name.empty()) { err = "a root has an empty name"; return false; }
		for (char c : r.name)
			if (!std::isalnum(static_cast<unsigned char>(c))
			        && c != '_' && c != '-') {
				err = "root name '" + r.name
				    + "' may contain only letters, digits, '_' and '-'";
				return false;
				}
		if (!names.insert(r.name).second) {
			err = "duplicate root name '" + r.name
			    + "'; names must be unique across all roots";
			return false;
			}
		std::error_code ec;
		if (!std::filesystem::is_directory(r.path, ec)) {
			err = "root '" + r.name + "' path is not a directory: " + r.path;
			return false;
			}
		if (r.type == "uploads") ++uploads; else ++library;
		}
	if (uploads > 1) {
		err = "at most one uploads root may be configured";
		return false;
		}
	if (need_library && library == 0) {
		err = "no library root configured; pass at least one --artist-root "
		      "or --category-root";
		return false;
		}
	return true;
	}

int main(int argc, char* argv[])
	{
	cxxopts::Options options("gaindrive", "Subsonic-compatible music server");
	options.add_options()
		("host",       "Host interface to listen on", cxxopts::value<std::string>()->default_value("127.0.0.1") )
		("port",       "Port to listen on",           cxxopts::value<int>()->default_value("4040"))
		("config",     "Path to config file",         cxxopts::value<std::string>()->default_value("/etc/gaindrive.conf"))
		("db",         "Path to database file",       cxxopts::value<std::string>()->default_value("/var/lib/gaindrive/gaindrive.db"))
		("user-db",    "Path to the user/state database (default: derived from --db)", cxxopts::value<std::string>())
		("artist-root",   "Library root whose subdirectories are artists, as name=path (repeatable)", cxxopts::value<std::vector<std::string>>())
		("category-root", "Library root whose subdirectories are categories, as name=path (repeatable)", cxxopts::value<std::vector<std::string>>())
		("upload-root",   "Root holding per-user personal uploads, as name=path", cxxopts::value<std::string>())
		("upload-dir", "Directory for uploaded archives", cxxopts::value<std::string>()->default_value("/tmp/gaindrive-uploads"))
		("transcode-cache",    "Directory for cached transcodes (default: alongside --db)", cxxopts::value<std::string>())
		("transcode-cache-mb", "Transcode cache size in MB (0 disables)", cxxopts::value<int>()->default_value("1024"))
		("transcode-jobs",     "Max concurrent ffmpeg transcodes (0 = half the cores)", cxxopts::value<int>()->default_value("0"))
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
	std::string user_db_path = args.count("user-db") ? args["user-db"].as<std::string>() : "";
	std::string upload_dir  = args["upload-dir"].as<std::string>();

	std::vector<MediaStore::Root> roots;
	{
	std::string err;
	auto collect = [&](const char* opt, const char* type) {
		if (!args.count(opt)) return true;
		for (const auto& v : args[opt].as<std::vector<std::string>>())
			if (!parse_root(v, type, roots, err)) {
				std::cerr << "Error: --" << opt << ": " << err << "\n";
				return false;
				}
		return true;
		};
	if (!collect("artist-root", "artists"))      return 1;
	if (!collect("category-root", "categories")) return 1;
	if (args.count("upload-root")
	        && !parse_root(args["upload-root"].as<std::string>(), "uploads",
	                       roots, err)) {
		std::cerr << "Error: --upload-root: " << err << "\n";
		return 1;
		}
	}
	bool        no_scan          = args.count("no-scan") > 0;
	bool        debug            = args.count("debug")   > 0;
	bool        flat_multi_disc  = true;
	std::string transcode_cache_dir = args.count("transcode-cache")
	    ? args["transcode-cache"].as<std::string>() : "";
	int         transcode_cache_mb  = args["transcode-cache-mb"].as<int>();
	int         transcode_jobs      = args["transcode-jobs"].as<int>();

	// Read config file; missing file is not fatal, just use defaults.
	std::string config_path = args["config"].as<std::string>();
	std::ifstream cfg_file(config_path);
	if (cfg_file) {
		try {
			nlohmann::json cfg = nlohmann::json::parse(cfg_file);
			if (cfg.contains("host"))       host       = cfg["host"];
			if (cfg.contains("db_path"))    db_path    = cfg["db_path"];
			// CLI --user-db takes precedence over config user_db_path.
			if (cfg.contains("user_db_path") && !args.count("user-db")) user_db_path = cfg["user_db_path"];
			// Roots from the config only when none were given on the command
			// line, matching the precedence used for port and user-db below.
			// The systemd unit runs with no arguments, so this is the normal
			// path for a service install.
			if (cfg.contains("roots") && roots.empty()) {
				for (const auto& r : cfg["roots"])
					roots.push_back({ r.value("name", std::string()),
					                  r.value("type", std::string("artists")),
					                  r.value("path", std::string()) });
				}
			if (cfg.contains("upload_dir"))  upload_dir  = cfg["upload_dir"];
			// CLI flags take precedence over config for port.
			if (cfg.contains("port") && !args.count("port")) port = cfg["port"];
			if (cfg.contains("flat_multi_disc")) flat_multi_disc = cfg["flat_multi_disc"].get<bool>();
			// CLI wins over config, same rule as --user-db and --port above.
			if (cfg.contains("transcode_cache_dir") && !args.count("transcode-cache"))
				transcode_cache_dir = cfg["transcode_cache_dir"];
			if (cfg.contains("transcode_cache_mb") && !args.count("transcode-cache-mb"))
				transcode_cache_mb = cfg["transcode_cache_mb"].get<int>();
			if (cfg.contains("transcode_jobs") && !args.count("transcode-jobs"))
				transcode_jobs = cfg["transcode_jobs"].get<int>();
			}
		catch (const std::exception& e) {
			std::cerr << "Warning: failed to parse " << config_path << ": " << e.what() << "\n";
			}
		}

	// --add-user touches no library, so it is the one mode that may run with
	// no roots configured.
	{
	std::string err;
	if (!validate_roots(roots, !args.count("add-user"), err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
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
		MediaStore store(db_path, roots, user_db_path);
		bool ok = store.add_user(username, password, true /* is_admin */);
		std::cout << (ok ? "User '" + username + "' created."
		               : "User '" + username + "' already exists.") << "\n";
		return ok ? 0 : 1;
		}

	GainDrive gd(db_path, roots, upload_dir, no_scan, debug, flat_multi_disc,
	             user_db_path, transcode_cache_dir, transcode_cache_mb,
	             transcode_jobs);
	// Non-zero on a failed bind, so a supervisor restarts rather than
	// recording a clean shutdown for a server that never served anything.
	return gd.listen(host, port) ? 0 : 1;
	}

#include <cctype>
#include <csignal>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <set>
#include <string>
#include <vector>

#include <termios.h>
#include <unistd.h>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "gaindrive.hh"
#include "mediastore.hh"

// Installation-dependent defaults, normally supplied by CMake from
// GAINDRIVE_SYSCONFDIR/GAINDRIVE_LOCALSTATEDIR. A package that installs under a
// prefix (Homebrew) points these into that prefix; a distribution package
// leaves them at the FHS locations these fallbacks name.
#ifndef GAINDRIVE_DEFAULT_CONFIG
#define GAINDRIVE_DEFAULT_CONFIG "/etc/gaindrive.conf"
#endif
#ifndef GAINDRIVE_DEFAULT_DB
#define GAINDRIVE_DEFAULT_DB "/var/lib/gaindrive/gaindrive.db"
#endif

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

// Ctrl-C at a password prompt would otherwise kill the process with ECHO still
// off, leaving the user in a shell that does not show what they type. Restore
// the terminal, then die the way we would have.
static termios g_saved_termios;

static void restore_termios_and_die(int sig)
	{
	tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
	signal(sig, SIG_DFL);
	raise(sig);
	}

// Reads one line with terminal echo turned off, so a password never lands on
// screen or in a scrollback buffer. Falls back to an ordinary read when stdin
// is not a terminal — callers only get here after checking isatty(), so that
// is a redirected-stdin corner case rather than the normal path.
static bool read_hidden(const char* prompt, std::string& out)
	{
	std::cout << prompt << std::flush;

	bool tty = tcgetattr(STDIN_FILENO, &g_saved_termios) == 0;
	if (tty) {
		signal(SIGINT, restore_termios_and_die);
		termios quiet = g_saved_termios;
		quiet.c_lflag &= ~ECHO;
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &quiet);
		}

	bool ok = static_cast<bool>(std::getline(std::cin, out));

	if (tty) {
		tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved_termios);
		signal(SIGINT, SIG_DFL);
		// The Enter that ended the line was not echoed either.
		std::cout << "\n";
		}
	return ok;
	}

static std::string trim(const std::string& s)
	{
	auto b = s.find_first_not_of(" \t");
	if (b == std::string::npos) return "";
	return s.substr(b, s.find_last_not_of(" \t") - b + 1);
	}

// Nobody can log in to a server with no accounts, so an empty user table is a
// hard stop rather than a warning. On a fresh install there is a terminal to
// ask on; a systemd start has none, and gets told what to run instead.
static bool create_first_user(MediaStore& store)
	{
	if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
		std::cerr << "Error: no user accounts exist, and there is no terminal "
		             "to ask on.\nCreate the first one with:\n"
		             "  gaindrive --add-user <name> --password <password>\n";
		return false;
		}

	std::cout << "No user accounts exist yet. Creating the first one; it will "
	             "be an administrator.\n";

	for (;;) {
		std::string username;
		std::cout << "Username: " << std::flush;
		// Failure here is EOF (^D), not a retryable mistake.
		if (!std::getline(std::cin, username)) return false;
		username = trim(username);
		if (username.empty()) {
			std::cout << "Username cannot be empty.\n";
			continue;
			}

		std::string password, again;
		if (!read_hidden("Password: ", password))        return false;
		if (password.empty()) {
			std::cout << "Password cannot be empty.\n";
			continue;
			}
		if (!read_hidden("Repeat password: ", again))    return false;
		if (password != again) {
			std::cout << "Passwords do not match.\n";
			continue;
			}

		if (!store.add_user(username, password, true /* is_admin */)) {
			std::cout << "User '" << username << "' already exists.\n";
			continue;
			}
		std::cout << "User '" << username << "' created.\n";
		return true;
		}
	}

int main(int argc, char* argv[])
	{
	// A peer that goes away mid-write must surface as EPIPE, not as a signal
	// that kills the server. Linux is covered incidentally because OpenSSL's
	// socket BIO and httplib both pass MSG_NOSIGNAL there; Darwin has no such
	// flag and issues a plain write(), so a Chromecast dropping its TLS
	// connection during SSL_write would take the whole process down.
	// ffmpeg inherits this — SIG_IGN survives exec — which is also what we
	// want: it exits non-zero on a closed pipe instead of dying by signal, and
	// every ffmpeg call site already handles a non-zero exit.
	signal(SIGPIPE, SIG_IGN);

	cxxopts::Options options("gaindrive", "Subsonic-compatible music server");
	options.add_options()
		("host",       "Host interface to listen on", cxxopts::value<std::string>()->default_value("127.0.0.1") )
		("port",       "Port to listen on",           cxxopts::value<int>()->default_value("4040"))
		("config",     "Path to config file",         cxxopts::value<std::string>()->default_value(GAINDRIVE_DEFAULT_CONFIG))
		("db",         "Path to database file",       cxxopts::value<std::string>()->default_value(GAINDRIVE_DEFAULT_DB))
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
		("version",    "Show version and exit")
		;

	auto args = options.parse(argc, argv);
	if (args.count("help")) {
		std::cout << options.help() << "\n";
		return 0;
		}
	if (args.count("version")) {
		std::cout << "gaindrive " GAINDRIVE_VERSION "\n";
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
	// Opening the database writes — the journal_mode pragma, the ATTACH, the
	// schema DDL, the root reconciliation — so a database locked by another
	// process, or a corrupt one, throws out here. Report it instead of letting
	// it reach the default terminate handler and abort.
	try {
		if (args.count("add-user")) {
			std::string username = args["add-user"].as<std::string>();
			std::string password;
			if (args.count("password"))
				password = args["password"].as<std::string>();
			else if (isatty(STDIN_FILENO)) {
				// Better than the command line anyway: --password puts the
				// password in the shell history and in ps output.
				if (!read_hidden("Password: ", password)) return 1;
				}
			else {
				std::cerr << "Error: --password is required with --add-user "
				             "when there is no terminal to ask on.\n";
				return 1;
				}
			if (password.empty()) {
				std::cerr << "Error: password cannot be empty.\n";
				return 1;
				}
			MediaStore store(db_path, roots, user_db_path);
			bool ok = store.add_user(username, password, true /* is_admin */);
			std::cout << (ok ? "User '" + username + "' created."
			               : "User '" + username + "' already exists.") << "\n";
			return ok ? 0 : 1;
			}

		// Before constructing GainDrive, which starts the cast manager, the
		// watcher and the transcode cache: a server nobody can log in to
		// should do none of that. The store is opened in its own scope so it
		// is closed again before GainDrive opens the same database.
		{
		MediaStore store(db_path, roots, user_db_path);
		if (store.list_users().empty() && !create_first_user(store))
			return 1;
		}

		GainDrive gd(db_path, roots, upload_dir, no_scan, debug, flat_multi_disc,
		             user_db_path, transcode_cache_dir, transcode_cache_mb,
		             transcode_jobs);
		// Non-zero on a failed bind, so a supervisor restarts rather than
		// recording a clean shutdown for a server that never served anything.
		return gd.listen(host, port) ? 0 : 1;
		}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
		}
	}

#pragma once

#include <string>
#include <vector>

// Installing gaindrive as a systemd service, from the options it was just
// proved to work with.
//
// This knows systemd, accounts and file permissions, and nothing else: no
// MediaStore, no HTTP, no cxxopts, and no notion of what a library root is
// beyond a path the service user has to be able to read.  Same shape as Tmdb,
// VideoArt and UrlFetcher -- the whole file is what a launchd or an OpenRC
// equivalent would sit beside rather than be woven into.  A namespace of free
// functions rather than a class, like imagescale, because there is no state to
// keep between calls.
//
// It deliberately does not build the config file it writes.  Those key names
// are already a fact known to the reader in main.cc, and a second copy of them
// would not fail loudly: a renamed key writes a file that parses, is ignored,
// and starts the server on defaults, with a clean log saying everything is
// fine.  So the config arrives here serialised, and the writer lives beside the
// reader.
//
// It is also the only code in src/ that touches geteuid(), getpwnam() or
// chown().  gaindrive otherwise runs as whoever launched it and leaves dropping
// privileges to systemd's User=; the one command that has to run as root to
// write into /etc keeps that knowledge here rather than spreading it.
namespace service
	{

	// Where a generated unit goes: /etc/systemd/system, never systemd's vendor
	// directory.  That one belongs to whatever installed the package, and a unit
	// of the same name in /etc overrides it rather than destroying it -- so
	// uninstalling reveals a packaged unit again instead of leaving nothing.
	// GAINDRIVE_UNIT_DIR overrides this, which is what makes a real write
	// testable inside a container without scribbling on the host.  It
	// deliberately does not relax the root check.
	std::string unit_dir();

	struct InstallRequest
		{
		// The config file, already serialised, and where it goes.
		std::string config_path;
		std::string config_text;

		// Appended to ExecStart.  Empty when the config sits at the path
		// compiled into this binary, so the common unit takes no arguments.
		std::vector<std::string> exec_args;

		// What the preconditions check.  All absolute, and all checked against
		// the *target* user rather than against root, which can read anything.
		std::string              client_db_path;   // must already exist
		std::string              db_dir;           // must be writable: -wal, -shm
		std::vector<std::string> root_paths;       // must be readable
		std::vector<std::string> mount_paths;      // for RequiresMountsFor=

		// Reported in warnings the installer alone is placed to give.
		std::string host;
		int         port = 0;
		std::string upload_dir;

		std::string user_name;         // --service-user; empty: from SUDO_USER
		bool        force   = false;   // replace a file we did not write
		bool        dry_run = false;   // print both documents, write nothing
		};

	struct UninstallRequest
		{
		// Named in the output and never touched.  Removing a service must not
		// remove a library.
		std::string config_path;
		std::string db_path;
		bool        force   = false;
		bool        dry_run = false;
		};

	// Both print their own diagnostics in main.cc's "Error: " / "Warning: "
	// voice and return a process exit code.  Nothing is written until every
	// precondition has passed, so a refusal leaves the machine as it was.
	int install(const InstallRequest& req);
	int uninstall(const UninstallRequest& req);

	}

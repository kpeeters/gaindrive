#include "service.hh"
#include "embedded_unit.hh"
#include "proc.hh"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <sstream>

#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <reproc++/reproc.hpp>

namespace fs = std::filesystem;

namespace {

constexpr const char* UNIT_NAME = "gaindrive.service";

// What identifies a unit or config this command wrote.  It is the flag's own
// name, which the generated header line carries, so the test is "does the
// provenance line say us" rather than a magic cookie nobody would recognise.
constexpr const char* MARKER     = "--install-service";
constexpr size_t      MARKER_LEN = 512;

// The account a service runs as, resolved once and passed about.
struct Target
	{
	std::string user, group;
	uid_t       uid = 0;
	gid_t       gid = 0;
	std::string home;
	};

std::string today()
	{
	std::time_t t  = std::time(nullptr);
	std::tm     tm = *std::localtime(&t);
	std::ostringstream ss;
	ss << std::put_time(&tm, "%F");
	return ss.str();
	}

// A path on its way into a systemd unit, through two separate hazards.
//
// The obvious one is whitespace: ExecStart and RequiresMountsFor are split on
// it, so a path holding any has to be quoted.  The other is that '%' begins a
// systemd *specifier* -- %H is the hostname, %i the instance name -- and
// ExecStart, RequiresMountsFor and ConditionPathExists all expand them.  So a
// music directory called "80% Live" silently becomes something else, and the
// escape is to double the character.  Being wrong here is not a parse error
// anywhere; it is a unit naming a directory that does not exist.
//
// A path holding a newline or a control character is refused rather than
// escaped: the whole value of a generated unit is that it can be read by eye,
// and one that cannot is worse than no unit at all.
bool sd_quote(const std::string& s, std::string& out, std::string& err)
	{
	for (unsigned char c : s)
		if (c < 0x20 || c == 0x7f) {
			err = "path contains a control character and cannot go in a "
			      "systemd unit: " + s;
			return false;
			}

	std::string esc;
	for (char c : s) {
		if (c == '%') esc += '%';   // %% is a literal per cent
		esc += c;
		}

	if (esc.find_first_of(" \t\"\\'") == std::string::npos) {
		out = esc;
		return true;
		}
	out = "\"";
	for (char c : esc) {
		if (c == '"' || c == '\\') out += '\\';
		out += c;
		}
	out += '"';
	return true;
	}

// The path that goes into ExecStart.  Canonicalised deliberately, unlike the
// roots in the config: this one has to keep naming the same binary after a
// reboot, so a symlink on somebody's PATH is exactly what must not be recorded.
bool self_exe(std::string& out, std::string& err)
	{
	std::error_code ec;
	fs::path p = fs::read_symlink("/proc/self/exe", ec);
	if (ec) { err = ec.message(); return false; }
	fs::path c = fs::weakly_canonical(p, ec);
	out = (ec ? p : c).string();
	return true;
	}

// SUDO_USER, or an explicit --service-user.  Refusing root is the point of the
// feature rather than caution: a unit running as root is what installing by
// hand gets you when you stop reading, and it is why gokapi refuses too.
bool resolve_user(const std::string& want, Target& t, std::string& err)
	{
	std::string name = want;
	if (name.empty()) {
		const char* s = std::getenv("SUDO_USER");
		if (s) name = s;
		}
	if (name.empty()) {
		err = "cannot tell which account the service should run as: SUDO_USER "
		      "is not set, which is what 'sudo -i' and 'su' do.\n"
		      "       Name one with --service-user <name>.";
		return false;
		}
	if (name == "root") {
		err = "refusing to install a service that runs as root; pass "
		      "--service-user <name> naming the account that owns the library.";
		return false;
		}
	errno = 0;
	struct passwd* pw = ::getpwnam(name.c_str());
	if (!pw) {
		err = errno ? "cannot look up user '" + name + "': "
		              + std::strerror(errno)
		            : "no such user '" + name + "'";
		return false;
		}
	if (pw->pw_uid == 0) {
		err = "refusing to install a service that runs as '" + name
		    + "', which is uid 0.";
		return false;
		}
	t.user = name;
	t.uid  = pw->pw_uid;
	t.gid  = pw->pw_gid;
	t.home = pw->pw_dir ? pw->pw_dir : "";
	struct group* gr = ::getgrgid(t.gid);
	t.group = gr && gr->gr_name ? gr->gr_name : std::to_string(t.gid);
	return true;
	}

// Can the target user reach this path?
//
// Under sudo every access() succeeds, because root passes every permission
// check -- so asking directly answers a question nobody asked.  The only honest
// way is to become the user, which means a fork: initgroups() for the
// supplementary groups (the shared-group recipe in README.md is exactly that),
// then setgid before setuid, because the reverse loses the privilege needed to
// change the group.  Without this the first sign of trouble is a service that
// starts and serves an empty library.
//
// X_OK is added for a DIRECTORY and never for a file.  On a directory it is
// search permission, which is what reaching anything inside it needs; on a file
// it asks whether the file may be executed, which for a database is false for
// everybody -- so asking it of one reports "you cannot read your own database"
// about a file the owner is reading happily.  That was a real bug here.
//
// The reason comes back rather than a bare bool, because the three ways this
// can fail want three different answers from the reader and rendering all of
// them as "not readable" sent somebody looking at file modes that were fine.
bool reachable_as(const std::string& path, const Target& t, bool want_write,
                  std::string& why)
	{
	pid_t pid = ::fork();
	if (pid < 0) return true;              // cannot tell; do not block the install
	if (pid == 0) {
		// 120-122 are distinguishable from any errno, which is capped below.
		if (::initgroups(t.user.c_str(), t.gid) != 0) ::_exit(120);
		if (::setgid(t.gid) != 0)                     ::_exit(121);
		if (::setuid(t.uid) != 0)                     ::_exit(122);
		// Directory-or-not is decided here, after the drop, rather than in the
		// parent: on an NFS mount exported with root_squash, root is the one
		// identity that may not stat the path at all, so asking as root can
		// give the wrong answer about a library the owner reads every day.
		// A stat that fails leaves is_dir false, and access() below then
		// reports the real reason rather than this guess.
		std::error_code ec;
		bool is_dir = fs::is_directory(path, ec);
		int  mode   = R_OK | (want_write ? W_OK : 0) | (is_dir ? X_OK : 0);
		if (::access(path.c_str(), mode) == 0)        ::_exit(0);
		::_exit(errno > 119 ? 119 : errno);
		}
	int status = 0;
	while (::waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
	if (!WIFEXITED(status)) {
		why = "the permission check did not complete";
		return false;
		}

	int code = WEXITSTATUS(status);
	if (code == 0) return true;
	switch (code) {
		case 120: why = "cannot look up the groups of '" + t.user + "'"; break;
		case 121: why = "cannot switch to group " + t.group;             break;
		case 122: why = "cannot switch to user " + t.user;               break;
		default:  why = std::strerror(code);                             break;
		}
	return false;
	}

// Is this file one we wrote?  Cheap and deliberately shallow: it says the file
// came from --install-service, not that nobody has edited it since.  That is
// why the two callers treat the answer differently -- see install().
bool wrote_by_us(const std::string& path)
	{
	std::ifstream f(path, std::ios::binary);
	if (!f) return false;
	std::string head(MARKER_LEN, '\0');
	head.resize(static_cast<size_t>(f.read(head.data(), MARKER_LEN).gcount()));
	return head.find(MARKER) != std::string::npos;
	}

// Fill the template.  Everything above the HEADER placeholder is the template's
// own documentation and is dropped; an unknown placeholder is a hard error,
// because configure_file's habit of substituting nothing quietly is precisely
// how a unit ships with a directive missing and nothing to say so.
bool fill_template(const std::vector<std::pair<std::string, std::string>>& vals,
                   std::string& out, std::string& err)
	{
	std::string tmpl(embedded::service_unit);
	size_t head = tmpl.find("\n%HEADER%\n");
	if (head == std::string::npos) {
		err = "the embedded unit template has no HEADER placeholder line; "
		      "this is a bug in dist/gaindrive.service.in.";
		return false;
		}
	tmpl.erase(0, head + 1);

	out.clear();
	for (size_t i = 0; i < tmpl.size(); ) {
		if (tmpl[i] != '%') { out += tmpl[i++]; continue; }
		// %% is systemd's own escape for a literal per cent, so it is passed
		// through untouched rather than read as an empty placeholder name.
		// Without this the template cannot contain one at all, and the error it
		// would give -- "this is a bug in service.cc" -- points at the wrong
		// file entirely.
		if (i + 1 < tmpl.size() && tmpl[i + 1] == '%') {
			out += "%%";
			i += 2;
			continue;
			}
		size_t end = tmpl.find('%', i + 1);
		if (end == std::string::npos) { out += tmpl[i++]; continue; }
		std::string name = tmpl.substr(i + 1, end - i - 1);
		auto it = std::find_if(vals.begin(), vals.end(),
		    [&name](const auto& v) { return v.first == name; });
		if (it == vals.end()) {
			err = "the unit template names %" + name + "%, which "
			      "--install-service cannot fill; this is a bug in service.cc.";
			return false;
			}
		out += it->second;
		i = end + 1;
		}
	return true;
	}

// Write a file as root, atomically, without ever exposing it at the wrong mode
// or to the wrong owner.
//
// mkstemp in the destination's own directory, then fchmod and fchown on the
// descriptor, then rename.  Three reasons, and each has bitten somebody:
//
//   * the rename is the only publish, as in TranscodeCache, so a failure never
//     leaves a half-written config that the next start parses as far as the
//     truncation and then ignores;
//   * mkstemp creates at 0600, so the file is never briefly world-readable in
//     the window a create-then-chmod would leave open;
//   * the mode and the owner are set on the descriptor rather than on a path,
//     because --config may name a directory the target user can write, and a
//     path-based chown there is a symlink race that hands them root's chown.
bool write_root_file(const std::string& path, const std::string& text,
                     mode_t mode, uid_t uid, gid_t gid, std::string& err)
	{
	fs::path dest(path);
	std::string tmpl = (dest.parent_path() / (dest.filename().string()
	                                          + ".XXXXXX")).string();
	std::vector<char> buf(tmpl.begin(), tmpl.end());
	buf.push_back('\0');

	int fd = ::mkstemp(buf.data());
	if (fd < 0) { err = std::string("cannot create a temporary file beside ")
	                  + path + ": " + std::strerror(errno); return false; }
	std::string tmp(buf.data());

	auto fail = [&](const char* what) {
		err = std::string(what) + " " + tmp + ": " + std::strerror(errno);
		::close(fd);
		std::error_code ec;
		fs::remove(tmp, ec);
		return false;
		};

	size_t off = 0;
	while (off < text.size()) {
		ssize_t n = ::write(fd, text.data() + off, text.size() - off);
		if (n < 0) { if (errno == EINTR) continue; return fail("cannot write"); }
		off += static_cast<size_t>(n);
		}
	if (::fchmod(fd, mode) != 0)          return fail("cannot set the mode on");
	if (::fchown(fd, uid, gid) != 0)      return fail("cannot set the owner of");
	if (::fsync(fd) != 0)                 return fail("cannot flush");
	if (::close(fd) != 0)                 { fd = -1; return fail("cannot close"); }

	std::error_code ec;
	fs::rename(tmp, dest, ec);
	if (ec) {
		err = "cannot move " + tmp + " into place as " + path + ": "
		    + ec.message();
		fs::remove(tmp, ec);
		return false;
		}
	return true;
	}

// systemctl, with both streams going straight to the terminal: its own output
// is the report here ("Created symlink ..."), and its errors are better than
// anything this could paraphrase.  Nothing is captured, so nothing needs
// draining and the pipe deadlock stderr_tail() exists for cannot arise.
bool run_systemctl(const std::vector<std::string>& args, bool quiet_failure)
	{
	std::vector<std::string> argv{ "systemctl" };
	argv.insert(argv.end(), args.begin(), args.end());

	reproc::options opts;
	opts.redirect.out.type = reproc::redirect::type::parent;
	opts.redirect.err.type = quiet_failure ? reproc::redirect::type::discard
	                                       : reproc::redirect::type::parent;
	opts.deadline = reproc::milliseconds(30 * 1000);

	reproc::process proc;
	if (auto ec = proc.start(argv, opts)) {
		if (!quiet_failure)
			std::cerr << "Error: cannot run systemctl: " << ec.message() << "\n";
		return false;
		}
	auto [status, ec] = proc.wait(reproc::infinite);
	if (ec) {
		if (!quiet_failure)
			std::cerr << "Error: systemctl did not finish: " << ec.message()
			          << "\n";
		return false;
		}
	return status == 0;
	}

}  // namespace

namespace service {

std::string unit_dir()
	{
	if (const char* d = std::getenv("GAINDRIVE_UNIT_DIR"); d && *d) return d;
	return "/etc/systemd/system";
	}

int install(const InstallRequest& req)
	{
	const std::string unit_path = (fs::path(unit_dir()) / UNIT_NAME).string();
	std::string err;

	// --- preconditions, cheapest and most-likely-wrong first ---------------
	//
	// "You skipped a step" is checked before "your permissions are wrong", so
	// somebody following the documented flow out of order is told which step
	// they missed rather than which directory mode surprised us.

	std::error_code ec;
	if (!fs::is_directory("/run/systemd/system", ec)) {
		std::cerr << "Error: --install-service needs systemd; "
		             "/run/systemd/system does not exist.\n"
		             "       On macOS, install through Homebrew instead -- its "
		             "formula registers a launchd job\n"
		             "       (brew services start gaindrive).\n";
		return 1;
		}
	if (!on_path("systemctl")) {
		std::cerr << "Error: systemctl is not on PATH; --install-service "
		             "cannot enable the unit.\n";
		return 1;
		}
	if (!req.dry_run && ::geteuid() != 0) {
		std::cerr << "Error: --install-service writes " << req.config_path
		          << " and a systemd unit, so it must\n"
		             "       be run as root; re-run it with sudo. Add "
		             "--service-dry-run to see what it would write.\n";
		return 1;
		}

	Target t;
	// Without sudo there is no SUDO_USER, so a dry run targets whoever is
	// running it -- which is what makes the flag usable, and a test hermetic.
	std::string want = req.user_name;
	if (want.empty() && req.dry_run && ::geteuid() != 0) {
		struct passwd* pw = ::getpwuid(::getuid());
		if (pw && pw->pw_name) want = pw->pw_name;
		}
	if (!resolve_user(want, t, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}

	std::string exec;
	if (!self_exe(exec, err)) {
		std::cerr << "Error: cannot read /proc/self/exe (" << err
		          << "); --install-service cannot tell systemd what to run.\n";
		return 1;
		}
	// A warning and never a refusal: a static binary dropped in ~/bin is a
	// legitimate deployment, and refusing would block the very flow this
	// command exists for -- step (c) follows step (b) in the same build tree.
	fs::path ep(exec);
	bool in_build = fs::exists(ep.parent_path() / "CMakeCache.txt", ec)
	             || fs::exists(ep.parent_path().parent_path() / "CMakeCache.txt", ec);
	bool in_home  = (!t.home.empty() && exec.rfind(t.home + "/", 0) == 0)
	             || exec.rfind("/home/", 0) == 0 || exec.rfind("/root/", 0) == 0;
	if (in_build)
		std::cerr << "Warning: this binary is at " << exec << ", inside a build "
		             "tree; the service will\n         break the next time you "
		             "rebuild or move it. Install it first:\n"
		             "         sudo cmake --install build\n";
	else if (in_home)
		std::cerr << "Warning: this binary is at " << exec << ", under a home "
		             "directory; the service will\n         break if that "
		             "directory is moved or its permissions change.\n";

	// The database, which is the "finish the setup first" check.  It catches
	// both the user who skipped step (a) and the one whose --db is still the
	// root-owned /var/lib/gaindrive default -- and it keys on the derived
	// -client.db rather than on --db, which is never opened as a file, so the
	// `sudo -i` case where ~ expanded to /root fails naming a real path.
	if (!fs::exists(req.client_db_path, ec)) {
		std::cerr << "Error: no gaindrive database at " << req.client_db_path
		          << ".\n       Run gaindrive once as " << t.user
		          << " first, to create the first account.\n";
		return 1;
		}
	std::string why;
	if (!req.dry_run && !reachable_as(req.client_db_path, t, false, why)) {
		std::cerr << "Error: " << t.user << " cannot read " << req.client_db_path
		          << ": " << why << ".\n       The service could not open it.\n";
		return 1;
		}
	if (!req.dry_run && !reachable_as(req.db_dir, t, true, why)) {
		std::cerr << "Error: " << t.user << " cannot write " << req.db_dir
		          << ": " << why << ".\n       SQLite creates the -wal and -shm "
		             "files beside the database, and creates no\n"
		             "       directories.\n";
		return 1;
		}
	for (const auto& r : req.root_paths)
		if (!req.dry_run && !reachable_as(r, t, false, why)) {
			std::cerr << "Error: " << t.user << " cannot read library root "
			          << r << ": " << why << ".\n       Grant access, or pass "
			             "--service-user naming an account that has it; the\n"
			             "       shared-group recipe is in README.md under "
			             "\"Install as a service\".\n";
			return 1;
			}

	// Nothing is being clobbered.  The two files get different rules, and the
	// difference is not an inconsistency: a config legitimately holds hand
	// edits this command never generates -- url_handlers above all -- while a
	// unit is ours by construction, since `systemctl edit` drop-ins are the
	// supported way to change one and they survive a rewrite.  So re-running
	// with one more --category-root, which is the normal second use of this
	// command, needs no flag for the unit and does need one for the config.
	if (fs::exists(req.config_path, ec) && !req.force) {
		std::cerr << "Error: " << req.config_path << " already exists; it is "
		             "not replaced, because it may hold\n       settings this "
		             "command does not generate. Edit it by hand, or re-run "
		             "with\n       --service-force (a copy is kept as "
		          << req.config_path << ".bak).\n";
		return 1;
		}
	if (fs::exists(unit_path, ec) && !req.force && !wrote_by_us(unit_path)) {
		std::cerr << "Error: " << unit_path << " was not written by "
		             "--install-service; nothing has been\n       changed. "
		             "Move it aside, or re-run with --service-force.\n";
		return 1;
		}

	// --- warnings the installer alone is placed to give --------------------

	if (req.port > 0 && req.port < 1024)
		std::cerr << "Warning: port " << req.port << " is below 1024 and the "
		             "service runs as " << t.user << ", which cannot\n"
		             "         bind it. Add AmbientCapabilities="
		             "CAP_NET_BIND_SERVICE with 'systemctl edit\n"
		             "         gaindrive', or put a reverse proxy in front.\n";
	if (req.host == "127.0.0.1" || req.host == "::1")
		std::cerr << "Warning: host is " << req.host << ", so the service will "
		             "only answer on this machine. That is\n         the right "
		             "setting behind a reverse proxy; use --host 0.0.0.0 to "
		             "reach it\n         directly.\n";
	if (req.upload_dir.rfind("/tmp/", 0) == 0)
		std::cerr << "Warning: upload_dir " << req.upload_dir << " is under "
		             "/tmp, which PrivateTmp=yes in the unit\n         makes "
		             "private to the service; it will look empty from "
		             "outside.\n";

	// --- compose ------------------------------------------------------------

	std::string mounts, exec_args, quoted;
	for (const auto& m : req.mount_paths) {
		if (!sd_quote(m, quoted, err)) { std::cerr << "Error: " << err << "\n"; return 1; }
		if (!mounts.empty()) mounts += ' ';
		mounts += quoted;
		}
	for (const auto& a : req.exec_args) {
		if (!sd_quote(a, quoted, err)) { std::cerr << "Error: " << err << "\n"; return 1; }
		exec_args += ' ';
		exec_args += quoted;
		}
	std::string exec_q, config_q;
	if (!sd_quote(exec, exec_q, err) || !sd_quote(req.config_path, config_q, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}

	std::string unit_text;
	if (!fill_template({ { "HEADER", "# Generated by gaindrive "
	                                 + std::string(MARKER) + " on " + today()
	                                 + ".\n# Re-running that command replaces "
	                                   "this file; keep local changes in a\n"
	                                   "# drop-in instead: systemctl edit "
	                                   "gaindrive" },
	                     { "CONFIG",    config_q },
	                     { "MOUNTS",    mounts },
	                     { "USER",      t.user },
	                     { "GROUP",     t.group },
	                     { "EXEC",      exec_q },
	                     { "EXEC_ARGS", exec_args } },
	                   unit_text, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}

	// --- write --------------------------------------------------------------

	if (req.dry_run) {
		std::cout << "# would write " << req.config_path << "  (0640 root:"
		          << t.group << ")\n" << req.config_text
		          << "# would write " << unit_path << "  (0644 root:root)\n"
		          << unit_text
		          << "# would run: systemctl daemon-reload\n"
		             "# would run: systemctl enable " << UNIT_NAME << "\n";
		return 0;
		}

	if (fs::exists(req.config_path, ec)) {
		fs::copy_file(req.config_path, req.config_path + ".bak",
		              fs::copy_options::overwrite_existing, ec);
		if (ec) {
			std::cerr << "Error: cannot back " << req.config_path << " up: "
			          << ec.message() << "; nothing has been changed.\n";
			return 1;
			}
		std::cout << "Kept the previous config as " << req.config_path
		          << ".bak\n";
		}

	// Whether it was running decides whether enabling is enough: a re-run into
	// a live service that is not restarted reports success while the old
	// config goes on being served.
	bool was_active = run_systemctl({ "is-active", "--quiet", UNIT_NAME }, true);

	if (!write_root_file(req.config_path, req.config_text, 0640, 0, t.gid, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}
	std::cout << "Wrote " << req.config_path << " (0640 root:" << t.group << ")\n";

	if (!write_root_file(unit_path, unit_text, 0644, 0, 0, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}
	std::cout << "Wrote " << unit_path << " (0644 root:root)\n";

	if (!run_systemctl({ "daemon-reload" }, false)) return 1;
	if (!run_systemctl({ "enable", UNIT_NAME }, false)) return 1;

	std::cout << "Enabled " << UNIT_NAME << ", running as " << t.user << ":"
	          << t.group << "\n";

	// Deliberately not `enable --now`: the foreground server from the run that
	// proved these options may still hold the port, and a bind failure is a
	// poor way to end an install that otherwise worked.
	if (was_active) {
		std::cout << "It was already running, so restarting it to pick the new "
		             "config up.\n";
		run_systemctl({ "try-restart", UNIT_NAME }, false);
		}
	else
		std::cout << "\nStart it when you are ready:\n"
		             "  sudo systemctl start gaindrive\n"
		             "  systemctl status gaindrive\n"
		             "  journalctl -u gaindrive -f\n";
	return 0;
	}

int uninstall(const UninstallRequest& req)
	{
	const std::string unit_path = (fs::path(unit_dir()) / UNIT_NAME).string();
	std::error_code ec;

	if (!fs::exists(unit_path, ec)) {
		std::cerr << "Error: there is no " << unit_path << " to remove.\n";
		return 1;
		}
	// Checked before anything is stopped: deciding the file may go is not a
	// reason to have already disabled the service it describes.
	if (!req.force && !wrote_by_us(unit_path)) {
		std::cerr << "Error: " << unit_path << " was not written by "
		             "--install-service; nothing has been\n       changed. "
		             "Remove it by hand, or re-run with --service-force.\n";
		return 1;
		}
	if (req.dry_run) {
		std::cout << "# would run: systemctl disable --now " << UNIT_NAME << "\n"
		             "# would remove " << unit_path << "\n"
		             "# would run: systemctl daemon-reload\n";
		return 0;
		}
	if (::geteuid() != 0) {
		std::cerr << "Error: --uninstall-service removes " << unit_path
		          << ", so it must be run with sudo.\n";
		return 1;
		}

	// Not running is not a failure, so its complaint is discarded rather than
	// reported as one.
	run_systemctl({ "stop", UNIT_NAME }, true);
	run_systemctl({ "disable", UNIT_NAME }, true);
	std::cout << "Stopped and disabled " << UNIT_NAME << "\n";

	fs::remove(unit_path, ec);
	if (ec) {
		std::cerr << "Error: cannot remove " << unit_path << ": "
		          << ec.message() << "\n";
		return 1;
		}
	std::cout << "Removed " << unit_path << "\n";
	run_systemctl({ "daemon-reload" }, false);

	// Removing a service must not remove a library, so both are named rather
	// than touched -- there is nothing else that would tell you where they were.
	std::cout << "Left " << req.config_path << " and " << req.db_path
	          << " alone;\ndelete them by hand if you want them gone.\n";
	return 0;
	}

}  // namespace service

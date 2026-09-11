#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <iterator>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <arpa/inet.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

#include <cxxopts.hpp>
#include <nlohmann/json.hpp>

#include "artistmatch.hh"
#include "chapters.hh"
#include "codecs.hh"
#include "gaindrive.hh"
#include "imagescale.hh"
#include "mediastore.hh"
#include "service.hh"
#include "stamp.hh"
#include "tmdb.hh"
#include "untrusted.hh"
#include "videoart.hh"
#include "videoname.hh"

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

// Absolute and tidied, but with symlinks left alone.
//
// Used by both the generated config and the checks --install-service runs
// against it, so the two cannot disagree about what path was tested versus what
// path was written.  Deliberately not weakly_canonical: resolving symlinks
// would rewrite a root that somebody spelled through a stable symlink into
// whatever mount it happens to point at today, and a root's stored name is
// permanent.  (The binary's own path in ExecStart *is* canonicalised, for the
// opposite reason -- see self_exe() in service.cc.)
static std::string abs_path(const std::string& p)
	{
	return std::filesystem::absolute(p).lexically_normal().string();
	}

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
                           std::string& err)
	{
	int uploads = 0;
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
		if (r.type == "uploads") ++uploads;
		}
	if (uploads > 1) {
		err = "at most one uploads root may be configured";
		return false;
		}
	return true;
	}

// Parses one "[name=]address[:port]" cast device argument.
//
// The name is optional, and when it is absent the address stands in as the
// display name. That fallback is load-bearing rather than tidy: the web
// client's picker labels each button with the device's name alone, so a blank
// one would render as an empty button.
//
// A port is only recognised after the last ':' when the address either holds no
// other ':' or is bracketed, because those are the only two cases that can be
// told apart. `[fe80::1%eth0]:8009` is an address and a port; `fe80::1` is an
// address. Guessing at the rest would silently truncate an IPv6 address.
static bool parse_cast_device(const std::string& arg,
                              std::vector<CastManager::CastDevice>& out,
                              std::string& err)
	{
	// Split on the FIRST '=' so an address can never be mistaken for a name,
	// exactly as parse_root does.
	std::string name, rest = arg;
	auto eq = arg.find('=');
	if (eq != std::string::npos) {
		if (eq == 0 || eq + 1 >= arg.size()) {
			err = "expected [name=]address[:port], got '" + arg + "'";
			return false;
			}
		name = arg.substr(0, eq);
		rest = arg.substr(eq + 1);
		}

	std::string address = rest;
	int port = 8009;

	auto close = rest.rfind(']');
	auto colon = rest.rfind(':');
	bool bracketed = !rest.empty() && rest.front() == '['
	              && close != std::string::npos;
	bool has_port = colon != std::string::npos
	             && (bracketed ? colon > close
	                           : rest.find(':') == colon);
	if (has_port) {
		std::string digits = rest.substr(colon + 1);
		if (digits.empty()
		        || digits.find_first_not_of("0123456789") != std::string::npos) {
			err = "port is not a number in '" + arg + "'";
			return false;
			}
		port    = std::atoi(digits.c_str());
		address = rest.substr(0, colon);
		}
	// The brackets are syntax, not part of the address: tls_connect() hands the
	// address straight to inet_pton, which rejects them.
	if (address.size() >= 2 && address.front() == '[' && address.back() == ']')
		address = address.substr(1, address.size() - 2);

	if (address.empty()) {
		err = "empty address in '" + arg + "'";
		return false;
		}

	CastManager::CastDevice dev;
	dev.id      = CastManager::manual_id(address, port);
	dev.name    = name.empty() ? address : name;
	dev.address = address;
	dev.port    = port;
	dev.manual  = true;
	out.push_back(dev);
	return true;
	}

// True for a literal IPv4 or IPv6 address, with an optional %iface scope.
//
// It has to be a literal: CastManager's tls_connect() calls inet_pton and never
// getaddrinfo, so a hostname cannot be connected to at all. That never came up
// while every device came from mDNS, which yields literals.
static bool is_ip_literal(const std::string& address)
	{
	struct in_addr  v4;
	struct in6_addr v6;
	if (inet_pton(AF_INET, address.c_str(), &v4) == 1) return true;
	std::string bare = address.substr(0, address.find('%'));
	return inet_pton(AF_INET6, bare.c_str(), &v6) == 1;
	}

// Configured devices never reach mDNS, so nothing downstream will notice a
// nonsense one until a cast silently fails to start. Reject it here instead.
static bool validate_cast_devices(
	const std::vector<CastManager::CastDevice>& devices, std::string& err)
	{
	std::set<std::string> addresses;
	for (const auto& d : devices) {
		if (d.address.empty()) {
			err = "a cast device has an empty address";
			return false;
			}
		// Named separately from any other failure, because "use the IP" is the
		// fix and nothing else would suggest it — a hostname otherwise reaches
		// the startup probe and comes back as an ordinary "unreachable".
		if (!is_ip_literal(d.address)) {
			err = "cast device address '" + d.address
			    + "' is not an IP address; a hostname cannot be used here";
			return false;
			}
		if (d.port < 1 || d.port > 65535) {
			err = "cast device '" + d.name + "' has port " + std::to_string(d.port)
			    + ", which is not in 1-65535";
			return false;
			}
		// Two entries at one address would carry the same derived id, and
		// startCast picks the first id that matches — so the second would be
		// unselectable rather than merely redundant.
		if (!addresses.insert(d.address).second) {
			err = "duplicate cast device address '" + d.address + "'";
			return false;
			}
		}
	return true;
	}

// Anything that is not the uploads root is library content. Deliberately not
// part of validate_roots(): having no library root is not a malformed
// configuration, it only matters at the moment the server would actually read
// one, and a start that stops before then must not be turned away for it.
static bool has_library_root(const std::vector<MediaStore::Root>& roots)
	{
	for (const auto& r : roots)
		if (r.type != "uploads") return true;
	return false;
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

// The name to parse, given a path. The extension is only stripped when it is
// actually a video extension: a bare release name is full of dots, and
// stem() on "The.Third.Man.1949.1080p" would eat ".1080p" as an extension.
static std::string name_to_parse(const std::filesystem::path& p)
	{
	std::string ext = p.extension().string();
	if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
	std::transform(ext.begin(), ext.end(), ext.begin(),
	               [](unsigned char c) { return std::tolower(c); });
	return is_video_ext(ext) ? p.stem().string() : p.filename().string();
	}

static void print_video_name(const std::filesystem::path& p,
                              const std::string& label)
	{
	auto v = resolve_video_name(name_to_parse(p),
	                            p.parent_path().filename().string(),
	                            p.parent_path().parent_path().filename().string());
	std::string ep;
	if (v.season > 0)
		ep = "S" + std::to_string(v.season) + "E" + std::to_string(v.episode);
	else if (v.episode > 0)
		ep = "E" + std::to_string(v.episode);
	// Tab separated, one line per file: readable in a terminal and parseable
	// by tests/test_video_names.py, which is what regression-tests the rules.
	std::cout << label << '\t' << v.title << '\t'
	          << (v.year ? std::to_string(v.year) : "") << '\t'
	          << ep << '\t' << (v.from_folder ? "folder" : "file") << '\t'
	          << (v.cleaned ? "cleaned" : "as-is") << '\t'
	          << v.series_title << '\n';
	}

// --chapters-test: what the sidecar parser makes of a chapter file, with no
// database, no scan and no server.
//
// It takes a *file*, and deliberately not the "-" stdin seam --video-name-test
// uses: that reads one name per line, and a chapter file is inherently
// multi-line, so the two could not mean the same thing. The regression table in
// tests/test_chapters_parse.py drives this with one fixture file per case.
static int run_chapters_test(const std::string& target)
	{
	std::ifstream f(target, std::ios::binary);
	if (!f) {
		std::cerr << "Cannot read " << target << "\n";
		return 1;
		}
	std::string text((std::istreambuf_iterator<char>(f)),
	                  std::istreambuf_iterator<char>());

	auto p = parse_chapters(text);
	// Tab separated, one line per marker -- index, time, title -- then a
	// summary on stderr so stdout stays exactly the parse. The time is printed
	// through format_chapter_time() rather than as a raw double so that what
	// the test compares is what would be written back to the file, which is
	// where a truncating round-trip would show up.
	for (size_t i = 0; i < p.chapters.size(); i++)
		std::cout << (i + 1) << '\t'
		          << format_chapter_time(p.chapters[i].start) << '\t'
		          << p.chapters[i].name << '\n';
	std::cerr << p.chapters.size() << " chapter(s), "
	          << p.skipped << " line(s) skipped\n";
	return 0;
	}

// --untrusted-test: what the provider-string guards make of a value, with no
// database, no roots and no network.
//
// One `kind:value` per line on stdin, kind naming which guard to apply, and
// one line of output each so a table in tests/test_untrusted.py can compare
// them. It exists for the reason --artist-pick-test and --video-name-test do:
// this is where a wrong answer would come from, so it is the part worth being
// able to exercise directly — and a rule about hostile input is only as good
// as the cases somebody wrote down.
//
// Non-printable input has to survive getting here, so a value may carry \xNN,
// \r, \n, \t and \\ escapes; output is escaped the same way. That is
// dump_derived.py's convention and it is here for its reason: a test whose
// whole subject is control characters cannot put them on a line raw.
static std::string untrusted_unescape(const std::string& in)
	{
	std::string out;
	for (size_t i = 0; i < in.size(); ++i) {
		if (in[i] != '\\' || i + 1 >= in.size()) { out += in[i]; continue; }
		char c = in[++i];
		switch (c) {
			case 'n':  out += '\n'; break;
			case 'r':  out += '\r'; break;
			case 't':  out += '\t'; break;
			case '\\': out += '\\'; break;
			// Parsed by hand rather than with std::stoi, which *throws* on a
			// pair that is not hex — in a mode whose whole subject is
			// malformed input, a fixture typo should be a visible passthrough
			// and not an abort.
			case 'x': {
				auto nib = [](char h) -> int {
					if (h >= '0' && h <= '9') return h - '0';
					if (h >= 'a' && h <= 'f') return h - 'a' + 10;
					if (h >= 'A' && h <= 'F') return h - 'A' + 10;
					return -1;
					};
				int hi = i + 1 < in.size() ? nib(in[i + 1]) : -1;
				int lo = i + 2 < in.size() ? nib(in[i + 2]) : -1;
				if (hi < 0 || lo < 0) { out += "\\x"; break; }
				out += (char)(hi * 16 + lo);
				i += 2;
				break;
				}
			default: out += '\\'; out += c;
			}
		}
	return out;
	}

static std::string untrusted_escape(const std::string& in)
	{
	static const char* hex = "0123456789abcdef";
	std::string out;
	for (unsigned char c : in) {
		if (c == '\\')      out += "\\\\";
		else if (c == '\n') out += "\\n";
		else if (c == '\r') out += "\\r";
		else if (c == '\t') out += "\\t";
		else if (c < 0x20 || c == 0x7f) {
			out += "\\x"; out += hex[c >> 4]; out += hex[c & 15];
			}
		else out += (char)c;
		}
	return out;
	}

static int run_untrusted_test()
	{
	std::string line;
	while (std::getline(std::cin, line)) {
		if (!line.empty() && line.back() == '\r') line.pop_back();
		if (line.empty() || line[0] == '#') continue;
		auto colon = line.find(':');
		if (colon == std::string::npos) {
			std::cerr << "no kind on line: " << line << "\n";
			return 1;
			}
		std::string kind = line.substr(0, colon);
		std::string val  = untrusted_unescape(line.substr(colon + 1));

		if      (kind == "url")   std::cout << untrusted_escape(clean_url(val));
		else if (kind == "prose") std::cout << untrusted_escape(
		                                        clean_prose(val, MAX_PROSE_BYTES));
		else if (kind == "genre") std::cout << untrusted_escape(clean_genre(val));
		else if (kind == "uuid")  std::cout << (is_uuid(val) ? "yes" : "no");
		else {
			std::cerr << "unknown kind: " << kind << "\n";
			return 1;
			}
		std::cout << '\n';
		}
	return 0;
	}

// --artist-pick-test: the query that would go to MusicBrainz for an artist
// name, and which of a search's results the matching rule chooses.
//
// It reads the response body on **stdin** and talks to nothing, which is the
// whole point: the rule that decides whose biography and portrait a folder
// gets is exercised with no network, no key and no database, and
// tests/test_artist_pick.py drives it as a regression table the way
// test_video_names.py drives the filename parser. Feeding it a live body is
// one curl away, so this needs no online mode of its own:
//
//   curl -s 'https://musicbrainz.org/ws/2/artist/?query=...&fmt=json&limit=8' \
//     | gaindrive --artist-pick-test 'Hiromi Uehara'
//
// stdout is exactly the decision, tab separated, so a test can compare it
// without parsing prose; the query goes to stderr beside it.
static int run_artist_pick_test(const std::string& name)
	{
	std::string body((std::istreambuf_iterator<char>(std::cin)),
	                  std::istreambuf_iterator<char>());

	std::cerr << "query: " << mb_artist_query(name) << "\n";

	auto pick = mb_pick_artist(body, name);
	if (!pick) {
		std::cout << "(no match)\n";
		return 0;
		}
	std::cout << pick->mbid << '\t' << pick->name << '\t'
	          << pick->score << '\t'
	          << (pick->exact ? "exact" : "guess") << '\n';
	return 0;
	}

// --video-name-test: what the filename parser makes of a name, with no
// database, no scan and no server. A directory is walked; "-" reads names on
// stdin, one per line, which is the seam the regression test drives.
static int run_video_name_test(const std::string& target)
	{
	namespace fs = std::filesystem;
	if (target == "-") {
		std::string line;
		while (std::getline(std::cin, line)) {
			if (line.empty()) continue;
			print_video_name(fs::path(line), line);
			}
		return 0;
		}

	std::error_code ec;
	if (fs::is_directory(target, ec)) {
		int n = 0;
		for (auto& e : fs::recursive_directory_iterator(
		         target, fs::directory_options::skip_permission_denied, ec)) {
			if (!e.is_regular_file()) continue;
			std::string ext = e.path().extension().string();
			if (!ext.empty() && ext.front() == '.') ext.erase(0, 1);
			std::transform(ext.begin(), ext.end(), ext.begin(),
			               [](unsigned char c) { return std::tolower(c); });
			if (!is_video_ext(ext)) continue;
			print_video_name(e.path(), e.path().filename().string());
			n++;
			}
		if (ec) {
			std::cerr << "Cannot read " << target << ": " << ec.message() << "\n";
			return 1;
			}
		std::cerr << n << " video files\n";
		return 0;
		}

	if (!fs::exists(target, ec)) {
		std::cerr << "No such file or directory: " << target << "\n";
		return 1;
		}
	print_video_name(fs::path(target), target);
	return 0;
	}

// --tmdb-test: one query against TMDB, printed. The matching rule is the part
// of this feature that decides whether a poster is right or confidently wrong,
// so being able to try a name without running a scan is what it is for.
static int run_tmdb_test(const std::string& title, int year, bool tv,
                          const std::string& key)
	{
	Tmdb tmdb(key);
	if (!tmdb.configured()) {
		std::cerr << "No TMDB API key. Pass --tmdb-key, or set one in the "
		             "client's Settings screen.\n";
		return 1;
		}
	auto m = tmdb.search(title, year, tv);
	if (!m) {
		std::cout << "no confident match for \"" << title << "\""
		          << (year ? " (" + std::to_string(year) + ")" : "") << "\n";
		return 1;
		}
	std::cout << m->title << " (" << m->year << ")  tmdb id " << m->id
	          << (m->poster_path.empty() ? "  [no poster]" : "")
	          << "\n";
	// The genres are the reason this prints more than it used to: they are
	// resolved through a second pair of requests, so seeing them here is the
	// cheapest proof that the id->name map loaded at all.
	std::cout << "genres: ";
	if (m->genres.empty()) std::cout << "(none)";
	for (size_t i = 0; i < m->genres.size(); ++i)
		std::cout << (i ? ", " : "") << m->genres[i];
	std::cout << "\n" << m->overview << "\n";
	return 0;
	}

// --url-fetch-test: which handler claims a URL, and exactly what would be run
// for it. A dry run — nothing is fetched and nothing is written.
//
// This is where an operator's own handler entry is debugged. The mistakes it
// catches are the ones that are otherwise invisible: a pattern that matches
// nothing because it was written for regex_search, an output template that
// writes too shallow to be promotable, a %DIR% that ended up inside -o.
static int run_url_fetch_test(
	const std::string& url,
	const std::optional<std::vector<UrlHandler>>& handlers)
	{
	UrlFetcher fetcher(handlers);
	if (!fetcher.configured()) {
		std::cerr << "No usable URL handlers.\n";
		return 1;
		}
	std::cout << "handlers:\n";
	for (const auto& c : fetcher.capabilities())
		std::cout << "  " << c.name
		          << (c.audio ? "  audio" : "") << (c.video ? "  video" : "")
		          << "\n";

	if (!urlfetch_http_url(url)) {
		std::cout << "\nrefused: only http and https URLs can be fetched.\n";
		return 1;
		}
	const UrlHandler* h = fetcher.match(url);
	if (!h) {
		std::cout << "\nrefused: no handler is configured for that URL.\n";
		return 1;
		}

	std::cout << "\nmatched: " << h->name << "\n";
	auto show = [&url, h](const char* label,
	                       const std::vector<std::string>& tmpl) {
		std::cout << label << ": ";
		if (tmpl.empty()) { std::cout << "(refused by this handler)\n"; return; }
		for (const auto& a : urlfetch_expand(tmpl, url, "<batch dir>"))
			std::cout << ' ' << a;
		std::cout << "\n";
		};
	show("audio", h->audio_argv);
	show("video", h->video_argv);
	return 0;
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
		("cast-device",   "Chromecast that does not announce itself, as [name=]IP[:port] (repeatable)", cxxopts::value<std::vector<std::string>>())
		("cast-probe",    "Connect to one Chromecast by IP, report whether it answered, and exit", cxxopts::value<std::string>())
		("trusted-proxy", "Address of a reverse proxy whose X-Forwarded-For may be believed (repeatable; default loopback)", cxxopts::value<std::vector<std::string>>())
		("public-url", "Origin clients reach this server at, e.g. https://music.example.org. Used for the URLs a Chromecast is told to fetch; without it the client's own Host header is believed", cxxopts::value<std::string>())
		("upload-dir", "Directory for uploaded archives", cxxopts::value<std::string>()->default_value("/tmp/gaindrive-uploads"))
		("transcode-cache",    "Directory for cached transcodes (default: alongside --db)", cxxopts::value<std::string>())
		("transcode-cache-mb", "Transcode cache size in MB (0 disables)", cxxopts::value<int>()->default_value("1024"))
		("transcode-jobs",     "Max concurrent ffmpeg transcodes (0 = half the cores)", cxxopts::value<int>()->default_value("0"))
		("scan-jobs",          "Files a scan reads metadata from at once (0 = up to 8, by core count; 1 = sequential)", cxxopts::value<int>()->default_value("0"))
		("no-video-art",  "Do not manufacture cover art for videos that have none")
		("video-art-px",  "Long edge of manufactured video cover art", cxxopts::value<int>()->default_value("640"))
		("video-art-frames", "Fall back to an extracted frame when a video has no embedded cover")
		("video-art-embedded", "Take the cover embedded in a video container; off because most files have none and looking costs an ffprobe each")
		("video-art-test","Write cover art for one video file and exit", cxxopts::value<std::string>())
		("image-scale-test","Scale one image file at each cover size and exit", cxxopts::value<std::string>())
		("video-name-test","Parse video filenames and exit; takes a file, a directory, or - for stdin", cxxopts::value<std::string>())
		("chapters-test", "Parse one .chapters.txt file and exit", cxxopts::value<std::string>())
		("artist-pick-test","Pick a MusicBrainz artist from a search response on stdin and exit", cxxopts::value<std::string>())
		("untrusted-test","Apply the provider-string guards to kind:value lines on stdin and exit")
		("tmdb-test",     "Look one title up on TMDB and exit", cxxopts::value<std::string>())
		("tmdb-year",     "Year for --tmdb-test", cxxopts::value<int>()->default_value("0"))
		("tmdb-key",      "API key for --tmdb-test (default: the stored setting)", cxxopts::value<std::string>())
		("tmdb-tv",       "Search series rather than films for --tmdb-test")
		("url-fetch-test","Show which handler claims one URL and what would be run, then exit", cxxopts::value<std::string>())
		("no-scan",    "Skip startup filesystem scan")
		("debug",      "Print all API responses to stdout")
		("add-user",   "Create a user and exit",      cxxopts::value<std::string>())
		("password",   "Password for --add-user",     cxxopts::value<std::string>())
		("install-service",   "Write the config and a systemd unit from these options, enable it, and exit")
		("uninstall-service", "Stop, disable and remove the unit written by --install-service, and exit")
		("service-user",      "Account the service runs as (default: the user who invoked sudo)", cxxopts::value<std::string>())
		("service-force",     "Replace a config or unit that --install-service did not write")
		("service-dry-run",   "Print what --install-service would write, write nothing, and exit")
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

	if (args.count("video-name-test"))
		return run_video_name_test(args["video-name-test"].as<std::string>());

	if (args.count("chapters-test"))
		return run_chapters_test(args["chapters-test"].as<std::string>());

	// Beside the others that need nothing at all: no database, no roots, no
	// config and no network, since the values arrive on stdin.
	if (args.count("untrusted-test"))
		return run_untrusted_test();

	// Beside the other two that need nothing at all: no database, no roots, no
	// config and no network, since the response body arrives on stdin.
	if (args.count("artist-pick-test"))
		return run_artist_pick_test(args["artist-pick-test"].as<std::string>());

	// Also before any database or root: asking one device whether it is there
	// needs neither, and this is the check that says whether an address put in
	// --cast-device or the config file is any good.
	if (args.count("cast-probe")) {
		std::vector<CastManager::CastDevice> one;
		std::string err;
		if (!parse_cast_device(args["cast-probe"].as<std::string>(), one, err)) {
			std::cerr << "Error: --cast-probe: " << err << "\n";
			return 1;
			}
		CastManager cast;
		auto result = cast.probe(one.front());
		std::cout << one.front().address << ":" << one.front().port << " — "
		          << CastManager::probe_text(result) << std::endl;
		return result == CastManager::Probe::ANSWERED ? 0 : 1;
		}


	// Before anything else touches a database or a root: this is a standalone
	// check of what VideoArt makes of one file, meant for judging the result
	// by eye across a sample of a collection without running a scan.
	if (args.count("video-art-test")) {
		std::string in     = args["video-art-test"].as<std::string>();
		bool        frames = args.count("video-art-frames") > 0;
		// The embedded tier is forced on here whatever the configuration says,
		// because answering "is there a cover inside this file" is the whole
		// job of this flag — with the tier off as it is by default, the tool
		// would report nothing for every file and tell you nothing about your
		// collection. --video-art-frames is still honoured, so what this
		// prints for the *second* tier is what a scan would store.
		VideoArt    art(args["video-art-px"].as<int>(), frames, true);
		auto        result = art.generate(in);
		if (!result) {
			std::cerr << "No cover art could be made from " << in
			          << (frames ? "\n"
			                     : " (add --video-art-frames to allow a frame"
			                       " grab)\n");
			return 1;
			}
		std::string out = std::filesystem::path(in).stem().string() + "-art"
		    + (result->mime == "image/png" ? ".png" : ".jpg");
		std::ofstream f(out, std::ios::binary);
		f.write(result->bytes.data(),
		        static_cast<std::streamsize>(result->bytes.size()));
		if (!f) {
			std::cerr << "Could not write " << out << "\n";
			return 1;
			}
		std::cout << out << ": " << result->source << ", "
		          << result->bytes.size() << " bytes, " << result->mime << "\n";
		return 0;
		}

	// Standalone check of what the scaler makes of one image, with no server,
	// no database and no library.  Every future report about cover art on this
	// path is really asking one of two questions — can gaindrive decode this
	// file, and what does it produce — and this answers both without needing
	// the file to be in a collection first.
	if (args.count("image-scale-test")) {
		std::string in = args["image-scale-test"].as<std::string>();

		std::ifstream f(in, std::ios::binary);
		if (!f) { std::cerr << "Cannot open " << in << "\n"; return 1; }
		std::string raw((std::istreambuf_iterator<char>(f)),
		                 std::istreambuf_iterator<char>());

		std::string mime = imagescale::sniff_mime(raw);
		auto        dims = imagescale::probe(raw);
		std::cout << in << ": " << raw.size() << " bytes, "
		          << (mime.empty() ? "unrecognised" : mime) << ", ";
		if (dims)
			std::cout << dims->width << "x" << dims->height << ", "
			          << dims->channels << " channels\n";
		else
			std::cout << "header not parsable by stb\n";

		// The sizes gaindrive's own clients ask for: the web client's player
		// thumbnail, grid cell, mediaSession artwork and hero, each on a 1x
		// and a 2x screen, plus Android's and iOS's.
		//
		// Fit::Short, because that is what getCoverArt asks for and the
		// question this flag exists to answer is what getCoverArt would send.
		// So a non-square source reports a long edge past the size given.
		for (int px : {64, 80, 96, 128, 144, 160, 256, 288, 320, 512, 640, 800}) {
			auto s = imagescale::scale_to_fit(raw, px, imagescale::Fit::Short);
			std::cout << "  size=" << px << ": ";
			if (!s.ok) { std::cout << "FAILED — " << s.error << "\n"; continue; }
			std::cout << s.width << "x" << s.height << ", " << s.bytes.size()
			          << " bytes, " << s.mime
			          << (s.bytes.size() == raw.size()
			                  ? "  (source returned unchanged)" : "")
			          << "\n";
			if (px != 640) continue;
			std::string out = std::filesystem::path(in).stem().string()
			    + "-640" + (s.mime == "image/png" ? ".png" : ".jpg");
			std::ofstream o(out, std::ios::binary);
			o.write(s.bytes.data(), static_cast<std::streamsize>(s.bytes.size()));
			if (o) std::cout << "  wrote " << out << "\n";
			}
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

	// Which peers' X-Forwarded-For may be believed. Empty here means "leave
	// the built-in default", which is loopback — the reverse proxy the
	// packaging sets up. It is a list rather than a flag because the header is
	// what the login throttle keys on, so believing it from the wrong peer
	// means an attacker choosing their own rate-limit bucket.
	std::vector<std::string> trusted_proxies;
	if (args.count("trusted-proxy"))
		trusted_proxies = args["trusted-proxy"].as<std::vector<std::string>>();

	// The origin this server is reachable at, when it is not the one clients
	// name in Host — i.e. behind a reverse proxy. Everything a Chromecast is
	// told to fetch is built on it; the fallback is the request's own Host
	// header, which is right on a LAN and attacker-chosen on the internet.
	std::string public_url;
	if (args.count("public-url"))
		public_url = args["public-url"].as<std::string>();

	std::vector<CastManager::CastDevice> cast_devices;
	if (args.count("cast-device")) {
		std::string err;
		// A cxxopts vector option also splits on commas, so a device name
		// holding one is not expressible. --artist-root has the same limit.
		for (const auto& v : args["cast-device"].as<std::vector<std::string>>())
			if (!parse_cast_device(v, cast_devices, err)) {
				std::cerr << "Error: --cast-device: " << err << "\n";
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
	int         scan_jobs           = args["scan-jobs"].as<int>();
	// 0 switches the whole thing off, which is what --no-video-art means.
	int         video_art_px        = args.count("no-video-art")
	    ? 0 : args["video-art-px"].as<int>();
	bool        video_art_frames    = args.count("video-art-frames") > 0;
	bool        video_art_embedded  = args.count("video-art-embedded") > 0;
	// nullopt until the config file says otherwise — see the note at the read.
	std::optional<std::vector<UrlHandler>> url_handlers;
	int         url_fetch_timeout   = 2 * 60 * 60;

	// Read config file; missing file is not fatal, just use defaults.
	std::string config_path = args["config"].as<std::string>();
	std::ifstream cfg_file(config_path);
	if (cfg_file) {
		// The config is not merely settings: `url_handlers` is an argv vector
		// that this process will execute, so anyone who can write this file has
		// code execution as the service user, and anyone who can read it may
		// learn the database's location. A warning rather than a refusal,
		// because a development tree legitimately keeps one at looser modes and
		// failing to start over it would be worse than saying so.
		{
		struct stat st{};
		if (::stat(config_path.c_str(), &st) == 0) {
			if (st.st_mode & S_IWOTH)
				std::cerr << "Warning: " << config_path << " is world-writable; "
				          << "anything written there runs as this user. "
				          << "chmod 0640 it.\n";
			else if (st.st_mode & S_IROTH)
				std::cerr << "Warning: " << config_path << " is world-readable. "
				          << "chmod 0640 it.\n";
			}
		}
		try {
			nlohmann::json cfg = nlohmann::json::parse(cfg_file);
			// CLI wins over config, the same rule as --user-db and --port
			// below.  These three carried no guard, so with any config file
			// present `--host 0.0.0.0` was read, accepted and silently
			// discarded -- which also contradicted what README.md said.
			// --install-service is what turned that from annoying into fatal:
			// run against an existing config it would serialise the config's
			// own host straight back into the config and report having written
			// what you asked for.
			if (cfg.contains("host") && !args.count("host"))
				host = cfg["host"];
			if (cfg.contains("db_path") && !args.count("db"))
				db_path = cfg["db_path"];
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
			// Cast devices follow the roots rule rather than the args.count()
			// one: the command line and the config file do not merge, so a
			// single --cast-device replaces the configured list rather than
			// adding to it.
			if (cfg.contains("cast_devices") && cast_devices.empty()) {
				for (const auto& d : cfg["cast_devices"]) {
					CastManager::CastDevice dev;
					dev.address = d.value("address", std::string());
					dev.port    = d.value("port", 8009);
					dev.name    = d.value("name", std::string());
					if (dev.name.empty()) dev.name = dev.address;
					dev.id      = CastManager::manual_id(dev.address, dev.port);
					dev.manual  = true;
					cast_devices.push_back(dev);
					}
				}
			// Same non-merging rule as cast_devices: naming one on the
			// command line replaces the configured list.
			if (cfg.contains("trusted_proxies") && trusted_proxies.empty())
				for (const auto& a : cfg["trusted_proxies"])
					trusted_proxies.push_back(a.get<std::string>());
			if (cfg.contains("public_url") && !args.count("public-url"))
				public_url = cfg["public_url"].get<std::string>();
			if (cfg.contains("upload_dir") && !args.count("upload-dir"))
				upload_dir = cfg["upload_dir"];
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
			if (cfg.contains("scan_jobs") && !args.count("scan-jobs"))
				scan_jobs = cfg["scan_jobs"].get<int>();
			// Same key for both flags: --no-video-art is just 0 px, so a
			// config that sets 0 turns it off exactly as the flag does.
			if (cfg.contains("video_art_px") && !args.count("video-art-px")
			        && !args.count("no-video-art"))
				video_art_px = cfg["video_art_px"].get<int>();
			if (cfg.contains("video_art_frames") && !args.count("video-art-frames"))
				video_art_frames = cfg["video_art_frames"].get<bool>();
			if (cfg.contains("video_art_embedded") && !args.count("video-art-embedded"))
				video_art_embedded = cfg["video_art_embedded"].get<bool>();
			// Config file only — a regex plus an argv template is not a sane
			// command-line argument, and a cxxopts vector option splits on
			// commas anyway. Follows flat_multi_disc, which has no flag either.
			//
			// The optional is the point: the key being absent must mean "use the
			// built-in table" while an explicit [] must mean "no fetching from a
			// URL on this server", and an empty vector cannot say both.
			if (cfg.contains("url_handlers")) {
				std::vector<UrlHandler> hs;
				for (const auto& h : cfg["url_handlers"]) {
					UrlHandler uh;
					uh.name    = h.value("name",  std::string());
					uh.pattern = h.value("match", std::string());
					auto argv_of = [&h](const char* key) {
						std::vector<std::string> v;
						if (h.contains(key))
							for (const auto& a : h[key])
								v.push_back(a.get<std::string>());
						return v;
						};
					uh.audio_argv = argv_of("audio");
					uh.video_argv = argv_of("video");
					hs.push_back(std::move(uh));
					}
				// Compiled and checked by UrlFetcher's constructor, not here, so
				// there is one place that reports a bad table.
				url_handlers = std::move(hs);
				}
			if (cfg.contains("url_fetch_timeout"))
				url_fetch_timeout = cfg["url_fetch_timeout"].get<int>();
			}
		catch (const std::exception& e) {
			std::cerr << "Warning: failed to parse " << config_path << ": " << e.what() << "\n";
			}
		}

	// The writer sits immediately below the reader deliberately.  These key
	// names are the same fact twice, and the way that fact goes wrong is not a
	// crash: a renamed key on one side produces a file the other side parses,
	// ignores, and starts on defaults from -- with a clean log saying
	// everything is fine.  In one screen the miss is a review comment.  It is
	// also why service.cc takes the config already serialised and never builds
	// one: it would be a third place that knew these names.
	//
	// ordered_json rather than json: nlohmann's default object is a sorted
	// std::map, so a plain json would emit the file alphabetically and nothing
	// generated could be read beside dist/gaindrive.conf.example.  Insertion
	// order here is that file's order, and dump(1, '\t') is its indentation.
	//
	// A key whose value this invocation does not have is left out rather than
	// written empty, because for three of them absent and present differ in
	// meaning: url_handlers absent is "use the built-in yt-dlp table" (writing
	// today's table into every generated config would freeze a copy of it),
	// user_db_path absent is "derive it from db_path", and transcode_cache_dir
	// absent is "beside the database".  Everything else is written even at its
	// default, since a generated config is meant to be read and edited and a
	// key that is not there cannot be discovered.
	auto effective_config = [&]() {
		nlohmann::ordered_json c;
		c["//generated"] =
		    "Written by gaindrive --install-service. Re-running that command "
		    "replaces this file. Every key is explained in "
		    "gaindrive.conf.example, installed beside it.";
		c["host"]    = host;
		c["port"]    = port;
		c["db_path"] = db_path;
		if (!user_db_path.empty()) c["user_db_path"] = user_db_path;
		if (!trusted_proxies.empty()) c["trusted_proxies"] = trusted_proxies;
		if (!public_url.empty()) c["public_url"] = public_url;

		nlohmann::ordered_json rs = nlohmann::ordered_json::array();
		for (const auto& r : roots)
			rs.push_back({ { "name", r.name }, { "type", r.type },
			               { "path", abs_path(r.path) } });
		c["roots"] = rs;

		if (!cast_devices.empty()) {
			nlohmann::ordered_json ds = nlohmann::ordered_json::array();
			for (const auto& d : cast_devices)
				ds.push_back({ { "name", d.name }, { "address", d.address },
				               { "port", d.port } });
			c["cast_devices"] = ds;
			}

		// Only when the optional holds one.  nullopt means "use the built-in
		// table" and an explicit [] means "no fetching from a URL on this
		// server"; writing [] for nullopt would silently turn the feature off.
		// The same NULL-versus-empty distinction songs.artist draws.
		if (url_handlers) {
			nlohmann::ordered_json hs = nlohmann::ordered_json::array();
			for (const auto& h : *url_handlers)
				hs.push_back({ { "name", h.name }, { "match", h.pattern },
				               { "audio", h.audio_argv },
				               { "video", h.video_argv } });
			c["url_handlers"] = hs;
			}
		c["url_fetch_timeout"] = url_fetch_timeout;

		c["upload_dir"]         = upload_dir;
		c["flat_multi_disc"]    = flat_multi_disc;
		if (!transcode_cache_dir.empty())
			c["transcode_cache_dir"] = transcode_cache_dir;
		c["transcode_cache_mb"]  = transcode_cache_mb;
		c["transcode_jobs"]      = transcode_jobs;
		c["scan_jobs"]           = scan_jobs;
		c["video_art_px"]        = video_art_px;
		c["video_art_frames"]    = video_art_frames;
		c["video_art_embedded"]  = video_art_embedded;
		return c;
		};

	// --tmdb-test needs the database only to read the stored API key, and no
	// roots at all, so it runs before the library is validated.
	if (args.count("tmdb-test")) {
		std::string key = args.count("tmdb-key")
		    ? args["tmdb-key"].as<std::string>() : "";
		if (key.empty()) {
			// The key normally lives in the database, so read it from there
			// rather than making the flag useless without a second argument.
			// video_art_px 0 so opening the store manufactures nothing.
			try {
				MediaStore store(db_path, {}, user_db_path, 0);
				key = store.get_setting("tmdb_key");
				}
			catch (const std::exception& e) {
				std::cerr << "Cannot read the stored API key (" << e.what()
				          << "); pass --tmdb-key.\n";
				}
			}
		return run_tmdb_test(args["tmdb-test"].as<std::string>(),
		                      args["tmdb-year"].as<int>(),
		                      args.count("tmdb-tv") > 0, key);
		}

	// After the config read, since the handler table is the thing being tested;
	// before the library validation, since it needs no roots and no database.
	if (args.count("url-fetch-test"))
		return run_url_fetch_test(args["url-fetch-test"].as<std::string>(),
		                           url_handlers);

	// Before validate_roots(), unlike --install-service: a root since unmounted
	// or deleted must not be what stops somebody removing the service that
	// points at it.  Nothing here needs a root, a database or even a config --
	// the two paths are printed, never opened.
	if (args.count("uninstall-service")) {
		if (args.count("install-service")) {
			std::cerr << "Error: --install-service and --uninstall-service "
			             "cannot be combined.\n";
			return 1;
			}
		service::UninstallRequest req;
		req.config_path = config_path;
		req.db_path     = db_path;
		req.force       = args.count("service-force")   > 0;
		req.dry_run     = args.count("service-dry-run") > 0;
		return service::uninstall(req);
		}

	// Only the shape of what was configured is checked here. Whether a library
	// root is *required* is decided further down, once it is known whether this
	// process is going to serve a library at all.
	{
	std::string err;
	if (!validate_roots(roots, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}
	if (!validate_cast_devices(cast_devices, err)) {
		std::cerr << "Error: " << err << "\n";
		return 1;
		}
	}

	// Clamped rather than rejected: a number out of range is a typo, most
	// likely in a config file whose author may not be reading this log, and
	// refusing to start over it is worse than starting sensibly and saying so.
	// The upper bound is about the disk, not the machine — see
	// MediaStore::scan_jobs_, which is also where 0 is resolved, so it must
	// pass through here rather than being warned up to 1.
	if (scan_jobs < 0 || scan_jobs > 16) {
		int clamped = std::clamp(scan_jobs, 0, 16);
		std::cerr << "Warning: --scan-jobs " << scan_jobs
		          << " is outside 0..16; using " << clamped << "\n";
		scan_jobs = clamped;
		}

	// After the config merge and the validation above, so what is written is
	// the effective configuration this invocation would have served rather than
	// whatever happened to be typed; before the try below, because that opens
	// the database and this process is root -- a root-owned
	// gaindrive-music.db-wal beside the user's database is a service that
	// starts once and never again.
	if (args.count("install-service")) {
		if (args.count("add-user")) {
			std::cerr << "Error: --add-user and --install-service cannot be "
			             "combined; create the account\n       first, then "
			             "install the service.\n";
			return 1;
			}
		// Checked here rather than left to the try below, which this must not
		// reach: a service with nothing to serve is not worth installing, and
		// the message down there assumes a terminal that is about to be given
		// a library.
		if (!has_library_root(roots)) {
			std::cerr << "Error: no library root configured; pass at least one "
			             "--artist-root or\n       --category-root, the same "
			             "ones you want the service to use.\n";
			return 1;
			}
		// Neither is a config key, so neither survives into the unit.  Saying
		// so is the point: --no-scan in particular looks like it would be kept.
		for (const char* f : { "no-scan", "debug" })
			if (args.count(f))
				std::cerr << "Warning: --" << f << " is not a config-file "
				             "setting and is not written; the\n         service "
				             "will run without it.\n";

		service::InstallRequest req;
		req.config_path = config_path;
		req.config_text = effective_config().dump(1, '\t') + "\n";
		// Only when it is not the path compiled into this binary, so the
		// common unit takes no arguments -- as the packaged one always did.
		if (config_path != std::string(GAINDRIVE_DEFAULT_CONFIG))
			req.exec_args = { "--config", config_path };
		req.client_db_path = user_db_path.empty()
		    ? MediaStore::client_db_path(db_path) : user_db_path;
		req.db_dir = std::filesystem::path(abs_path(db_path)).parent_path().string();
		req.mount_paths.push_back(req.db_dir);
		for (const auto& r : roots) {
			req.root_paths.push_back(abs_path(r.path));
			req.mount_paths.push_back(abs_path(r.path));
			}
		req.host       = host;
		req.port       = port;
		req.upload_dir = upload_dir;
		req.user_name  = args.count("service-user")
		    ? args["service-user"].as<std::string>() : "";
		req.force      = args.count("service-force")   > 0;
		req.dry_run    = args.count("service-dry-run") > 0;
		return service::install(req);
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
			// The art settings are passed even here, where no scan runs:
			// opening a store purges art whose tier is now off, and
			// --add-user must not decide that on default settings.
			MediaStore store(db_path, roots, user_db_path, video_art_px,
			                 video_art_frames, video_art_embedded);
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
		MediaStore store(db_path, roots, user_db_path, video_art_px,
		                 video_art_frames, video_art_embedded);
		bool first_start = store.list_users().empty();
		if (first_start && !create_first_user(store))
			return 1;

		// A library root can only come from the command line or the config
		// file, so a server started without one has nothing to serve and no
		// way of being given anything — hence an error rather than an empty
		// library. It is checked here rather than with the rest of the root
		// validation because a first start reads no music: it creates the
		// account and stops, and complaining about a library it was never
		// going to open only obscures the one thing the operator has to do
		// first.
		if (!has_library_root(roots)) {
			if (first_start) {
				std::cout << "\nNothing is configured to serve yet. Start the "
				             "server with at least one library root:\n"
				             "  gaindrive --artist-root music=/path/to/music\n";
				return 0;
				}
			std::cerr << "Error: no library root configured; pass at least one "
			             "--artist-root or --category-root\n";
			return 1;
			}
		}

		// Before the server exists, because client_addr() is consulted by the
		// very first request's log line. Empty leaves the loopback default.
		if (!trusted_proxies.empty()) {
			gaindrive_set_trusted_proxies(trusted_proxies);
			std::cout << stamp() << "Trusting X-Forwarded-For from "
			          << trusted_proxies.size() << " configured proxy address(es)"
			          << std::endl;
			}

		if (!public_url.empty()) {
			if (public_url.rfind("http://", 0) != 0
			    && public_url.rfind("https://", 0) != 0) {
				std::cerr << "Error: public_url must start with http:// or "
				             "https://\n";
				return 1;
				}
			while (!public_url.empty() && public_url.back() == '/')
				public_url.pop_back();
			gaindrive_set_public_url(public_url);
			std::cout << stamp() << "Public URL: " << public_url << std::endl;
			}

		GainDrive gd(db_path, roots, upload_dir, no_scan, debug, flat_multi_disc,
		             user_db_path, transcode_cache_dir, transcode_cache_mb,
		             transcode_jobs, video_art_px, video_art_frames,
		             video_art_embedded, cast_devices, url_handlers,
		             url_fetch_timeout, scan_jobs);
		// Non-zero on a failed bind, so a supervisor restarts rather than
		// recording a clean shutdown for a server that never served anything.
		return gd.listen(host, port) ? 0 : 1;
		}
	catch (const std::exception& e) {
		std::cerr << "Error: " << e.what() << "\n";
		return 1;
		}
	}

#include "gaindrive.hh"
#include "stamp.hh"
#include "batchtools.hh"

#include <algorithm>
#include <filesystem>
#include <iostream>
#include <thread>

#include <netdb.h>
#include <sys/socket.h>

// ---- GainDrive --------------------------------------------------------

GainDrive::GainDrive(const std::string& db_path,
                     const std::vector<MediaStore::Root>& roots,
                     const std::string& upload_dir,
                     bool no_scan,
                     bool debug,
                     bool flat_multi_disc,
                     const std::string& user_db_path,
                     const std::string& transcode_cache_dir,
                     int transcode_cache_mb,
                     int transcode_jobs,
                     int video_art_px,
                     bool video_art_frames,
                     bool video_art_embedded,
                     const std::vector<CastManager::CastDevice>& cast_devices,
                     const std::optional<std::vector<UrlHandler>>& url_handlers,
                     int url_fetch_timeout_s,
                     int scan_jobs)
	: debug_(debug), flat_multi_disc_(flat_multi_disc), upload_dir_(upload_dir),
	  store_(db_path, roots, user_db_path, video_art_px, video_art_frames,
	         video_art_embedded, scan_jobs),
	  transcode_cache_(
	      transcode_cache_dir.empty()
	          ? std::filesystem::path(db_path).parent_path() / "transcodes"
	          : std::filesystem::path(transcode_cache_dir),
	      static_cast<int64_t>(transcode_cache_mb) * 1024 * 1024,
	      transcode_jobs > 0 ? transcode_jobs
	          : std::max(2u, std::thread::hardware_concurrency() / 2)),
	  cover_cache_(store_),
	  url_fetcher_(url_handlers, url_fetch_timeout_s),
	  watcher_(store_)
	{
	cast_manager_.set_manual_devices(cast_devices);

	namespace fs = std::filesystem;
	// A cache inside any root would be rescanned and indexed, and the
	// transcodes would then appear in the library as tracks of their own.
	if (store_.path_is_within_root(
	        fs::absolute(transcode_cache_dir.empty()
	            ? fs::path(db_path).parent_path() / "transcodes"
	            : fs::path(transcode_cache_dir)).string())) {
		std::cerr << stamp()
		          << "Error: the transcode cache must not live inside "
		          << "a library root." << std::endl;
		std::exit(1);
		}
	if (!fs::exists(upload_dir_))
		fs::create_directories(upload_dir_);
	// Personal uploads live in their own root rather than a hidden directory
	// inside a library, so nothing there can be mistaken for someone's album.
	// With no uploads root configured both stay empty and the upload endpoints
	// refuse - better than silently writing into a library root.
	if (const auto* up = store_.uploads_root()) {
		users_dir_         = up->path;
		uploads_root_name_ = up->name;
		fs::create_directories(users_dir_);
		}
	else
		std::cout << stamp() << "No uploads root configured; personal uploads "
		          << "are disabled." << std::endl;

	routes_web();
	routes_system();
	routes_browse();
	routes_playlist();
	routes_stream();
	routes_cast();
	routes_edit();
	routes_fetch();
	// Last, and it has to be: it registers the `/rest/:endpoint`
	// wildcard, which httplib would otherwise match before any of the
	// real endpoints registered after it.
	routes_fallback();

	if (!store_.has_users())
		std::cout << stamp()
		          << "WARNING: no users in database. "
		             "Create one with --add-user <name> --password <pass>."
		          << std::endl;

	// Background work (library scan, Cast discovery, folder watching) is NOT
	// started here - listen() starts it once the port is actually held.
	// Starting it in the constructor meant a server that could not bind still
	// spent minutes scanning, and could not exit promptly either: the detached
	// scan holds db_mutex_, so the watcher's join in the destructor blocks
	// behind it.  Nothing should run until we know we can serve.
	no_scan_ = no_scan;
	}

GainDrive::~GainDrive()
	{
	// The fetch worker first: it is the one that can be inside scan_dirs(), and
	// joining it here - in the destructor body - is what guarantees it is not
	// still holding store_ when the members are destroyed.
	fetch_stop_ = true;
	fetch_cv_.notify_all();
	// A fetch is allowed to run for hours, and the worker checks the stop flag
	// only between jobs - so without killing the child, this join is the
	// shutdown. The half-written batch is removed by the worker's own failure
	// path on the way out.
	url_fetcher_.cancel_any();
	if (fetch_thread_.joinable()) fetch_thread_.join();

	lookup_stop_ = true;
	lookup_cv_.notify_all();
	if (lookup_thread_.joinable()) lookup_thread_.join();
	}

// A statically linked binary resolves names through whatever its libc provides:
// musl does it itself and works, a static glibc cannot do it at all. Either way
// the failure is invisible - the library serves fine and only the MusicBrainz,
// Wikidata, Discogs and cover-art-by-URL fetches quietly come back empty. One
// lookup at startup turns that into a line in the log.
static void check_dns_background()
	{
	std::thread([]{
		try {
			addrinfo hints{};
			hints.ai_family   = AF_UNSPEC;
			hints.ai_socktype = SOCK_STREAM;
			addrinfo* res = nullptr;
			int rc = getaddrinfo("www.gaindrive.org", nullptr, &hints, &res);
			if (rc == 0) {
				freeaddrinfo(res);
				return;
				}
			std::cout << stamp() << "Warning: cannot resolve www.gaindrive.org ("
			          << gai_strerror(rc) << "). Artist info, lyrics and cover "
			             "art fetched from the web will not work. Harmless if "
			             "this server is deliberately offline." << std::endl;
			}
		catch (const std::exception& e) {
			std::cout << stamp() << "DNS check failed: " << e.what() << std::endl;
			}
		catch (...) {
			std::cout << stamp() << "DNS check failed: unknown exception"
			          << std::endl;
			}
		}).detach();
	}

bool GainDrive::listen(const std::string& host, int port)
	{
	// Bind first and announce afterwards.  The message used to print before
	// the attempt, so a start that never acquired the port still looked like
	// a healthy one in the log.
	if (!server_.bind_to_port(host, port)) {
		std::cerr << stamp() << "Error: cannot bind to " << host << ":" << port
		          << " - is another gaindrive already running?" << std::endl;
		return false;
		}
	std::cout << stamp() << "Listening on " << host << ":" << port << std::endl;

	// Only now that the port is ours: a server that cannot serve should do no
	// work at all, and should be able to exit at once.
	// An exception escaping a thread's top-level function calls
	// std::terminate, so an unguarded scan turns a momentary database lock
	// into a dead server.  A stale library until the next scan is a far better
	// outcome, and the folder watcher will retry on the next filesystem event.
	// Before the scan and before the watcher, so neither can reach a batch the
	// previous run left half-written.
	if (!users_dir_.empty()) sweep_held_batches(users_dir_);

	if (!no_scan_)
		std::thread([this]{
			try { store_.scan(); }
			catch (const std::exception& e) {
				std::cout << stamp() << "Scan aborted: " << e.what()
				          << std::endl;
				}
			catch (...) {
				std::cout << stamp() << "Scan aborted: unknown exception"
				          << std::endl;
				}
			// Outside the try, and on every path: an aborted scan still added
			// whatever it got through, and those artists want portraits as
			// much as any others.  Safe before the worker below has started -
			// it sets a flag the worker's first wait tests, so an early wake
			// costs one extra seed rather than being lost.
			lookup_wake();
			}).detach();
	cast_manager_.discover_background();
	probe_cast_devices_background();
	check_dns_background();
	watcher_.start();
	// Joined in the destructor rather than detached: it holds references to
	// store_ and cover_cache_, so it must not outlive them.
	lookup_thread_ = std::thread([this]{ lookup_worker(); });
	// Same reasoning, and more sharply: this one calls scan_dirs().
	if (url_fetcher_.configured())
		fetch_thread_ = std::thread([this]{ fetch_worker(); });

	if (!server_.listen_after_bind()) {
		std::cerr << stamp() << "Error: server loop exited unexpectedly."
		          << std::endl;
		return false;
		}
	return true;
	}

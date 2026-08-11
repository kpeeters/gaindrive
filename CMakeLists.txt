cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_DEFAULT_CMP0148 NEW)
project(gaindrive VERSION 0.1 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reported as serverVersion and baked into the outgoing User-Agent. Overridden
# from the git tag by the release build, so a downloaded binary can say which
# one it is.
set(GAINDRIVE_VERSION "${PROJECT_VERSION}" CACHE STRING
	"Version string reported by the server")

# There was no default, so a plain `cmake -B build` compiled with no -O flag at
# all. Release is what an unqualified build should mean; pass -DCMAKE_BUILD_TYPE
# explicitly for anything else.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
	set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Optional mostly-static link for easy distribution: OpenSSL, zlib, libstdc++
# and libgcc are linked statically while glibc stays dynamic (a fully static
# glibc would warn about getaddrinfo/NSS and break NSS name resolution). The
# default OFF build is byte-for-byte the same dynamic link as before. Use a
# separate build dir, e.g. cmake -B build-static -DGAINDRIVE_STATIC=ON.
option(GAINDRIVE_STATIC
	"Link gaindrive mostly-statically (static OpenSSL/zlib/libstdc++/libgcc; glibc stays dynamic) for easier distribution"
	OFF)

# Goes further and links libc too, which needs musl: the NSS problem above is
# glibc-specific, whereas musl's resolver lives inside libc and works fine in a
# fully static binary. This is what the release artifacts are built with; see
# cmake/static_build.cmake for the Alpine container that provides the toolchain.
option(GAINDRIVE_STATIC_LIBC
	"Also link libc statically. Requires a musl toolchain — a fully static glibc breaks NSS, so getaddrinfo() stops resolving and the MusicBrainz/Wikidata/Discogs lookups fail."
	OFF)

# Implied, not independent: a static libc cannot be combined with a dynamic
# OpenSSL/zlib. A plain set() is enough — it shadows the cache entry for the
# rest of this file, which is all the GAINDRIVE_STATIC blocks below need.
if(GAINDRIVE_STATIC_LIBC)
	set(GAINDRIVE_STATIC ON)
endif()

# Both static options are GNU-ld only: -static, -static-libstdc++, --gc-sections
# and -z stack-size are all rejected by ld64. Fail here rather than at link time
# with an error that reads like a toolchain problem. macOS distribution goes
# through the Homebrew formula instead, which wants a normal dynamic link
# against brew's own openssl@3 — see README.
if(GAINDRIVE_STATIC AND APPLE)
	message(FATAL_ERROR
		"GAINDRIVE_STATIC/GAINDRIVE_STATIC_LIBC are Linux-only: Apple ships no "
		"static libSystem and ld64 rejects these flags. On macOS build normally "
		"and distribute via the Homebrew tap.")
endif()

# ---- Dependencies ----------------------------------------------------------
#
# cpp-httplib and mdns are vendored in third_party/ and are not dependencies at
# all; see third_party/README.md for why neither can be taken from a package.
# The remaining seven come from system packages by default. Building them from
# source is the exception, not the rule:
option(GAINDRIVE_BUNDLED_DEPS
	"Build the remaining dependencies from source instead of requiring system packages. Needed only for the fully static release build, since Alpine ships no static taglib/tinyxml2/reproc and no SQLiteCpp at all."
	OFF)

# Minimum versions. These are not decoration — they are what keeps a system-first
# default safe on an older distribution. Ubuntu 24.04 ships taglib 1.13.1, and
# the code uses the taglib 2 API, so without a floor find_package() would accept
# it and the failure would surface as a compile error in mediastore.cc instead of
# an actionable message here.
set(GD_MIN_TagLib        2.0)
set(GD_MIN_SQLiteCpp     3.3)
set(GD_MIN_reproc++      14.2)
set(GD_MIN_LibArchive    3.6)
set(GD_MIN_tinyxml2      9.0)
set(GD_MIN_cxxopts       3.0)
set(GD_MIN_nlohmann_json 3.11)

# Deliberately outside both branches below, and ahead of them. FindOpenSSL and
# FindZLIB pick shared-vs-static for their imported targets the first time they
# run, and that is either the find_package(OpenSSL) at the end of this section or
# one that libarchive/taglib run internally during MakeAvailable — whichever
# comes first. Nesting this inside the bundled branch would leave
# -DGAINDRIVE_STATIC=ON with a system dependency set silently linking shared
# OpenSSL, which is exactly the sort of "nothing failed, the result is just
# wrong" outcome this file works to avoid.
if(GAINDRIVE_STATIC)
	set(OPENSSL_USE_STATIC_LIBS ON)
	set(ZLIB_USE_STATIC_LIBS    ON)   # FindZLIB: needs CMake >= 3.24

	# Make every find_library/find_package resolve to a .a archive so the
	# remaining codec backends link statically (or auto-disable when no static
	# lib is installed) instead of pulling .so files into the binary.
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

	# Directory-scoped rather than target-scoped so the dependencies get it too:
	# taglib, libarchive, sqlite3 and static OpenSSL are the bulk of the binary,
	# and --gc-sections can only drop a function that was given its own section.
	# One genex per flag: a `;` inside an unquoted argument is a list separator,
	# so a combined $<...:-a;-b> would be split into two broken fragments.
	add_compile_options($<$<CONFIG:Release,MinSizeRel>:-ffunction-sections>)
	add_compile_options($<$<CONFIG:Release,MinSizeRel>:-fdata-sections>)
endif()

if(GAINDRIVE_BUNDLED_DEPS)

	include(FetchContent)

	# Nothing may leak in from the machine: the point of this mode is a build
	# whose dependency versions are a property of this file.
	set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)

	# v3.3.1 rather than v3.2.1 only because the older tag declares
	# cmake_minimum_required(VERSION 3.1...3.19), and CMake 4 rejects a <min>
	# below 3.5 even in the range form.
	FetchContent_Declare(
		cxxopts
		GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
		GIT_TAG        v3.3.1
	)

	# Likewise: 3.11.3 declares 3.1...3.14, 3.12.0 declares 3.5...4.0.
	FetchContent_Declare(
		nlohmann_json
		GIT_REPOSITORY https://github.com/nlohmann/json.git
		GIT_TAG        v3.12.0
	)

	# SQLiteCpp bundles and builds sqlite3 from source (SQLITECPP_INTERNAL_SQLITE
	# defaults ON), which is what we want for a single self-contained binary.
	# Disable cpplint so it never searches for Python.
	set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		SQLiteCpp
		GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
		GIT_TAG        3.3.2
	)

	# Build taglib as a static library; disable things we don't need.
	set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
	set(TAGLIB_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
	set(TAGLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		taglib
		GIT_REPOSITORY https://github.com/taglib/taglib.git
		GIT_TAG        v2.0.2
	)

	FetchContent_Declare(
		tinyxml2
		GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
		GIT_TAG        10.0.0
	)

	# reproc++ for spawning ffmpeg child processes.
	set(REPROC++ ON CACHE BOOL "" FORCE)
	FetchContent_Declare(
		reproc
		GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
		GIT_TAG        v14.2.5
	)

	# libarchive for extracting user-uploaded zip/tar archives. v3.8.9 rather
	# than v3.7.4, whose CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12) CMake 4 refuses;
	# 3.8.x declares 3.17.
	set(ENABLE_TEST    OFF CACHE BOOL "" FORCE)
	set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		libarchive
		GIT_REPOSITORY https://github.com/libarchive/libarchive.git
		GIT_TAG        v3.8.9
	)

	# Trim optional backends gaindrive never uses. This only bites in bundled
	# mode — with a system libarchive the distribution has already chosen its
	# backends — and it matters most for the static link this mode exists to
	# serve. The worst offender is libarchive's xar reader: its libxml2 pulls in
	# ICU (libicuuc/libicudata), and because ICU is C++ it drags
	# libstdc++/libgcc_s back into an otherwise static link even with
	# -static-libstdc++. zlib/bz2/lzma/zstd stay enabled so zip and the common
	# compressed-tar uploads still extract.
	set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)   # xar -> libxml2 -> ICU
	set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
	set(ENABLE_ICONV   OFF CACHE BOOL "" FORCE)
	set(ENABLE_LZO     OFF CACHE BOOL "" FORCE)
	set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
	set(ENABLE_LIBB2   OFF CACHE BOOL "" FORCE)

	FetchContent_MakeAvailable(
		cxxopts nlohmann_json SQLiteCpp taglib tinyxml2 reproc libarchive)

	# Reproduce, for the in-tree build, the include layout taglib's INSTALLED
	# form provides — its exported target offers both <prefix>/include and
	# <prefix>/include/taglib, and its PUBLIC_HEADER install flattens
	# toolkit/tpropertymap.h to include/taglib/tpropertymap.h. The source tree
	# keeps the toolkit headers in a subdirectory instead, so without these three
	# entries the same #include lines resolve in system mode and fail here.
	target_include_directories(tag PUBLIC
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}>
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>
	)

else()

	# System packages. Every miss is collected so one configure run reports
	# everything that needs installing, rather than making the user rediscover
	# the list one failure at a time.
	set(_gd_missing "")
	set(_gd_apt     "")
	set(_gd_brew    "")

	# find_package name, apt package, brew formula. Getting a find_package name
	# wrong reports "not found" for something that is in fact installed, so:
	# TagLib is spelled capitalised even though the file is taglib-config.cmake
	# (CMake also searches the lowercase-dashed form, and taglib's own config
	# calls check_required_components("TagLib"), so the capitalised spelling is
	# the one its internals agree with). LibArchive is CMake's bundled find
	# module rather than an upstream config package.
	foreach(dep
			"TagLib|libtag-dev|taglib"
			"tinyxml2|libtinyxml2-dev|tinyxml2"
			"cxxopts|libcxxopts-dev|cxxopts"
			"nlohmann_json|nlohmann-json3-dev|nlohmann-json"
			"SQLiteCpp|libsqlitecpp-dev|sqlitecpp"
			"reproc++|libreproc-dev|reproc"
			"LibArchive|libarchive-dev|libarchive")
		string(REPLACE "|" ";" _parts "${dep}")
		list(GET _parts 0 _name)
		list(GET _parts 1 _apt)
		list(GET _parts 2 _brew)
		find_package(${_name} ${GD_MIN_${_name}} QUIET)
		if(NOT ${_name}_FOUND)
			list(APPEND _gd_missing "${_name} >= ${GD_MIN_${_name}}")
			list(APPEND _gd_apt  "${_apt}")
			list(APPEND _gd_brew "${_brew}")
		endif()
	endforeach()

	if(_gd_missing)
		list(JOIN _gd_missing "\n    " _list)
		if(APPLE)
			list(JOIN _gd_brew " " _cmd)
			set(_install "brew install ${_cmd} openssl@3")
		elseif(EXISTS "/etc/debian_version")
			list(JOIN _gd_apt " " _cmd)
			set(_install "sudo apt install ${_cmd} libssl-dev")
		else()
			list(JOIN _gd_apt " " _cmd)
			# One argument, not two: a second would make this a list and render
			# with a stray semicolon in the middle of the message.
			set(_install "install your distribution's development packages for the above.\n    Debian and Ubuntu name them: ${_cmd} libssl-dev")
		endif()
		message(FATAL_ERROR
			"Missing system dependencies:\n"
			"    ${_list}\n\n"
			"  Install them with:\n"
			"    ${_install}\n\n"
			"  Or build them from source instead, which needs network access:\n"
			"    cmake -B build -DGAINDRIVE_BUNDLED_DEPS=ON\n")
	endif()

endif()

find_package(OpenSSL REQUIRED)

# httplib.h spawns threads and is no longer a CMake target that could carry the
# requirement itself. On Linux this is a link error rather than a silent one, but
# it is the sort of error that reads like a toolchain fault, so be explicit.
find_package(Threads REQUIRED)

# Resolve each dependency to whichever target name it actually exposes, and use
# the resolved list to link. The two modes do not agree on the spelling, and not
# consistently either: taglib and libarchive are namespaced when installed but
# plain in-tree, while SQLiteCpp and reproc++ set no NAMESPACE on their
# install(EXPORT ...) at all and so are unnamespaced in both. Taking the first
# candidate that exists beats hardcoding a spelling per mode, and absorbs a
# distribution — or a future release — adding a namespace.
#
# SQLiteCpp's config already does find_dependency(SQLite3) when it was not built
# against its bundled amalgamation, so nothing here needs to chase sqlite3.
set(GD_DEP_TARGETS "")
foreach(candidates
		"cxxopts::cxxopts|cxxopts"
		"nlohmann_json::nlohmann_json|nlohmann_json"
		"SQLiteCpp::SQLiteCpp|SQLiteCpp"
		"TagLib::tag|tag"
		"tinyxml2::tinyxml2|tinyxml2"
		"reproc++::reproc++|reproc++"
		"LibArchive::LibArchive|archive_static|archive")
	string(REPLACE "|" ";" _cands "${candidates}")
	set(_hit "")
	foreach(_c IN LISTS _cands)
		if(TARGET ${_c})
			set(_hit ${_c})
			break()
		endif()
	endforeach()
	if(NOT _hit)
		list(JOIN _cands ", " _tried)
		message(FATAL_ERROR
			"A dependency was found but exposes none of the CMake targets "
			"gaindrive knows how to link: ${_tried}.\n"
			"  Its installed config package probably differs from upstream's. "
			"Build from source instead with -DGAINDRIVE_BUNDLED_DEPS=ON, and "
			"please report the target name your distribution uses.")
	endif()
	list(APPEND GD_DEP_TARGETS ${_hit})
endforeach()

# Embed web/ client files into the binary at build time.
set(EMBEDDED_WEB_CC ${CMAKE_CURRENT_BINARY_DIR}/embedded_web.cc)
add_custom_command(
	OUTPUT  ${EMBEDDED_WEB_CC}
	COMMAND ${CMAKE_COMMAND}
		-DOUTPUT=${EMBEDDED_WEB_CC}
		-DWEB_DIR=${CMAKE_CURRENT_SOURCE_DIR}/web
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/web/index.html
		${CMAKE_CURRENT_SOURCE_DIR}/web/style.css
		${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
		${CMAKE_CURRENT_SOURCE_DIR}/web/favicon.svg
		${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake
	COMMENT "Embedding web client files"
)

# ---- Install locations -----------------------------------------------------

# Where the config file and the database live. These are compiled into the
# binary as the --config/--db defaults AND used by install(), so the two can
# never disagree. The defaults are the FHS locations gaindrive has always used,
# so a distribution package sees no change; a prefix-based package manager
# (Homebrew) points them inside its own prefix, which it must, because its build
# sandbox refuses any write outside the prefix.
set(GAINDRIVE_SYSCONFDIR    "/etc"     CACHE PATH "Directory holding gaindrive.conf")
set(GAINDRIVE_LOCALSTATEDIR "/var/lib" CACHE PATH "Parent directory of gaindrive's state directory")

add_executable(gaindrive
	src/main.cc
	src/gaindrive.cc
	src/mediastore.cc
	src/streamer.cc
	src/transcodecache.cc
	src/stamp.cc
	src/castmanager.cc
	src/folderwatcher.cc
	${EMBEDDED_WEB_CC}
)

target_compile_definitions(gaindrive PRIVATE
	CPPHTTPLIB_OPENSSL_SUPPORT
	GAINDRIVE_VERSION="${GAINDRIVE_VERSION}"
	GAINDRIVE_DEFAULT_CONFIG="${GAINDRIVE_SYSCONFDIR}/gaindrive.conf"
	GAINDRIVE_DEFAULT_DB="${GAINDRIVE_LOCALSTATEDIR}/gaindrive/gaindrive.db")

# So #include "embedded_web.hh" resolves from both src/ and the build dir, and
# <httplib.h>/<mdns.h> from the vendored copies in third_party/.
target_include_directories(gaindrive PRIVATE
	src
	third_party
	${CMAKE_CURRENT_BINARY_DIR})

# libarchive's in-tree layout puts the headers in a libarchive/ subdirectory and
# exports no usage requirement for the un-installed target. A system libarchive
# needs none of this: <archive.h> resolves from the standard include path and
# LibArchive::LibArchive carries its own.
if(GAINDRIVE_BUNDLED_DEPS)
	target_include_directories(gaindrive PRIVATE
		${libarchive_SOURCE_DIR}/libarchive)
endif()

target_link_libraries(gaindrive PRIVATE
	${GD_DEP_TARGETS}
	OpenSSL::SSL
	OpenSSL::Crypto
	Threads::Threads
)

# FolderWatcher's macOS branch uses FSEvents, which lives in CoreServices
# (CarbonCore). The framework also brings in CoreFoundation, which that branch
# needs for the CFArray of watched roots.
if(APPLE)
	target_link_libraries(gaindrive PRIVATE "-framework CoreServices")
endif()

# OpenSSL/zlib are made static by the USE_STATIC_LIBS vars set before
# MakeAvailable; the C++ runtime is made static here. glibc and the satellites
# static OpenSSL pulls in (libdl/libpthread/libm) stay dynamic, by design.
if(GAINDRIVE_STATIC)
	target_link_options(gaindrive PRIVATE -static-libstdc++ -static-libgcc)

	# Pairs with -ffunction-sections above. -s is the strip, done at link time
	# so no separate strip step is needed for a release artifact.
	target_link_options(gaindrive PRIVATE
		$<$<CONFIG:Release,MinSizeRel>:-Wl,--gc-sections>
		$<$<CONFIG:Release,MinSizeRel>:-s>)

	if(GAINDRIVE_STATIC_LIBC)
		target_link_options(gaindrive PRIVATE -static)

		# musl gives every thread a 128 KB stack by default, against glibc's
		# 8 MB, and takes that default from PT_GNU_STACK. Three 64 KB stack
		# buffers (streamer.cc, gaindrive.cc) live on httplib worker threads,
		# which leaves almost no margin once OpenSSL and iostream frames sit on
		# top. glibc ignores this header when sizing thread stacks, so the flag
		# costs nothing in the non-musl case.
		target_link_options(gaindrive PRIVATE -Wl,-z,stack-size=1048576)

		# Linking -static against glibc succeeds and produces a binary whose
		# name resolution is dead, which only shows up as artist info silently
		# returning nothing. Say so at configure time.
		execute_process(COMMAND ${CMAKE_CXX_COMPILER} -dumpmachine
			OUTPUT_VARIABLE _triple OUTPUT_STRIP_TRAILING_WHITESPACE)
		if(NOT _triple MATCHES "musl")
			message(WARNING "GAINDRIVE_STATIC_LIBC with a non-musl toolchain "
			                "(${_triple}): hostname lookups will fail at runtime.")
		endif()
	endif()
endif()

# Not part of ALL: it shells out to a container. The script is the source of
# truth for the release build and also runs standalone (cmake -P), which is what
# CI uses so it need not configure this tree just to invoke docker.
add_custom_target(static
	COMMAND ${CMAKE_COMMAND} -DVERSION=${GAINDRIVE_VERSION}
	        -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/static_build.cmake
	USES_TERMINAL
	COMMENT "Building fully static release binary in Alpine")

# ---- Install ---------------------------------------------------------------

include(GNUInstallDirs)

install(TARGETS gaindrive RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# systemd integration: unit file + declarative user creation. Defaults OFF off
# Linux — the block below falls back to /lib/systemd/system when pkg-config finds
# no systemd, so leaving it ON would have a macOS install write units into
# directories that do not exist. macOS gets a launchd job from the Homebrew
# formula's `service` block instead.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
	set(_gd_systemd_default ON)
else()
	set(_gd_systemd_default OFF)
endif()
option(GAINDRIVE_INSTALL_SYSTEMD "Install systemd unit and sysusers.d file"
       ${_gd_systemd_default})

if(GAINDRIVE_INSTALL_SYSTEMD)
	# Resolve systemd's real directories from its pkg-config file. These live
	# OUTSIDE CMAKE_INSTALL_PREFIX on purpose: systemd does not search
	# /usr/local/lib/systemd, so units installed under a /usr/local prefix would
	# never be found. The cache variables let packagers override (e.g. for
	# DESTDIR staging).
	find_package(PkgConfig)
	if(PkgConfig_FOUND)
		pkg_get_variable(_sd_unit_dir    systemd systemdsystemunitdir)
		pkg_get_variable(_sd_sysusers_dir systemd sysusersdir)
	endif()
	if(NOT _sd_unit_dir)
		set(_sd_unit_dir "/lib/systemd/system")
	endif()
	if(NOT _sd_sysusers_dir)
		set(_sd_sysusers_dir "/usr/lib/sysusers.d")
	endif()
	set(SYSTEMD_UNIT_DIR "${_sd_unit_dir}" CACHE PATH "systemd system unit directory")
	set(SYSTEMD_SYSUSERS_DIR "${_sd_sysusers_dir}" CACHE PATH "systemd sysusers.d directory")

	# Substitute the installed binary path into ExecStart.
	configure_file(dist/gaindrive.service.in
	               ${CMAKE_CURRENT_BINARY_DIR}/gaindrive.service @ONLY)

	install(FILES ${CMAKE_CURRENT_BINARY_DIR}/gaindrive.service
	        DESTINATION ${SYSTEMD_UNIT_DIR})
	install(FILES dist/gaindrive.sysusers
	        DESTINATION ${SYSTEMD_SYSUSERS_DIR} RENAME gaindrive.conf)
endif()

# Example config installs next to the config the binary actually reads, which is
# GAINDRIVE_SYSCONFDIR and not CMAKE_INSTALL_SYSCONFDIR: on Linux that stays
# /etc regardless of prefix, exactly as before, while a Homebrew build redirects
# both the default and this file into its own prefix. Installed with an .example
# suffix so it never clobbers an admin's live config.
install(FILES dist/gaindrive.conf.example DESTINATION ${GAINDRIVE_SYSCONFDIR})

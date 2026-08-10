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

include(FetchContent)

FetchContent_Declare(
	httplib
	GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
	GIT_TAG        v0.18.7
)

FetchContent_Declare(
	cxxopts
	GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
	GIT_TAG        v3.2.1
)

FetchContent_Declare(
	nlohmann_json
	GIT_REPOSITORY https://github.com/nlohmann/json.git
	GIT_TAG        v3.11.3
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
set(BUILD_SHARED_LIBS   OFF CACHE BOOL "" FORCE)
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

FetchContent_Declare(
	mdns
	GIT_REPOSITORY https://github.com/mjansson/mdns.git
	GIT_TAG        1.4.3
)

# libarchive for extracting user-uploaded zip/tar archives.
set(ENABLE_TEST    OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
	libarchive
	GIT_REPOSITORY https://github.com/libarchive/libarchive.git
	GIT_TAG        v3.7.4
)

# Must be set before MakeAvailable: httplib (and libarchive/taglib) run their
# own find_package(OpenSSL)/find_package(ZLIB) during configuration, and
# FindOpenSSL/FindZLIB pick shared-vs-static for the imported targets the first
# time they run. Setting these here makes OpenSSL::SSL/Crypto and ZLIB::ZLIB
# resolve to the .a archives for every consumer, including httplib's transitive
# link.
if(GAINDRIVE_STATIC)
	set(OPENSSL_USE_STATIC_LIBS ON)
	set(ZLIB_USE_STATIC_LIBS    ON)   # FindZLIB: needs CMake >= 3.24

	# Make every find_library/find_package resolve to a .a archive so the
	# remaining codec backends link statically (or auto-disable when no static
	# lib is installed) instead of pulling .so files into the binary. Requires
	# the static dev libs to be present (on Debian/Ubuntu the -dev packages
	# ship the .a, e.g. zlib1g-dev, liblzma-dev, libbz2-dev, libzstd-dev).
	set(CMAKE_FIND_LIBRARY_SUFFIXES ".a")

	# Trim optional backends gaindrive never uses but which otherwise drag in
	# heavy shared libraries. The worst offender is libarchive's xar reader:
	# its libxml2 pulls in ICU (libicuuc/libicudata), and because ICU is C++ it
	# drags libstdc++/libgcc_s back in even with -static-libstdc++. httplib's
	# Brotli/Zstd HTTP content-encoding is likewise unused. zlib/bz2/lzma/zstd
	# stay enabled so zip and the common compressed-tar uploads still extract.
	set(HTTPLIB_USE_BROTLI_IF_AVAILABLE OFF CACHE BOOL "" FORCE)
	set(HTTPLIB_USE_ZSTD_IF_AVAILABLE   OFF CACHE BOOL "" FORCE)
	set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)   # xar -> libxml2 -> ICU
	set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
	set(ENABLE_ICONV   OFF CACHE BOOL "" FORCE)
	set(ENABLE_LZO     OFF CACHE BOOL "" FORCE)
	set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
	set(ENABLE_LIBB2   OFF CACHE BOOL "" FORCE)

	# Directory-scoped rather than target-scoped so the dependencies get it too:
	# taglib, libarchive, sqlite3 and static OpenSSL are the bulk of the binary,
	# and --gc-sections can only drop a function that was given its own section.
	# One genex per flag: a `;` inside an unquoted argument is a list separator,
	# so a combined $<...:-a;-b> would be split into two broken fragments.
	add_compile_options($<$<CONFIG:Release,MinSizeRel>:-ffunction-sections>)
	add_compile_options($<$<CONFIG:Release,MinSizeRel>:-fdata-sections>)
endif()

FetchContent_MakeAvailable(httplib cxxopts nlohmann_json SQLiteCpp taglib tinyxml2 reproc mdns libarchive)

find_package(OpenSSL REQUIRED)

target_include_directories(tag PUBLIC
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}>
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>
)

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
	GAINDRIVE_VERSION="${GAINDRIVE_VERSION}")

# So #include "embedded_web.hh" resolves from both src/ and the build dir.
target_include_directories(gaindrive PRIVATE src ${CMAKE_CURRENT_BINARY_DIR})

# mdns.h is header-only; add its source directory to the include path.
target_include_directories(gaindrive PRIVATE ${mdns_SOURCE_DIR})

# libarchive: headers live in the source tree's libarchive/ subdirectory.
target_include_directories(gaindrive PRIVATE ${libarchive_SOURCE_DIR}/libarchive)

target_link_libraries(gaindrive PRIVATE
	httplib::httplib
	cxxopts::cxxopts
	nlohmann_json::nlohmann_json
	SQLiteCpp
	tag
	tinyxml2::tinyxml2
	OpenSSL::SSL
	OpenSSL::Crypto
	reproc++
	archive_static
)

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

# systemd integration: unit file + declarative user creation. Gated so non-Linux
# builds and minimal setups can opt out.
option(GAINDRIVE_INSTALL_SYSTEMD "Install systemd unit and sysusers.d file" ON)

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

# Example config installs to /etc (not CMAKE_INSTALL_SYSCONFDIR): gaindrive
# always reads /etc/gaindrive.conf regardless of prefix. Installed with an
# .example suffix so it never clobbers an admin's live config.
install(FILES dist/gaindrive.conf.example DESTINATION /etc)

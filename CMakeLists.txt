cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_DEFAULT_CMP0148 NEW)
# The root VERSION file is the only place a version is written; the Android and
# iOS builds read the same file. Only the `version` line is of interest here —
# `build` numbers a store upload and means nothing to the server.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION _gd_version REGEX "^version=")
string(REGEX REPLACE "^version=" "" _gd_version "${_gd_version}")
# file(STRINGS) reads the file but creates no dependency on it, so nothing about
# editing VERSION makes the build re-run cmake — an existing build/ went on
# compiling the version it was last configured with, and `cmake --build` said
# everything was up to date. This is the line that makes a bump take effect.
set_property(DIRECTORY APPEND PROPERTY
	CMAKE_CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/VERSION)
# Without this, an empty result would leave `project(gaindrive VERSION LANGUAGES
# CXX)` and CMake would report a problem with LANGUAGES.
if(NOT _gd_version MATCHES "^[0-9]+(\\.[0-9]+)*$")
	message(FATAL_ERROR "VERSION: no usable `version=' line (got '${_gd_version}')")
endif()
project(gaindrive VERSION ${_gd_version} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Reported as serverVersion and baked into the outgoing User-Agent.
#
# -DGAINDRIVE_VERSION= still wins, which is how a release binary is stamped with
# its git tag (.github/workflows/build.yml, via cmake/static_build.cmake) and how
# the Homebrew formula stamps its own — the `if` sees the cache entry that -D
# creates.
#
# **The entry is then removed again, and that is the whole point.** An override
# is a property of one configure, not of the build directory, but -D has no way
# to say so: the value it writes is indistinguishable on the next configure from
# one someone meant. Dropping `CACHE STRING` from the set() below was an earlier
# attempt at this and does not work — removing it from the source does not
# remove an entry an earlier configure already wrote, and `if(NOT
# GAINDRIVE_VERSION)` goes on reading it. That is not hypothetical: this tree
# carried GAINDRIVE_VERSION:STRING=0.1 for months while VERSION said 1.0.1, so
# --version, the User-Agents and the web client's About box all reported 0.1,
# and the only sign of it was the STATUS line below claiming an override nobody
# had asked for. Hence WARNING rather than STATUS: a tag is checked against
# VERSION in CI and Homebrew stamps the version it downloaded, so the two
# disagreeing is a forgotten bump or a stale cache, never a plan.
if(GAINDRIVE_VERSION)
	set(_gd_stamped "${GAINDRIVE_VERSION}")
	unset(GAINDRIVE_VERSION CACHE)
	set(GAINDRIVE_VERSION "${_gd_stamped}")
	if(NOT GAINDRIVE_VERSION STREQUAL PROJECT_VERSION)
		message(WARNING "gaindrive version: building as ${GAINDRIVE_VERSION}, "
			"but VERSION says ${PROJECT_VERSION}")
	endif()
else()
	set(GAINDRIVE_VERSION "${PROJECT_VERSION}")
endif()

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
# The remaining seven come from system packages by default. Building from source
# is per-dependency, not all-or-nothing: a distribution that is merely behind on
# one library should not force every other one to be rebuilt too. Ubuntu 24.04 is
# the case that shaped this — it has five of the seven, but TagLib only at 1.13
# and no reproc++ package at all, so
#
#     cmake -B build -DGAINDRIVE_BUNDLED_DEPS="TagLib;reproc++"
#
# is the right answer there, not rebuilding the lot.
set(GAINDRIVE_BUNDLED_DEPS "" CACHE STRING
	"Dependencies to build from source even when the system provides them: a list of names from GD_ALL_DEPS, or ALL. Empty (the default) takes whatever the system has and builds only the rest.")

# Anything not found is built from source rather than refused. A distribution
# that is merely behind on one library — Ubuntu 24.04 has no reproc package at
# all and TagLib only at 1.13 — should still give a working `cmake -B build`.
#
# Turning this OFF is how the stricter guarantee is asserted by the people who
# actually need it. A distribution packager must not ship convenience copies and
# must not download during a build, so they set it and get an error listing
# exactly what belongs in Build-Depends. Refusing by default would not have
# bought them anything they cannot assert themselves, while blocking everyone
# else on a distro they do not control.
option(GAINDRIVE_ALLOW_FETCH
	"Build a dependency from source when the system does not provide one new enough. Turn OFF for distribution packaging or an offline build, so a missing package is an error instead of a download."
	ON)

set(GD_ALL_DEPS TagLib tinyxml2 cxxopts nlohmann_json SQLiteCpp reproc++ LibArchive)

# Accept the boolean spellings too, so -DGAINDRIVE_BUNDLED_DEPS=ON keeps meaning
# "all of them" for anyone who already has that in a script.
string(TOUPPER "${GAINDRIVE_BUNDLED_DEPS}" _gd_req)
if(_gd_req STREQUAL "ALL" OR _gd_req STREQUAL "ON"
		OR _gd_req STREQUAL "TRUE" OR _gd_req STREQUAL "YES" OR _gd_req STREQUAL "1")
	set(GD_BUNDLED ${GD_ALL_DEPS})
elseif(_gd_req STREQUAL "" OR _gd_req STREQUAL "OFF"
		OR _gd_req STREQUAL "FALSE" OR _gd_req STREQUAL "NO" OR _gd_req STREQUAL "0")
	set(GD_BUNDLED "")
else()
	set(GD_BUNDLED ${GAINDRIVE_BUNDLED_DEPS})
	foreach(_d IN LISTS GD_BUNDLED)
		if(NOT _d IN_LIST GD_ALL_DEPS)
			list(JOIN GD_ALL_DEPS ", " _known)
			message(FATAL_ERROR
				"GAINDRIVE_BUNDLED_DEPS names an unknown dependency: ${_d}\n"
				"  Known names are: ${_known}\n"
				"  (or ALL for every one of them)")
		endif()
	endforeach()
endif()

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

# Deliberately here, and deliberately at top level.
#
# Nothing in gaindrive uses zlib directly; taglib and libarchive do. But taglib
# hands ZLIB::ZLIB to its consumers and does not guarantee anyone has defined
# it, which breaks in two different ways depending on where taglib came from:
#
#   * built from source here, taglib's own find_package("ZLIB") runs in its
#     subdirectory, and its static build then OVERWRITES tag's whole
#     INTERFACE_LINK_LIBRARIES with the bare string ZLIB::ZLIB;
#   * installed, taglib-config.cmake includes taglib-targets.cmake -- which
#     carries that same INTERFACE_LINK_LIBRARIES -- and never calls
#     find_dependency(ZLIB), so nothing defines the target at all.
#
# Either way the failure lands on OUR add_executable, as "Target gaindrive
# links to target ZLIB::ZLIB but the target was not found. Perhaps a
# find_package() call is missing" -- which names no dependency and points at a
# line that has nothing to do with it. Defining the target here satisfies both.
#
# Not REQUIRED: without zlib, a bundled taglib sets HAVE_ZLIB false and never
# names the target, so a machine that would have built happily must not be
# refused. And it must come AFTER the block above, for the reason that block
# already gives -- FindZLIB fixes shared-vs-static on its first run, so running
# it here is what stops the answer depending on which subproject asked first.
find_package(ZLIB)

set(GD_FETCH "")     # FetchContent names to make available
set(GD_SYSTEM "")    # resolved from a system package
set(_gd_missing "")  # wanted from the system, not found there
set(_gd_apt     "")
set(_gd_brew    "")

# find_package name | FetchContent name | apt package | brew formula.
#
# Getting a find_package name wrong reports "not found" for something that is in
# fact installed, so: TagLib is spelled capitalised even though the file is
# taglib-config.cmake — CMake also searches the lowercase-dashed form, and
# taglib's own config calls check_required_components("TagLib"), so the
# capitalised spelling is the one its internals agree with. LibArchive is
# CMake's bundled find module rather than an upstream config package.
#
# The apt names are for Debian 13 and newer, and the advisory below says so.
# Older releases differ: Ubuntu 24.04 calls taglib libtag1-dev (and ships 1.13
# anyway) and has no reproc package at all. An install command must never mix
# names that do and do not exist on the target release — apt aborts the entire
# transaction on one unknown name, so the user installs nothing, and the next
# configure then reports every package missing including the available ones.
foreach(dep
		"TagLib|taglib|libtag-dev|taglib"
		"tinyxml2|tinyxml2|libtinyxml2-dev|tinyxml2"
		"cxxopts|cxxopts|libcxxopts-dev|cxxopts"
		"nlohmann_json|nlohmann_json|nlohmann-json3-dev|nlohmann-json"
		"SQLiteCpp|SQLiteCpp|libsqlitecpp-dev|sqlitecpp"
		"reproc++|reproc|libreproc-dev|reproc"
		"LibArchive|libarchive|libarchive-dev|libarchive")
	string(REPLACE "|" ";" _parts "${dep}")
	list(GET _parts 0 _name)
	list(GET _parts 1 _fetch)
	list(GET _parts 2 _apt)
	list(GET _parts 3 _brew)

	if(_name IN_LIST GD_BUNDLED)
		list(APPEND GD_FETCH ${_fetch})
	else()
		# No version is passed to find_package, deliberately. The COMPATIBILITY
		# mode of a package's version file is its upstream author's choice, not
		# ours, and several of these use SameMajorVersion — under which a
		# "minimum" request is really a major-version match. Asking for
		# tinyxml2 >= 9.0 therefore REJECTED the installed 11.0.0, and reproc's
		# floor of 14.2 only works because 14 is still the current major. Ask
		# for any version and do the comparison here, where >= means >=.
		find_package(${_name} QUIET)
		set(_ver "${${_name}_VERSION}")

		# Installed, but older than we can use. This cannot fall back to a source
		# build the way an absent package can: a successful find_package has
		# already created imported targets, and the bundled build would then try
		# to create targets of the same names — plain `SQLiteCpp` and `reproc++`
		# collide directly, tinyxml2 collides on its ::tinyxml2 alias — which
		# CMake rejects with an "add_library cannot create target" error that
		# reads like a bug in this file. A failed find_package never loads the
		# config at all, which is why the absent case is safe and this one is not.
		if(${_name}_FOUND AND NOT _ver STREQUAL ""
				AND _ver VERSION_LESS "${GD_MIN_${_name}}")
			message(FATAL_ERROR
				"${_name} ${_ver} is installed, but gaindrive needs "
				"${GD_MIN_${_name}} or newer.\n\n"
				"  Upgrade the package, or build this one from source:\n"
				"      cmake -B build -DGAINDRIVE_BUNDLED_DEPS=${_name}\n\n"
				"  (That flag skips the system probe for ${_name} entirely, "
				"which is what makes the source build possible here.)\n")
		endif()
		# A package that ships no version file at all leaves _ver empty — cxxopts
		# upstream is one. Accept it rather than rebuild a library that is
		# probably fine; the compile would catch a genuinely too-old one.

		if(${_name}_FOUND)
			list(APPEND GD_SYSTEM "${_name}")
		else()
			# Absent entirely: safe to build from source, since nothing was
			# loaded. Appending to GD_BUNDLED is what the per-dependency
			# declaration blocks below key on, so they need no knowledge of how a
			# name got there.
			list(APPEND _gd_missing "${_name} >= ${GD_MIN_${_name}}")
			list(APPEND _gd_apt     "${_apt}")
			list(APPEND _gd_brew    "${_brew}")
			list(APPEND GD_BUNDLED  "${_name}")
			list(APPEND GD_FETCH    "${_fetch}")
		endif()
	endif()
endforeach()

if(_gd_missing)
	list(JOIN _gd_missing "\n      " _list)
	if(APPLE)
		list(JOIN _gd_brew " " _cmd)
		set(_install "brew install ${_cmd}")
	else()
		list(JOIN _gd_apt " " _cmd)
		set(_install "sudo apt install ${_cmd}\n      (those names are Debian 13 or newer; older releases differ)")
	endif()

	if(GAINDRIVE_ALLOW_FETCH)
		# Information, not a complaint: on a distribution that is behind this is
		# the normal, expected outcome of a perfectly good build.
		message(STATUS
			"gaindrive: not available from system packages, will build from source:\n"
			"      ${_list}\n"
			"    To use system copies instead, install:\n"
			"      ${_install}")
	else()
		message(FATAL_ERROR
			"These dependencies were not found on the system, or are too old:\n"
			"      ${_list}\n\n"
			"  GAINDRIVE_ALLOW_FETCH is OFF, so they cannot be built from source.\n"
			"  Install them:\n"
			"      ${_install}\n\n"
			"  Or drop -DGAINDRIVE_ALLOW_FETCH=OFF to have them built instead.\n")
	endif()
endif()

if(GD_BUNDLED)
	include(FetchContent)
	# Nothing may leak in from the machine: a bundled dependency's version is a
	# property of this file, not of the build host.
	set(FETCHCONTENT_TRY_FIND_PACKAGE_MODE NEVER)
endif()

# A bundled dependency must not install itself.
#
# FetchContent adds each one with add_subdirectory(), so its install() rules are
# ours by adoption and `sudo cmake --install build` scatters SQLiteCpp, taglib,
# tinyxml2 and reproc++ across the prefix -- headers, static libraries, pkgconfig
# and cmake package files. That is not merely untidy. The next configure then
# FINDS those copies, so the build silently stops using the pinned versions this
# file chose and starts using whatever it installed last time; and an installed
# taglib brings the ZLIB::ZLIB fault documented above with it.
#
# EXCLUDE_FROM_ALL on the declaration is what suppresses those rules. The
# targets are still built -- an inter-target dependency supersedes the exclusion,
# and gaindrive links every one of them -- so nothing is lost but the installing.
#
# Two spellings of that, because the FetchContent_Declare option is 3.28 or
# newer and this project's floor is 3.20 -- Ubuntu 22.04 ships CMake 3.22, and
# there the option is silently no option at all. What suppresses the rules is
# add_subdirectory()'s EXCLUDE_FROM_ALL, which has done so for far longer; 3.28
# only added a way to reach it through FetchContent. So old CMake gets the same
# effect by doing FetchContent's last step by hand. Verified on 3.22.1: with the
# declaration option alone a bundled taglib installs its library, its headers
# and its whole cmake package; through add_subdirectory it installs nothing.
#
# The split is by version rather than one path for both because
# FetchContent_Populate(<name>) is deprecated from 3.30 (CMP0169) -- each half
# uses the API of its own era, so neither warns.
#
# Three of the seven offer no install switch of their own, so this is not
# belt-and-braces for them: it is the only thing that works.
set(GD_FETCH_EXCLUDE "")
if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
	set(GD_FETCH_EXCLUDE EXCLUDE_FROM_ALL)
endif()

# ---- Bundled dependency declarations ---------------------------------------
# Each is declared only when it is actually being built, so an unrelated
# dependency's options never reach a build that does not include it.

if("cxxopts" IN_LIST GD_BUNDLED)
	# Already off for a sub-project -- it defaults to CXXOPTS_STANDALONE_PROJECT
	# -- but set explicitly, because "we depend on somebody's default" is the
	# kind of thing that stops being true in a version bump without a word.
	set(CXXOPTS_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	# v3.3.1 rather than v3.2.1 only because the older tag declares
	# cmake_minimum_required(VERSION 3.1...3.19), and CMake 4 rejects a <min>
	# below 3.5 even in the range form.
	FetchContent_Declare(
		cxxopts
		GIT_REPOSITORY https://github.com/jarro2783/cxxopts.git
		GIT_TAG        v3.3.1
		${GD_FETCH_EXCLUDE}
	)
endif()

if("nlohmann_json" IN_LIST GD_BUNDLED)
	# As with cxxopts: already off for a sub-project (it follows MAIN_PROJECT),
	# and pinned here so it stays that way.
	set(JSON_Install OFF CACHE BOOL "" FORCE)
	# Likewise: 3.11.3 declares 3.1...3.14, 3.12.0 declares 3.5...4.0.
	FetchContent_Declare(
		nlohmann_json
		GIT_REPOSITORY https://github.com/nlohmann/json.git
		GIT_TAG        v3.12.0
		${GD_FETCH_EXCLUDE}
	)
endif()

if("SQLiteCpp" IN_LIST GD_BUNDLED)
	# SQLiteCpp, taglib and tinyxml2 offer no "do not install" option of any
	# kind -- their install() calls are unconditional -- so EXCLUDE_FROM_ALL is
	# the only thing that stops them, and on CMake older than 3.28 nothing does.
	# Do not go looking for a switch to set here; there is none to find.
	#
	# SQLiteCpp bundles and builds sqlite3 from source (SQLITECPP_INTERNAL_SQLITE
	# defaults ON), which is what we want for a single self-contained binary.
	# Disable cpplint so it never searches for Python.
	set(SQLITECPP_RUN_CPPLINT OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		SQLiteCpp
		GIT_REPOSITORY https://github.com/SRombauts/SQLiteCpp.git
		GIT_TAG        3.3.2
		${GD_FETCH_EXCLUDE}
	)
endif()

if("TagLib" IN_LIST GD_BUNDLED)
	# Build taglib as a static library; disable things we don't need.
	set(BUILD_SHARED_LIBS     OFF CACHE BOOL "" FORCE)
	set(TAGLIB_BUILD_TESTS    OFF CACHE BOOL "" FORCE)
	set(TAGLIB_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
	set(BUILD_TESTING         OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		taglib
		GIT_REPOSITORY https://github.com/taglib/taglib.git
		GIT_TAG        v2.0.2
		${GD_FETCH_EXCLUDE}
	)
endif()

if("tinyxml2" IN_LIST GD_BUNDLED)
	FetchContent_Declare(
		tinyxml2
		GIT_REPOSITORY https://github.com/leethomason/tinyxml2.git
		GIT_TAG        10.0.0
		${GD_FETCH_EXCLUDE}
	)
endif()

if("reproc++" IN_LIST GD_BUNDLED)
	# reproc++ for spawning ffmpeg child processes.
	set(REPROC++ ON CACHE BOOL "" FORCE)
	# Unlike cxxopts and nlohmann_json, reproc's REPROC_INSTALL defaults to ON
	# whenever it is not building object libraries, so as a sub-project it
	# really does install itself -- headers, the static library, its cmake
	# package and a pkg-config file. This is the one of the four switches that
	# changes anything on its own.
	set(REPROC_INSTALL OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		reproc
		GIT_REPOSITORY https://github.com/DaanDeMeyer/reproc.git
		GIT_TAG        v14.2.5
		${GD_FETCH_EXCLUDE}
	)
endif()

if("LibArchive" IN_LIST GD_BUNDLED)
	# libarchive for extracting user-uploaded zip/tar archives. v3.8.9 rather
	# than v3.7.4, whose CMAKE_MINIMUM_REQUIRED(VERSION 2.8.12) CMake 4 refuses;
	# 3.8.x declares 3.17.
	set(ENABLE_TEST    OFF CACHE BOOL "" FORCE)
	# The original of the four, and for a while the only one -- which is why
	# every other bundled dependency was installing itself.
	set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
	FetchContent_Declare(
		libarchive
		GIT_REPOSITORY https://github.com/libarchive/libarchive.git
		GIT_TAG        v3.8.9
		${GD_FETCH_EXCLUDE}
	)

	# Trim optional backends gaindrive never uses. This only applies when we
	# build libarchive ourselves — with a system one the distribution has
	# already chosen its backends — and it matters most for the static link.
	# The worst offender is libarchive's xar reader: its libxml2 pulls in ICU
	# (libicuuc/libicudata), and because ICU is C++ it drags libstdc++/libgcc_s
	# back into an otherwise static link even with -static-libstdc++.
	# zlib/bz2/lzma/zstd stay enabled so zip and the common compressed-tar
	# uploads still extract.
	set(ENABLE_LIBXML2 OFF CACHE BOOL "" FORCE)   # xar -> libxml2 -> ICU
	set(ENABLE_EXPAT   OFF CACHE BOOL "" FORCE)
	set(ENABLE_ICONV   OFF CACHE BOOL "" FORCE)
	set(ENABLE_LZO     OFF CACHE BOOL "" FORCE)
	set(ENABLE_NETTLE  OFF CACHE BOOL "" FORCE)
	set(ENABLE_LIBB2   OFF CACHE BOOL "" FORCE)
endif()

# One line saying where each dependency came from, so the answer is in the
# configure log rather than something to infer from build noise later.
if(GD_SYSTEM)
	list(JOIN GD_SYSTEM ", " _s)
else()
	set(_s "(none)")
endif()
if(GD_BUNDLED)
	list(JOIN GD_BUNDLED ", " _b)
else()
	set(_b "(none)")
endif()
message(STATUS "gaindrive: system packages: ${_s}")
message(STATUS "gaindrive: built from source: ${_b}")

# ---- Uninstall -------------------------------------------------------------
#
# Defined HERE, above FetchContent_MakeAvailable, and that position is
# load-bearing rather than tidy: taglib declares an uninstall target of its own,
# guarded by `if(NOT TARGET uninstall)`. Below the fetch, taglib wins the race
# and the configure fails outright with "another target with the same name
# already exists"; above it, taglib sees ours and skips its own -- which is also
# the outcome we want, since taglib's would run taglib's uninstall script rather
# than gaindrive's. Moving these two calls back down to the install section,
# where they otherwise belong, breaks every build with a bundled taglib.
#
# CMake generates no uninstall rule of its own, and here that is not a nicety:
# a bundled dependency is a subproject, so its own install() rules run as part
# of ours and `sudo cmake --install build` scatters SQLiteCpp, reproc++,
# taglib, tinyxml2 and their cmake package files across the prefix. Undoing
# that by hand means reading install_manifest.txt and trusting yourself with
# rm as root, which is exactly the thing to automate rather than document.
#
# The dry run exists because this is normally run as root on a server. See
# cmake/uninstall.cmake for what the manifest can and cannot account for.
add_custom_target(uninstall
	COMMAND ${CMAKE_COMMAND}
		-DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
		-DPREFIX=${CMAKE_INSTALL_PREFIX}
		-DBUILD_DIR=${CMAKE_BINARY_DIR}
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake
	USES_TERMINAL
	COMMENT "Removing everything the last install of this build wrote")

add_custom_target(uninstall-dry-run
	COMMAND ${CMAKE_COMMAND}
		-DMANIFEST=${CMAKE_BINARY_DIR}/install_manifest.txt
		-DPREFIX=${CMAKE_INSTALL_PREFIX}
		-DBUILD_DIR=${CMAKE_BINARY_DIR}
		-DDRY_RUN=ON
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/uninstall.cmake
	USES_TERMINAL
	COMMENT "Listing what an uninstall would remove")

if(GD_FETCH)
	if(GD_FETCH_EXCLUDE)
		FetchContent_MakeAvailable(${GD_FETCH})
	else()
		# FetchContent_MakeAvailable() with the pre-3.28 API: populate, then
		# add the subdirectory ourselves so EXCLUDE_FROM_ALL can be passed.
		# Everything MakeAvailable would otherwise do that matters here is
		# these two calls; the find_package redirect bookkeeping it also does
		# on 3.24+ is not used, because a bundled dependency is never looked up
		# with find_package -- FETCHCONTENT_TRY_FIND_PACKAGE_MODE is NEVER and
		# the probe above excludes it from the find_package loop entirely.
		foreach(_gd_dep IN LISTS GD_FETCH)
			string(TOLOWER "${_gd_dep}" _gd_lc)
			FetchContent_GetProperties(${_gd_dep})
			if(NOT ${_gd_lc}_POPULATED)
				FetchContent_Populate(${_gd_dep})
				add_subdirectory("${${_gd_lc}_SOURCE_DIR}"
				                 "${${_gd_lc}_BINARY_DIR}" EXCLUDE_FROM_ALL)
			endif()
		endforeach()
	endif()
endif()

if("TagLib" IN_LIST GD_BUNDLED)
	# Reproduce, for the in-tree build, the include layout taglib's INSTALLED
	# form provides — its exported target offers both <prefix>/include and
	# <prefix>/include/taglib, and its PUBLIC_HEADER install flattens
	# toolkit/tpropertymap.h to include/taglib/tpropertymap.h. The source tree
	# keeps the toolkit headers in a subdirectory instead, so without these three
	# entries the same #include lines resolve against a system taglib and fail
	# against this one.
	# taglib/mp4 for the same reason: mp4file.h is the only container-specific
	# header gaindrive includes (read_song_audio_codec() needs
	# MP4::Properties::codec() to tell AAC from ALAC), and installed taglib
	# flattens it to include/taglib/mp4file.h while the source tree does not.
	# Everything it pulls in cross-directory -- tfile.h, tag.h, taglib_export.h
	# -- is already covered by the three entries above.
	target_include_directories(tag PUBLIC
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}>
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>
		$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/mp4>
	)
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
			"Build that one from source instead with "
			"-DGAINDRIVE_BUNDLED_DEPS=<name>, and "
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
		${CMAKE_CURRENT_SOURCE_DIR}/web/link.html
		${CMAKE_CURRENT_SOURCE_DIR}/web/style.css
		${CMAKE_CURRENT_SOURCE_DIR}/web/app.js
		${CMAKE_CURRENT_SOURCE_DIR}/web/theme.js
		${CMAKE_CURRENT_SOURCE_DIR}/web/favicon.svg
		${CMAKE_CURRENT_SOURCE_DIR}/web/material-symbols-rounded.woff2
		${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_web.cmake
	COMMENT "Embedding web client files"
)

# The systemd unit template, compiled in so --install-service works from a build
# tree -- which is where it is normally run, step (c) following step (b).  Its
# own script rather than another entry in embed_web.cmake's TEXT_FILES: that one
# is keyed to a single WEB_DIR and carries filename -> identifier and
# filename -> MIME maps that a unit file has no place in.  Both the template and
# the script belong in DEPENDS, or editing either will not rebuild.
add_custom_command(
	OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/embedded_unit.cc
	COMMAND ${CMAKE_COMMAND}
		-DINPUT=${CMAKE_CURRENT_SOURCE_DIR}/dist/gaindrive.service.in
		-DOUTPUT=${CMAKE_CURRENT_BINARY_DIR}/embedded_unit.cc
		-DHEADER=embedded_unit.hh
		-DVAR=service_unit
		-P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
	DEPENDS
		${CMAKE_CURRENT_SOURCE_DIR}/dist/gaindrive.service.in
		${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_text.cmake
	COMMENT "Embedding systemd unit template"
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
	src/service.cc
	${CMAKE_CURRENT_BINARY_DIR}/embedded_unit.cc
	src/gaindrive.cc
	src/subsonic.cc
	src/textutil.cc
	src/netaddr.cc
	src/authz.cc
	src/apientry.cc
	src/batchtools.cc
	src/infolookup.cc
	src/apiweb.cc
	src/apisystem.cc
	src/apibrowse.cc
	src/apiplaylist.cc
	src/apistream.cc
	src/apicast.cc
	src/apiedit.cc
	src/apifetch.cc
	src/apifallback.cc
	src/mediastore.cc
	src/streamer.cc
	src/transcodecache.cc
	src/imagescale.cc
	src/coverart.cc
	src/videoart.cc
	src/videoname.cc
	src/artistmatch.cc
	src/tmdb.cc
	src/urlfetch.cc
	src/stamp.cc
	src/castmanager.cc
	src/folderwatcher.cc
	src/codecs.cc
	src/chapters.cc
	src/untrusted.cc
	src/dvd.cc
	src/jsonread.cc
	src/parallel.cc
	src/proc.cc
	src/md5.cc
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
if("LibArchive" IN_LIST GD_BUNDLED)
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

# ---- gaindrive-cast --------------------------------------------------------
#
# A diagnostic front end to the Chromecast code, for investigating mDNS
# discovery from the command line. It links castmanager.cc and stamp.cc and
# nothing else: castmanager includes no project header but those two, so no
# database, no HTTP server and — note the absence of ${EMBEDDED_WEB_CC} — not
# even the embedded web client, whose custom command fires only for a target
# that lists its output. Not installed; this is a tool, not a program we ship.
option(GAINDRIVE_CAST_TOOL "Build the gaindrive-cast discovery diagnostic" ON)
if(GAINDRIVE_CAST_TOOL)
	add_executable(gaindrive-cast
		src/casttool.cc
		src/castmanager.cc
		src/stamp.cc
		# castmanager.cc reads the receiver's JSON through jsonread.hh, whose
		# bodies now live in a .cc rather than in the header.
		src/jsonread.cc
	)
	target_include_directories(gaindrive-cast PRIVATE src third_party)
	# ${GD_DEP_TARGETS} rather than named packages: the spellings differ
	# between the system and bundled modes. Only cxxopts and nlohmann_json are
	# actually reached, and both are header-only.
	target_link_libraries(gaindrive-cast PRIVATE
		${GD_DEP_TARGETS}
		OpenSSL::SSL
		OpenSSL::Crypto
		Threads::Threads
	)
	if(GAINDRIVE_STATIC)
		target_link_options(gaindrive-cast PRIVATE -static-libstdc++ -static-libgcc)
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

# No systemd unit is installed here, and that is deliberate rather than an
# omission.  gaindrive writes its own with `sudo gaindrive <options>
# --install-service`, which is the documented route because it reaches everyone:
# the release pipeline's only artifact is a static binary, so `cmake --install`
# never served most users at all.  The unit it writes runs as the person who ran
# sudo rather than as a system account, which is what makes the recursive
# chgrp/chmod over the music tree that the old instructions needed unnecessary.
#
# dist/gaindrive.service.in survives as the single source of the unit text --
# embedded into the binary, filled in at install time -- and is what a
# distribution packager should start from to get the system-account shape back.

# Example config installs next to the config the binary actually reads, which is
# GAINDRIVE_SYSCONFDIR and not CMAKE_INSTALL_SYSCONFDIR: on Linux that stays
# /etc regardless of prefix, exactly as before, while a Homebrew build redirects
# both the default and this file into its own prefix. Installed with an .example
# suffix so it never clobbers an admin's live config.
install(FILES dist/gaindrive.conf.example DESTINATION ${GAINDRIVE_SYSCONFDIR})

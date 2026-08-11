# Builds the fully static, stripped release binary inside an Alpine container,
# so the exact artifact the release workflow ships can be reproduced locally.
#
# musl and not glibc: a static glibc has no working getaddrinfo, which would
# silently kill the MusicBrainz/Wikidata/Discogs lookups. See the
# GAINDRIVE_STATIC_LIBC comment in CMakeLists.txt.
#
# Two ways in, both ending up here:
#    cmake --build build --target static                  (configured tree)
#    cmake [-DVERSION=1.2.3] -P cmake/static_build.cmake  (standalone; CI uses this)
#
# The standalone form is why this is a script rather than only a target:
# configuring the host tree first would clone every FetchContent dependency and
# require OpenSSL on the host, purely to invoke docker.

cmake_minimum_required(VERSION 3.20)

set(ALPINE_IMAGE alpine:3.22)
set(BUILD_DIR    build-static)

# This is the one build that passes GAINDRIVE_BUNDLED_DEPS=ON, so no dev
# packages for the C++ dependencies appear below. That is not a preference: a
# fully static musl link needs .a archives, and Alpine ships no taglib-static,
# no tinyxml2-static and no reproc-static, and no SQLiteCpp in any branch. The
# ordinary build uses system packages and needs no container at all.
#
# -static-suffixed packages carry the .a archives; CMAKE_FIND_LIBRARY_SUFFIXES
# is forced to ".a" under GAINDRIVE_STATIC, so a missing one silently disables
# that libarchive backend rather than failing the build.
set(APK_PACKAGES
	build-base cmake git perl linux-headers ca-certificates
	openssl-dev openssl-libs-static zlib-dev zlib-static
	bzip2-dev bzip2-static xz-dev xz-static zstd-dev zstd-static)

get_filename_component(SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)

find_program(CONTAINER NAMES docker podman)
if(NOT CONTAINER)
	message(FATAL_ERROR "docker or podman is required for the static build")
endif()

if(VERSION)
	set(VERSION_ARG "-DGAINDRIVE_VERSION=${VERSION}")
endif()

# Under docker the container writes as root, so hand the tree back on the way
# out. Not under rootless podman, where uid 0 already maps to the caller and an
# explicit chown would push the files into a subuid the caller cannot touch.
if(CONTAINER MATCHES "docker")
	execute_process(COMMAND id -u OUTPUT_VARIABLE UID OUTPUT_STRIP_TRAILING_WHITESPACE)
	execute_process(COMMAND id -g OUTPUT_VARIABLE GID OUTPUT_STRIP_TRAILING_WHITESPACE)
	set(CHOWN "chown -R ${UID}:${GID} ${BUILD_DIR}")
endif()

string(JOIN " " APK_LIST ${APK_PACKAGES})

# The bind mount makes the checkout look like someone else's to git, which stops
# FetchContent's clones dead; safe.directory is the documented way round it.
set(SCRIPT "set -eu
apk add --no-cache ${APK_LIST}
git config --global --add safe.directory '*'
cmake -B ${BUILD_DIR} -DCMAKE_BUILD_TYPE=Release -DGAINDRIVE_STATIC_LIBC=ON -DGAINDRIVE_BUNDLED_DEPS=ON ${VERSION_ARG}
cmake --build ${BUILD_DIR} -j\"$(nproc)\"
${CHOWN}")

execute_process(
	COMMAND ${CONTAINER} run --rm -v "${SOURCE_DIR}:/src" -w /src
	        ${ALPINE_IMAGE} sh -c "${SCRIPT}"
	RESULT_VARIABLE rc)

if(NOT rc EQUAL 0)
	message(FATAL_ERROR "static build failed (${rc})")
endif()

message(STATUS "Static binary: ${SOURCE_DIR}/${BUILD_DIR}/gaindrive")

cmake_minimum_required(VERSION 3.20)
set(CMAKE_POLICY_DEFAULT_CMP0148 NEW)
project(gaindrive CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

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

FetchContent_MakeAvailable(httplib cxxopts nlohmann_json SQLiteCpp taglib tinyxml2)

target_include_directories(tag PUBLIC
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}>
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib>
	$<BUILD_INTERFACE:${taglib_SOURCE_DIR}/taglib/toolkit>
)

add_executable(gaindrive
	src/main.cc
	src/gaindrive.cc
	src/mediastore.cc
	src/stamp.cc
)

target_link_libraries(gaindrive PRIVATE
	httplib::httplib
	cxxopts::cxxopts
	nlohmann_json::nlohmann_json
	SQLiteCpp
	tag
	tinyxml2::tinyxml2
)

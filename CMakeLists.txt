cmake_minimum_required(VERSION 3.20)
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

FetchContent_MakeAvailable(httplib cxxopts nlohmann_json)

add_executable(gaindrive src/main.cc src/gaindrive.cc)

target_link_libraries(gaindrive PRIVATE
	httplib::httplib
	cxxopts::cxxopts
	nlohmann_json::nlohmann_json
)

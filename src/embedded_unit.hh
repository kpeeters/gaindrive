#pragma once

#include <string_view>

// The systemd unit template, dist/gaindrive.service.in, compiled in by
// cmake/embed_text.cmake.  It is embedded rather than installed and read at
// runtime because --install-service is most often run from a build tree: step
// (c) of the documented flow follows step (b) immediately, so the binary is
// frequently ./build/gaindrive, and a tool whose job is to tell you to run
// `cmake --install` cannot itself depend on having been installed.
namespace embedded {
extern const std::string_view service_unit;
}

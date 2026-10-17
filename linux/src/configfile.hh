#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>

// JSON files in ~/.config/gaindrive-player. A missing or unreadable file
// reads as null.
nlohmann::json read_config(const std::string& name);
void           write_config(const std::string& name, const nlohmann::json& j);

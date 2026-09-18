#pragma once

// Tiny RFC 1321 MD5 — used only for Subsonic token auth (md5(password+salt)).
// Not a general-purpose crypto library.

#include <string>

std::string md5_hex(const std::string& msg);

#pragma once

// Who a request is, and what that account is allowed to touch.
//
// Split out of gaindrive.cc because check_auth is the first line of all but
// two endpoints in the server, and the item-level checks are reached from
// browse, cover art, playlist, stream, cast and edit alike. The login throttle
// lives here too rather than beside it, because check_auth is its only caller
// and a throttle anything else could reach would be a throttle something else
// could reset.

#include <cstddef>
#include <string>

#include <httplib.h>

#include "mediastore.hh"

// Authenticates u/p or u/t/s against the user database, applying the login
// throttle. Returns false having already written the refusal.
bool check_auth(const httplib::Request& req, httplib::Response& res,
                MediaStore& store);

// Entries currently in the login-throttle map. Purely for getServerStatus;
// a visible spike means someone is guessing passwords.
std::size_t throttle_entries();

// Whether the authenticated user may drive a Chromecast.
bool check_cast_perm(const httplib::Request& req, httplib::Response& res,
                     MediaStore& store, bool use_json);

// Returns true if the authenticated user may write into their personal uploads
// folder. Admins may regardless - the same rule /upload has applied inline
// since it was written, lifted out here because four more endpoints now need it
// and a permission check with five copies is a permission check with four
// chances of being forgotten.
bool check_upload_perm(const httplib::Request& req, httplib::Response& res,
                       MediaStore& store, bool use_json);

// Whether this account may modify the item at `rel_path`: admins anywhere,
// everyone else only inside their own directory under the uploads root.
bool item_write_allowed(const httplib::Request& req, MediaStore& store,
                        const std::string& uploads_root_name,
                        const std::string& rel_path);
bool check_item_write_perm(const httplib::Request& req, httplib::Response& res,
                           MediaStore& store,
                           const std::string& uploads_root_name,
                           const std::string& rel_path, bool use_json);

// The same question for reading. Anything outside the uploads root is shared
// and readable; inside it, only the owner and an admin may look.
bool item_read_allowed(const httplib::Request& req, MediaStore& store,
                       const std::string& uploads_root_name,
                       const std::string& rel_path);
bool check_item_read_perm(const httplib::Request& req, httplib::Response& res,
                          MediaStore& store,
                          const std::string& uploads_root_name,
                          const std::string& rel_path, bool use_json);

// Resolves the `personal` parameter to what get_artist_dirs() wants.
//
// "true" is the caller's own uploads; "*" is everybody's and is **admin only**,
// because it is other people's material. Anything else, including absent, is the
// shared library. Returns false having already written the refusal.
bool personal_scope(const httplib::Request& req, httplib::Response& res,
                    MediaStore& store, bool use_json, std::string& out);

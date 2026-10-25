#pragma once

#include "model.hh"

#include <vector>

// The same artist or album on several servers, folded into one row as the
// phones do. Each takes one list per server, in the server list's order; the
// first server's copy keeps its ref, cover and place, and the others go into
// `also`. Only copies on different servers fold: two artists of one name on
// one server are two artists.

// By name, trimmed and case-folded. Album counts add up.
std::vector<Artist> merge_artists(const std::vector<std::vector<Artist>>& lists);
// By the letters and digits of artist and title, case-folded, so
// punctuation and spacing differences between two rips do not matter.
std::vector<Album>  merge_albums(const std::vector<std::vector<Album>>& lists);

// The key artists fold by.
std::string artist_key(const std::string& name);

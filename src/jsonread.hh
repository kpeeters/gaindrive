#pragma once

#include <string>

#include <nlohmann/json.hpp>

// Reading a field out of JSON that someone else wrote.
//
// Every field gaindrive takes from a metadata provider (MusicBrainz, Wikidata,
// Wikipedia, TheAudioDB, Discogs, TMDB) or from a Chromecast receiver goes
// through these, and none of them can throw. Neither of the obvious ways to
// write it holds:
//
// * `value(key, default)` substitutes the default only when the key is
//   *absent*, and throws type_error.302 when the key is present holding null
//   — which is exactly how a provider says it has nothing. TheAudioDB answers
//   a miss with `"artists": null`, and a TMDB result with no poster carries
//   `"poster_path": null`; that one killed an entire scan.
// * `operator[]` throws type_error.305 on a string or an array, so one field
//   arriving in an unexpected shape takes down a whole chain such as
//   `claims["P18"][0]["mainsnak"]["datavalue"]`. The non-const overload also
//   silently *inserts* nulls into the parsed document as it walks, and the
//   const one is undefined behaviour — not an exception — on a missing key.
//
// A field that is missing, null or the wrong type is worth exactly as much as
// an empty one at every one of these call sites: the metadata lookups are
// best-effort embellishment and already handle a provider that does not know,
// and a Chromecast status field that is not what it should be says nothing
// about playback. So each helper narrows to what it wants and yields nothing
// when it does not find it.
//
// Where it matters most: both the scan and the Cast poll loop run in detached
// threads, where an escaping exception is std::terminate — a dead server, not
// a failed request.

// A member of an object, or a null that stays null. Chainable.
inline const nlohmann::json& jsub(const nlohmann::json& j,
                                  const std::string& key)
	{
	static const nlohmann::json none;
	if (!j.is_object()) return none;
	auto it = j.find(key);
	return it == j.end() ? none : *it;
	}

// An element of an array, or null. Chainable with the above.
inline const nlohmann::json& jidx(const nlohmann::json& j, size_t i)
	{
	static const nlohmann::json none;
	if (!j.is_array() || i >= j.size()) return none;
	return j[i];
	}

// A string member, or empty.
inline std::string jstr(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_string() ? v.get<std::string>() : std::string();
	}

// A numeric member, or 0. A number arriving as a JSON string stays 0 — every
// caller here would rather see the absent value than guess.
inline double jnum(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_number() ? v.get<double>() : 0.0;
	}

inline int jint(const nlohmann::json& j, const std::string& key)
	{
	const auto& v = jsub(j, key);
	return v.is_number_integer() ? v.get<int>() : 0;
	}

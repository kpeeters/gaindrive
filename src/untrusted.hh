#pragma once

#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>

// Storing a string that someone else wrote.
//
// The sibling of jsonread.hh: that one guards the *type* of a provider's JSON,
// so a null or an array cannot throw; this one guards the *content* of the
// strings that come out of it. Every field gaindrive takes from MusicBrainz,
// Wikidata, Wikipedia, TheAudioDB, Discogs or TMDB is written to a cache table
// and served to every client there is — the web client, the iOS app, and any
// third-party Subsonic app — so the server is the only place a guarantee about
// it can be made. A client cannot be told to be careful, and two of them
// already were not.
//
// Three failure modes, and none of them is markup breaking out of a response:
//
//  * **XML.** tinyxml2 entity-escapes < > & " ' on output, so that part is
//    already safe. It does not reject **control characters**, which are
//    illegal in XML 1.0 and cannot be represented at all — so one 0x01 in a
//    biography makes the whole subsonic-response unparseable for every
//    conformant client, not merely that one field. A NUL is worse: everything
//    reaches tinyxml2 as .c_str(), so it silently truncates the value.
//  * **JSON.** nlohmann's dump() *throws* on invalid UTF-8. That is why
//    utf8_clean() exists at all, and why it now lives here rather than as a
//    static in gaindrive.cc: the throw happens on an httplib thread and
//    becomes a bare 500, and since the bytes are in artist_info_cache — which
//    has no TTL — that artist stays a 500 for ever.
//  * **URLs.** A provider hands over a link and a client puts it in an href.
//    Nothing about escaping helps: `javascript:alert(1)` needs no special
//    character to be dangerous, so a URL has to be *validated*, not escaped.
//
// The first two are separate jobs and both are needed — the same split
// chapters.hh states for chapter titles, which are the one field family in
// this codebase that already had both. Provider fields had neither.

// Bounds for the fields above. Guesses at nothing: they are the point at which
// a provider's answer has stopped being prose about an artist and become a
// payload, and each comes straight back out through a JSON document and an XML
// element — the reasoning MAX_CHAPTER_NAME_BYTES already records.
inline constexpr size_t MAX_PROSE_BYTES = 8000;
inline constexpr size_t MAX_NAME_BYTES  = 500;

// Valid UTF-8 only, truncated on a character boundary.
//
// Moved here verbatim from gaindrive.cc, where it was a file-local static and
// so unreachable from mediastore.cc and tmdb.cc — which is where the TMDB
// writes and the cache reads happen, and therefore where it is now needed.
//
// Not defensive programming for its own sake: nlohmann's dump() *throws* on
// invalid UTF-8, on an httplib thread, where it becomes a bare 500 with
// nothing in the log. A half-copied multi-byte sequence is exactly what a
// byte-count truncate produces, which is why the bound is applied here rather
// than by a caller's substr().
inline std::string utf8_clean(std::string_view s, size_t max_bytes)
	{
	std::string out;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char c = s[i];
		size_t len = c < 0x80 ? 1
		           : (c & 0xE0) == 0xC0 ? 2
		           : (c & 0xF0) == 0xE0 ? 3
		           : (c & 0xF8) == 0xF0 ? 4 : 0;
		if (len == 0 || i + len > s.size()) { i++; continue; }
		bool ok = true;
		for (size_t k = 1; k < len; k++)
			if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) ok = false;
		if (!ok) { i++; continue; }
		if (out.size() + len > max_bytes) break;
		out.append(s.substr(i, len));
		i += len;
		}
	return out;
	}

// A biography, album notes, a film's plot: prose from a provider, on its way
// into a cache column and out through both response formats.
//
// **Newline and tab survive and every other control character does not**, and
// that is the whole decision here. 0x09, 0x0A and 0x0D are the only C0
// characters XML 1.0 permits, so the rest cannot be served at all; and the
// newlines are worth keeping rather than flattening, because these fields are
// several paragraphs of plain text and the paragraph breaks are the only
// structure they have. The web client renders them with white-space: pre-line
// for exactly that reason.
//
// CR and CRLF both become a bare LF — TheAudioDB sends CRLF — so that a
// consumer counting line breaks sees one thing rather than three, and 0x0D
// need not be carried as a third legal case.
//
// Filtering the control bytes before validating the encoding is safe in either
// order and done in this one deliberately: every byte removed here is
// single-byte ASCII, so removing it cannot orphan a continuation byte and turn
// a valid sequence into an invalid one.
inline std::string clean_prose(std::string_view s, size_t max_bytes)
	{
	std::string out;
	out.reserve(s.size());
	for (size_t i = 0; i < s.size(); ++i) {
		unsigned char c = s[i];
		if (c == '\r') {
			if (i + 1 < s.size() && s[i + 1] == '\n') continue;  // CRLF: keep the LF
			out.push_back('\n');
			continue;
			}
		if (c == '\n' || c == '\t') { out.push_back(static_cast<char>(c)); continue; }
		if (c < 0x20 || c == 0x7f) continue;
		out.push_back(static_cast<char>(c));
		}
	return utf8_clean(out, max_bytes);
	}

// A link from a provider, or nothing.
//
// Validated rather than cleaned, because there is no such thing as escaping a
// scheme: a client puts this in an href, and `javascript:` needs no special
// character to be a one-click XSS. So the answer is an allow-list of two
// schemes and a refusal for everything else — including a scheme-relative
// `//host/path`, which resolves against whatever page is holding it.
//
// Empty on refusal, because that is what every consumer already understands:
// the field is TEXT NOT NULL DEFAULT '' in both cache tables, and the clients
// draw a link only when it is non-empty. Dropping a bad link therefore needs
// no new state anywhere, and a link that cannot be trusted is worth less than
// no link at all.
//
// The character rejections are not decoration. A space or a control character
// in a URL that is later split into host and path — which portrait_fetch()
// does — is a request-splitting attempt, and `<`, `>` and `"` are there
// because a URL is the one provider string that reaches an HTML attribute in
// every client that renders one.
inline std::string clean_url(std::string_view s, size_t max_bytes = 2048)
	{
	if (s.size() > max_bytes) return std::string();

	auto starts_with_ci = [&](std::string_view p) {
		if (s.size() < p.size()) return false;
		for (size_t i = 0; i < p.size(); ++i)
			if (std::tolower((unsigned char)s[i]) != p[i]) return false;
		return true;
		};
	if (!starts_with_ci("http://") && !starts_with_ci("https://"))
		return std::string();

	for (unsigned char c : s)
		if (c < 0x20 || c == 0x7f || c == ' ' ||
		    c == '<' || c == '>' || c == '"')
			return std::string();

	// Last rather than first: the checks above are the security ones and are
	// cheap, and this one only decides whether the bytes can be serialised.
	std::string out = utf8_clean(s, max_bytes);
	if (out.size() != s.size()) return std::string();
	return out;
	}

// A genre name from a provider.
//
// clean_prose's rules minus the newlines — a genre is a label, not prose, so a
// line break in one is damage rather than structure — plus the one rule that
// is specific to where these are kept: **a name containing '|' is dropped
// outright.** video_meta.genre is a pipe-joined list, and the comment on that
// column rests the separator's safety on "no TMDB genre contains one". That is
// true today and is an assumption about a third party's data, which is the
// kind this header exists to stop making: a genre carrying a pipe would not
// corrupt one row's list, it would silently become *two* genres, and
// song_genres is aggregated and GROUP BY-ed across the library.
//
// Dropped rather than stripped, because a name we have altered is a name that
// no longer matches the one TMDB will send next time, and the row it would
// merge with under getGenres is somebody else's.
inline std::string clean_genre(std::string_view s)
	{
	if (s.find('|') != std::string_view::npos) return std::string();
	std::string out;
	out.reserve(s.size());
	for (unsigned char c : s)
		if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
	size_t b = out.find_first_not_of(" \t");
	if (b == std::string::npos) return std::string();
	size_t e = out.find_last_not_of(" \t");
	return utf8_clean(std::string_view(out).substr(b, e - b + 1), MAX_NAME_BYTES);
	}

// Whether a string is shaped like a MusicBrainz identifier.
//
// Lifted out of mb_uuid() so that one definition covers both paths that need
// it. The comment there gives the reason and it applies with more force to a
// provider than to a tag: these are concatenated into a MusicBrainz URL
// *path*, so anything not shaped like a UUID is discarded rather than reasoned
// about downstream. A search result's `id` had been trusted where a file's tag
// was not, which is the wrong way round — a tag at least came from a machine
// its owner controls.
inline bool is_uuid(std::string_view v)
	{
	if (v.size() != 36) return false;
	for (size_t i = 0; i < v.size(); ++i) {
		bool dash = (i == 8 || i == 13 || i == 18 || i == 23);
		if (dash ? v[i] != '-' : !std::isxdigit((unsigned char)v[i]))
			return false;
		}
	return true;
	}


#include "artistmatch.hh"
#include "jsonread.hh"

#include <cctype>

#include <nlohmann/json.hpp>

// How much MusicBrainz has to like a hit that matched nothing exactly before
// it is believed.  Reached only when neither the primary name nor any alias of
// any candidate keys equal to the folder name -- a misspelling, a name with a
// year or a release tag left on it, a folder that is not an artist at all.
// Below this the first hit is a guess, and every consequence of a wrong one is
// silent: a biography, a portrait and a Last.fm link, all confidently
// attributed to somebody else and cached with nothing to say they are wrong.
static constexpr int MB_MIN_SCORE = 70;

std::string artist_key(const std::string& s)
	{
	std::string r;
	for (unsigned char c : s) {
		if (c >= 0x80)                          r += static_cast<char>(c);
		else if (c == '_' || std::isspace(c))   r += ' ';
		else if (std::isalnum(c))               r += static_cast<char>(std::tolower(c));
		}
	// Collapse runs of space, and trim.
	std::string out;
	for (char c : r)
		if (c != ' ' || (!out.empty() && out.back() != ' ')) out += c;
	while (!out.empty() && out.back() == ' ') out.pop_back();
	return out;
	}

// Latin letters with diacritics, folded to ASCII, for provider matching only.
//
// This is the one normalisation artist_key() deliberately refuses, and the
// reason the two are different functions rather than one: artist_key() compares
// two strings *a user controls* -- a file's ARTIST tag against its folder name
// -- where an accent is a real difference and folding it hides one. Here the
// comparison is against MusicBrainz's data, which the user does not control and
// which spells a romanization however its editors did: 坂本龍一's aliases are
// "Ryūichi Sakamoto" and "Ryûichi Sakamoto" and there is no plain-ASCII one, so
// a folder called "Ryuichi Sakamoto" matches none of them exactly. Without this
// he loses to the score-100 duo "Alva Noto + Ryuichi Sakamoto", which is the
// wrong-artist failure the exactness rule exists to prevent.
//
// It is also what MusicBrainz's own search already does -- folding is why he is
// in the results at all -- so matching within those results the same way is
// agreeing with the provider rather than guessing past it.
//
// Latin-1 Supplement and Latin Extended-A only, both two bytes in UTF-8.
// Everything else passes through byte for byte: CJK, Cyrillic, Greek and Hangul
// are not accented ASCII, and mapping them would be the transliteration this
// codebase has no business attempting without ICU.
static const char* const FOLD_00C0[64] = {
	"a","a","a","a","a","a","ae","c", "e","e","e","e","i","i","i","i",
	"d","n","o","o","o","o","o",nullptr, "o","u","u","u","u","y","th","ss",
	"a","a","a","a","a","a","ae","c", "e","e","e","e","i","i","i","i",
	"d","n","o","o","o","o","o",nullptr, "o","u","u","u","u","y","th","y"
	};

static const char* const FOLD_0100[128] = {
	"a","a","a","a","a","a","c","c", "c","c","c","c","c","c","d","d",
	"d","d","e","e","e","e","e","e", "e","e","e","e","g","g","g","g",
	"g","g","g","g","h","h","h","h", "i","i","i","i","i","i","i","i",
	"i","i","ij","ij","j","j","k","k", "k","l","l","l","l","l","l","l",
	"l","l","l","n","n","n","n","n", "n","n","n","n","o","o","o","o",
	"o","o","oe","oe","r","r","r","r", "r","r","s","s","s","s","s","s",
	"s","s","t","t","t","t","t","t", "u","u","u","u","u","u","u","u",
	"u","u","u","u","w","w","y","y", "y","z","z","z","z","z","z","s"
	};

// The two-byte UTF-8 range above, decoded and folded; anything else copied.
static std::string fold_diacritics(const std::string& s)
	{
	std::string out;
	for (size_t i = 0; i < s.size(); ) {
		unsigned char b0 = static_cast<unsigned char>(s[i]);
		if (b0 < 0x80) { out += static_cast<char>(b0); i++; continue; }
		if ((b0 & 0xE0) == 0xC0 && i + 1 < s.size()
		    && (static_cast<unsigned char>(s[i + 1]) & 0xC0) == 0x80) {
			unsigned cp = ((b0 & 0x1Fu) << 6)
			            | (static_cast<unsigned char>(s[i + 1]) & 0x3Fu);
			const char* r = cp >= 0x00C0 && cp <= 0x00FF ? FOLD_00C0[cp - 0x00C0]
			              : cp >= 0x0100 && cp <= 0x017F ? FOLD_0100[cp - 0x0100]
			              : nullptr;
			if (r) { out += r; i += 2; continue; }
			}
		// Not a mapped letter: copy this byte and carry on, so a multi-byte
		// sequence survives intact one byte at a time.
		out += static_cast<char>(b0);
		i++;
		}
	return out;
	}

// The key mb_pick_artist() compares with: artist_key()'s rule, plus the fold.
static std::string provider_key(const std::string& s)
	{
	return artist_key(fold_diacritics(s));
	}

std::string mb_artist_query(const std::string& name)
	{
	std::string esc;
	for (char c : name) {
		if (c == '\\' || c == '"') esc += '\\';
		esc += c;
		}
	return "artist:\"" + esc + "\" OR alias:\"" + esc + "\"";
	}

std::optional<MbArtistMatch> mb_pick_artist(const std::string& body,
                                             const std::string& name)
	{
	// Non-throwing, and note a discarded parse is *not* null -- is_null() is
	// false for it and every jsonread helper narrows on the positive type, so
	// garbage falls through to "no candidates" rather than to a throw.
	auto j = nlohmann::json::parse(body, nullptr, false);

	const std::string want = provider_key(name);
	if (want.empty()) return std::nullopt;

	const auto& artists = jsub(j, "artists");
	if (!artists.is_array()) return std::nullopt;

	std::optional<MbArtistMatch> best;
	for (const auto& a : artists) {
		MbArtistMatch cand;
		cand.mbid  = jstr(a, "id");
		cand.name  = jstr(a, "name");
		cand.score = jint(a, "score");
		if (cand.mbid.empty()) continue;

		cand.exact = provider_key(cand.name) == want;
		// An alias is how a romanization, a stage name or a translation is
		// spelled, so this is the half that finds an artist filed under a name
		// MusicBrainz does not use as the primary one.  The search response
		// carries aliases inline, so it costs no second request.  is_array()
		// rather than a bare loop because iterating a nlohmann *scalar* yields
		// that one scalar, so a malformed `aliases` would be read as one alias
		// object -- an empty name, which keys empty.  The guard on `want`
		// above is what stops two empty keys ever comparing equal.
		const auto& aliases = jsub(a, "aliases");
		if (!cand.exact && aliases.is_array())
			for (const auto& al : aliases)
				if (provider_key(jstr(al, "name")) == want) { cand.exact = true; break; }

		// Exactness first, then MusicBrainz's own score.  Ties keep the order
		// MusicBrainz returned, which is why this is a strict improvement
		// rather than >=.
		if (!best
		    || (cand.exact && !best->exact)
		    || (cand.exact == best->exact && cand.score > best->score))
			best = cand;
		}

	if (!best) return std::nullopt;
	if (!best->exact && best->score < MB_MIN_SCORE) return std::nullopt;
	return best;
	}

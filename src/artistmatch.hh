#pragma once

#include <optional>
#include <string>

// Turning an artist folder's name into a MusicBrainz artist id.
//
// Pure string and JSON work: no network, no database, no MediaStore.  That is
// the shape tmdb_pick() has, and for the reason its comment gives - this is
// where a wrong answer comes from, so it is the part worth being able to
// exercise directly.  --artist-pick-test pipes a canned search response
// through it and tests/test_artist_pick.py is a regression table over the
// rules.
//
// The fact the whole file exists for: **MusicBrainz's `artist` search field
// holds an artist's primary name and nothing else.**  Every other spelling -
// a romanization, a stage name, a translation - is in the `alias` field.  So
// a folder called "Hiromi Uehara" scores zero against artist:"Hiromi Uehara",
// because her primary name is 上原ひろみ and the romanization everyone files
// her under is an alias.  Searching only the one field made every such artist
// unresolvable, and since the search still returned HTTP 200 the empty result
// was cached as an answer.

// A candidate from a MusicBrainz artist search.
struct MbArtistMatch
	{
	std::string mbid;
	std::string name;         // MusicBrainz's primary name, not what was asked
	int         score = 0;    // MusicBrainz's own 0..100 relevance
	bool        exact = false;  // the name or one of its aliases *is* the query
	};

// One string, safe to drop between the quotes of a Lucene phrase.
//
// **Only \ and " need this, and that is measured rather than assumed.**
// Everything else is literal inside a quoted phrase and the search analyzer
// drops the punctuation anyway: against the live service, a title holding a
// comma, a colon, a slash, an exclamation mark or a bracket searches
// identically escaped and unescaped, which is why "AC/DC" needs no special
// case.  A double quote is the one that is genuinely query syntax -- it closes
// the phrase and has the rest of the name parsed as operators.
//
// So this is not what makes an album resolve; strip_album_decoration() in
// gaindrive.cc is.  It is here so that neither query can be malformed.
std::string mb_escape_phrase(const std::string& s);

// The search query for one artist name, covering both fields:
//
//     artist:"<name>" OR alias:"<name>"
//
// The name is escaped with mb_escape_phrase(), which the caller must not do
// again.
std::string mb_artist_query(const std::string& name);

// Which of a search's results, if any, is the artist that was asked for.
// `body` is a /ws/2/artist search response.
//
// **Exactness outranks MusicBrainz's score, and that is the rule.**  A search
// ranks a name that *contains* the query above one that merely lists it as an
// alias, so for "Ryuichi Sakamoto" the top hit at score 100 is the duo "Alva
// Noto + Ryuichi Sakamoto" while the man himself is second at 80, under his
// primary name 坂本龍一.  Taking the first hit is how a folder acquires a
// collaboration's biography, permanently and with nothing downstream able to
// tell.  Score still breaks ties *among* exact matches, which is what keeps
// "Pink Floyd" on Pink Floyd rather than on the unrelated artist carrying it
// as a junk alias.  Diacritics are folded before comparing, which is what
// finds 坂本龍一 through his "Ryūichi Sakamoto" alias for a folder spelled
// without the macron -- MusicBrainz's own search folds them, which is why he
// is among the results at all.
//
// With no exact match at all the top hit is taken only if it reaches
// MB_MIN_SCORE, and otherwise nothing is: a wrong id is worse than none,
// because everything downstream - biography, portrait, Last.fm link - is
// confidently attributed to whoever it named.
std::optional<MbArtistMatch> mb_pick_artist(const std::string& body,
                                             const std::string& name);

// Comparison key for "do these two strings name the same artist".  Deliberately
// loose about case, punctuation and underscores: an artist folder is a
// *filename*, so "AC/DC" is on disk as "AC-DC" and "Pink_Floyd" is a legal
// spelling of Pink Floyd.
//
// Every byte from 0x80 up is copied through untouched, and that is the part to
// leave alone: classifying a UTF-8 continuation byte with isalnum() and
// dropping it would make "Sigur Rós" key as "sigur rs" while a folder spelled
// the same way keys as "sigur rs" too -- fine -- but "Sigur Ros" would then
// match it as well, and worse, a name written entirely in a non-Latin script
// would key as the empty string, so two unrelated artists would compare equal
// and neither would ever show its own name.  Accents survive on every
// filesystem that matters, so an exact comparison outside ASCII is right, and
// it is what lets this avoid an ICU dependency.
//
// Matching a *provider's* data is the other case and wants the opposite, which
// is why mb_pick_artist() folds diacritics before applying this rather than
// changing it: see fold_diacritics() in artistmatch.cc.
//
// No article stripping ("The Beatles" against "Beatles").  The two failure
// directions are not symmetric: over-normalising hides a real difference, which
// is invisible and unreportable, while under-normalising shows a redundant line
// that anyone can see and fix in the tag or the folder name.  ignoredArticles
// exists for *sorting*, which may guess; identity may not.
std::string artist_key(const std::string& s);

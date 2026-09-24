package org.gaindrive.android.data.model

/**
 * Which order an artist's albums are listed in.
 *
 * [YEAR] is what the server answers with - `get_artist()` in `src/mediastore.cc`
 * ends its query `ORDER BY al.year, al.title COLLATE NOCASE` - and is the
 * default here for that reason. It is right for a discography and useless for a
 * film category, where the only thing anyone knows about an item is its name.
 *
 * Stored by [name], like [AudioQuality]'s tag, so a value written by an older
 * build still parses and an unknown one falls back rather than throwing.
 */
enum class AlbumSort {
	YEAR,
	NAME;

	/** For the menu row. */
	val label: String
		get() = when (this) {
			YEAR -> "Year"
			NAME -> "Name"
		}

	companion object {
		val DEFAULT = YEAR

		fun parse(name: String?): AlbumSort =
			entries.firstOrNull { it.name == name } ?: DEFAULT
	}
}

/**
 * A total order, so the tie-break is never left to which server answered first.
 *
 * The year arm reproduces the server's own `ORDER BY` - an album with no year
 * sorts first, as it does under SQLite, where the column is NULL rather than
 * zero. `CASE_INSENSITIVE_ORDER` stands in for `COLLATE NOCASE`; it folds more
 * than SQLite's ASCII-only rule does, which for a title list is the better
 * answer rather than a discrepancy worth reproducing.
 *
 * Sorting is worth doing even under [AlbumSort.YEAR], which is what the server
 * already answered with: a merged artist's albums arrive as one server's list
 * concatenated with another's - `mergeAlbums` keeps arrival order - so the union
 * was never in year order at all.
 */
val AlbumSort.comparator: Comparator<Album>
	get() = when (this) {
		AlbumSort.YEAR ->
			compareBy<Album> { it.year ?: 0 }
				.thenBy(String.CASE_INSENSITIVE_ORDER) { it.title }
		AlbumSort.NAME ->
			compareBy<Album, String>(String.CASE_INSENSITIVE_ORDER) { it.title }
				.thenBy { it.year ?: 0 }
	}

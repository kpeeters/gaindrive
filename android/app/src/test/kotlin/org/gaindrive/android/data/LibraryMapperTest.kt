package org.gaindrive.android.data

import org.gaindrive.android.data.model.ServerId
import org.gaindrive.android.net.AlbumDto
import org.gaindrive.android.net.ArtistDto
import org.gaindrive.android.net.SongDto
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * The mapper is where an absent field turns into something renderable, and
 * where the server id gets welded onto every reference. Both are invisible
 * when they go wrong with a single server configured.
 */
class LibraryMapperTest {

	private val server = ServerId("srv-a")
	private val other = ServerId("srv-b")

	@Test
	fun `refs carry the server they came from`() {
		val song = SongDto(id = "501", title = "Peg").toDomain(server)
		assertEquals(server, song.ref.server)
		assertEquals("501", song.ref.id)
	}

	@Test
	fun `the same id on two servers maps to different refs`() {
		val a = SongDto(id = "501", title = "Peg").toDomain(server)
		val b = SongDto(id = "501", title = "Peg").toDomain(other)
		assertTrue(a.ref != b.ref)
	}

	/** getAlbum sends `name`; the directory-shaped endpoints send `title`. */
	@Test
	fun `album title comes from whichever field is present`() {
		assertEquals("Aja", AlbumDto(id = "1", name = "Aja").toDomain(server).title)
		assertEquals("Aja", AlbumDto(id = "1", title = "Aja").toDomain(server).title)
	}

	/** artistId is the ID3 field; parent is the folder equivalent. */
	@Test
	fun `artist ref prefers artistId but accepts parent`() {
		val withId = AlbumDto(id = "1", artistId = "12", parent = "99").toDomain(server)
		assertEquals("12", withId.artistRef?.id)

		val parentOnly = AlbumDto(id = "1", parent = "12").toDomain(server)
		assertEquals("12", parentOnly.artistRef?.id)

		val neither = AlbumDto(id = "1").toDomain(server)
		assertNull(neither.artistRef)
	}

	@Test
	fun `song album ref prefers albumId but accepts parent`() {
		assertEquals("77", SongDto(id = "1", albumId = "77", parent = "88")
			.toDomain(server).albumRef?.id)
		assertEquals("88", SongDto(id = "1", parent = "88").toDomain(server).albumRef?.id)
	}

	@Test
	fun `starred timestamp becomes an isStarred flag without losing the time`() {
		val starred = SongDto(id = "1", title = "x", starred = "2026-07-27T18:04:11Z")
			.toDomain(server)
		assertTrue(starred.isStarred)
		assertEquals("2026-07-27T18:04:11Z", starred.starredAt)

		val plain = SongDto(id = "1", title = "x").toDomain(server)
		assertFalse(plain.isStarred)
		assertNull(plain.starredAt)
	}

	/** Zero means "not set" for these, and must not reach the UI as "0". */
	@Test
	fun `zero year track and disc become null`() {
		val song = SongDto(id = "1", title = "x", year = 0, track = 0, discNumber = 0)
			.toDomain(server)
		assertNull(song.year)
		assertNull(song.track)
		assertNull(song.discNumber)
	}

	/**
	 * An episode carries its season alongside the disc number, which holds the
	 * same value. Null is what makes the album screen head a group "Disc"
	 * rather than "Series", so the server omitting the field — for a film, or
	 * on an endpoint that does not select it — has to arrive as null and not
	 * as 0.
	 */
	@Test
	fun `season survives, and its absence is null`() {
		val episode = SongDto(id = "1", title = "Jungles", discNumber = 2, season = 2)
			.toDomain(server)
		assertEquals(2, episode.season)
		assertEquals(2, episode.discNumber)

		val film = SongDto(id = "2", title = "The Third Man", discNumber = 1)
			.toDomain(server)
		assertNull(film.season)

		val zeroed = SongDto(id = "3", title = "x", season = 0).toDomain(server)
		assertNull(zeroed.season)
	}

	@Test
	fun `blank optional strings become null`() {
		val album = AlbumDto(id = "1", name = "x", genre = "").toDomain(server)
		assertNull(album.genre)
	}

	/** An artist's own folder id doubles as its cover art id on this server. */
	@Test
	fun `artist cover falls back to the artist id`() {
		val artist = ArtistDto(id = "12", name = "Steely Dan").toDomain(server)
		assertEquals("12", artist.coverArt?.id)
		assertEquals(server, artist.coverArt?.server)
	}

	@Test
	fun `video fields map through`() {
		val video = SongDto(
			id = "1",
			title = "The Third Man",
			isVideo = true,
			nativeSeek = true,
			originalWidth = 1920,
			originalHeight = 1080,
		).toDomain(server)

		assertTrue(video.isVideo)
		assertTrue(video.nativeSeek)
		assertEquals(1920, video.width)
		assertEquals(1080, video.height)
		assertEquals(16f / 9f, video.aspectRatio!!, 0.001f)
	}

	/**
	 * Every audio entry the server has ever sent lacks all four, and an
	 * endpoint that omits `nativeSeek` for a video must not be read as
	 * promising byte-range seeking it cannot do.
	 */
	@Test
	fun `a song with no video fields is audio that cannot be range-seeked`() {
		val song = SongDto(id = "1", title = "Peg").toDomain(server)
		assertFalse(song.isVideo)
		assertFalse(song.nativeSeek)
		assertNull(song.width)
		assertNull(song.height)
		assertNull(song.aspectRatio)
	}

	/** Unprobed videos report zero, which is not a shape. */
	@Test
	fun `zero dimensions become null rather than an aspect of zero`() {
		val video = SongDto(id = "1", title = "x", isVideo = true, originalWidth = 0)
			.toDomain(server)
		assertNull(video.width)
		assertNull(video.aspectRatio)
	}

	/** Counts are derived when the server omits them, e.g. on search results. */
	@Test
	fun `song count falls back to the songs actually present`() {
		val album = AlbumDto(
			id = "1",
			name = "x",
			song = listOf(SongDto(id = "a", title = "a"), SongDto(id = "b", title = "b")),
		).toDomain(server)
		assertEquals(2, album.songCount)
	}
}

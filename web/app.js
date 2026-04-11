'use strict';

// ── Credential storage ──────────────────────────────────────────────────────

const creds = {
   load() {
      return {
         server:   localStorage.getItem('gd_server'),
         user:     localStorage.getItem('gd_user'),
         password: localStorage.getItem('gd_password'),
      };
   },
   save(server, user, password) {
      localStorage.setItem('gd_server',   server);
      localStorage.setItem('gd_user',     user);
      localStorage.setItem('gd_password', password);
   },
   clear() {
      localStorage.removeItem('gd_server');
      localStorage.removeItem('gd_user');
      localStorage.removeItem('gd_password');
   },
};

// ── Theme ────────────────────────────────────────────────────────────────────

// Cycles: auto (system preference) → light → dark → auto.
const THEME_CYCLE = ['auto', 'light', 'dark'];

function applyTheme(theme) {
   document.documentElement.classList.remove('light', 'dark');
   if (theme !== 'auto')
      document.documentElement.classList.add(theme);

   if (theme === 'auto') localStorage.removeItem('gd_theme');
   else                  localStorage.setItem('gd_theme', theme);

   const btn = document.getElementById('theme-toggle');
   if (btn) btn.textContent = theme.charAt(0).toUpperCase() + theme.slice(1);
}

function cycleTheme() {
   const current = localStorage.getItem('gd_theme') ?? 'auto';
   const next    = THEME_CYCLE[(THEME_CYCLE.indexOf(current) + 1) % THEME_CYCLE.length];
   applyTheme(next);
}

// ── Subsonic API wrapper ────────────────────────────────────────────────────

// Build a subsonic API URL. Extra params can be passed as an object.
function apiUrl(endpoint, extra = {}) {
   const {server, user, password} = creds.load();
   const p = new URLSearchParams({
      u: user,
      p: password,
      v: '1.16.1',
      c: 'gaindrive-web',
      f: 'json',
      ...extra,
   });
   return `${server}/rest/${endpoint}.view?${p}`;
}

// Call a subsonic endpoint and return the parsed subsonic-response object.
// Throws on network error or non-ok subsonic status.
async function apiCall(endpoint, extra = {}) {
   const resp = await fetch(apiUrl(endpoint, extra));
   if (!resp.ok)
      throw new Error(`HTTP ${resp.status}`);
   const data = await resp.json();
   const sr = data['subsonic-response'];
   if (sr.status !== 'ok')
      throw new Error(sr.error?.message ?? 'Unknown error');
   return sr;
}

// ── Login ───────────────────────────────────────────────────────────────────

async function tryLogin(server, user, password) {
   // Normalise server URL: strip trailing slash.
   server = server.replace(/\/+$/, '');
   console.log('[login] attempting ping', server, user);

   const p = new URLSearchParams({
      u: user,
      p: password,
      v: '1.16.1',
      c: 'gaindrive-web',
      f: 'json',
   });
   const url = `${server}/rest/ping.view?${p}`;
   console.log('[login] fetch', url);
   const resp = await fetch(url);
   console.log('[login] HTTP status', resp.status);
   if (!resp.ok)
      throw new Error(`Server returned HTTP ${resp.status}`);

   const data = await resp.json();
   console.log('[login] response JSON', data);

   const sr = data['subsonic-response'];
   console.log('[login] subsonic-response', sr);
   if (sr.status !== 'ok')
      throw new Error(sr.error?.message ?? 'Authentication failed');

   console.log('[login] success, saving credentials');
   creds.save(server, user, password);
}

// ── Multi-pane navigation ────────────────────────────────────────────────────

// The content area is divided into three fixed panes (artists / albums / tracks)
// laid out side by side in a strip. Depending on screen width, 1, 2, or 3 panes
// are visible at once. Navigating deeper slides the strip left; going back
// slides right. On resize the layout is recalculated without animation.

const paneNav = {
   depth: 0,   // 0 = artists, 1 = albums, 2 = tracks

   _visiblePanes() {
      const w = document.getElementById('pane-viewport').offsetWidth;
      if (w >= 1100) return 3;
      if (w >= 650)  return 2;
      return 1;
      },

   _apply(animate) {
      const vp    = document.getElementById('pane-viewport');
      const strip = document.getElementById('pane-strip');
      if (!vp || !strip) return;

      const n  = this._visiblePanes();
      const pw = vp.offsetWidth / n;

      if (!animate) strip.style.transition = 'none';

      document.querySelectorAll('.pane').forEach(p => { p.style.width = pw + 'px'; });

      // Leftmost visible pane index so the current depth is always rightmost.
      const leftmost = Math.max(0, this.depth - (n - 1));
      strip.style.transform = `translateX(${-leftmost * pw}px)`;

      // Show back-link only in the leftmost pane when it isn't the root pane.
      document.querySelectorAll('.back-link').forEach(el => {
         el.hidden = (parseInt(el.dataset.pane) !== leftmost) || (leftmost === 0);
         });


      if (!animate)
         requestAnimationFrame(() => { strip.style.transition = ''; });
      },

   // Returns true only when navigating to newDepth would move the strip —
   // i.e. the leftmost visible pane index changes. Used to decide whether
   // a history entry is worth pushing.
   willSlide(newDepth) {
      const n = this._visiblePanes();
      return Math.max(0, newDepth - (n - 1)) !== Math.max(0, this.depth - (n - 1));
      },

   slideTo(depth) {
      this.depth = depth;
      this._apply(true);
      },

   // Re-layout without animation, e.g. on window resize.
   relayout() {
      this._apply(false);
      },
};

// ── Views ───────────────────────────────────────────────────────────────────

async function showView(name) {
   console.log('[view] showView', name);
   document.querySelectorAll('#sidebar a, #bottom-nav a').forEach(a => {
      a.classList.toggle('active', a.dataset.view === name);
      });

   if (name === 'artists') {
      await viewArtists();
      } else {
      document.getElementById('pane-artists').innerHTML =
         `<p style="color:var(--text-dim)">${name}</p>`;
      document.getElementById('pane-albums').innerHTML = '';
      document.getElementById('pane-tracks').innerHTML = '';
      paneNav.slideTo(0);
      }
}

async function viewArtists() {
   console.log('[artists] loading');
   const pane = document.getElementById('pane-artists');
   pane.innerHTML = '';
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   const sr = await apiCall('getArtists');
   const indexes = sr.artists?.index ?? [];
   console.log('[artists] got', indexes.reduce((n, i) => n + i.artist.length, 0), 'artists');

   const frag = document.createDocumentFragment();
   for (const index of indexes) {
      const heading = document.createElement('h2');
      heading.className = 'index-heading';
      heading.textContent = index.name;
      frag.appendChild(heading);

      for (const artist of index.artist) {
         const row = document.createElement('div');
         row.className = 'artist-row';
         row.dataset.id = artist.id;

         const name = document.createElement('span');
         name.className = 'artist-name';
         name.textContent = artist.name;

         const count = document.createElement('span');
         count.className = 'artist-albums';
         count.textContent = artist.albumCount === 1
            ? '1 album' : `${artist.albumCount} albums`;

         row.appendChild(name);
         row.appendChild(count);
         row.addEventListener('click', () => {
            document.querySelectorAll('#pane-artists .artist-row.selected')
               .forEach(r => r.classList.remove('selected'));
            row.classList.add('selected');
            if (paneNav.willSlide(1))
               history.pushState({view: 'albums', artistId: artist.id, artistName: artist.name}, '');
            viewAlbums(artist.id, artist.name);
            });
         frag.appendChild(row);
         }
      }
   pane.appendChild(frag);
   paneNav.slideTo(0);
}

async function viewAlbums(artistId, artistName) {
   console.log('[albums] loading artist', artistId, artistName);
   const pane = document.getElementById('pane-albums');
   pane.innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   const srArtist = await apiCall('getArtist', {id: artistId});
   const albums   = srArtist.artist?.album ?? [];
   console.log('[albums] got', albums.length, 'albums');

   // Back link + artist heading.
   const header = document.createElement('div');
   header.className = 'view-header';
   const back = document.createElement('span');
   back.className = 'back-link';
   back.dataset.pane = '1';
   back.textContent = '← Artists';
   back.addEventListener('click', () => history.back());
   const heading = document.createElement('h1');
   heading.className = 'view-title';
   heading.textContent = artistName;
   header.appendChild(back);
   header.appendChild(heading);
   pane.appendChild(header);

   // Placeholder filled asynchronously once getArtistInfo2 responds.
   // The loading class reserves the same height as the collapsed bio block so
   // the album list does not jump when the bio arrives.
   // Kept as a detached-node reference so a stale callback can't corrupt a
   // pane that has already been reused for a different artist.
   const bioSlot = document.createElement('div');
   bioSlot.className = 'artist-bio-loading';
   pane.appendChild(bioSlot);

   const frag = document.createDocumentFragment();
   for (const album of albums) {
      const row = document.createElement('div');
      row.className = 'album-row';
      row.dataset.id = album.id;

      const cover = document.createElement('img');
      cover.className = 'album-cover';
      cover.width  = 80;
      cover.height = 80;
      cover.alt    = '';
      if (album.coverArt)
         cover.src = apiUrl('getCoverArt', {id: album.coverArt, size: 80});

      const info = document.createElement('div');
      info.className = 'album-info';

      const title = document.createElement('span');
      title.className = 'album-title';
      title.textContent = album.title;

      const meta = document.createElement('span');
      meta.className = 'album-meta';
      const parts = [];
      if (album.year)      parts.push(album.year);
      if (album.songCount) parts.push(`${album.songCount} tracks`);
      meta.textContent = parts.join(' · ');

      info.appendChild(title);
      info.appendChild(meta);
      row.appendChild(cover);
      row.appendChild(info);
      row.addEventListener('click', () => {
         document.querySelectorAll('#pane-albums .album-row.selected')
            .forEach(r => r.classList.remove('selected'));
         row.classList.add('selected');
         if (paneNav.willSlide(2))
            history.pushState({view: 'tracks', albumId: album.id, albumTitle: album.title, artistId, artistName}, '');
         viewTracks(album.id, album.title, artistId, artistName);
         });
      frag.appendChild(row);
      }
   pane.appendChild(frag);
   paneNav.slideTo(1);

   // Fetch artist info without blocking the album list.
   apiCall('getArtistInfo2', {id: artistId}).then(srInfo => {
      const info        = srInfo?.artistInfo2 ?? {};
      const imgUrl      = info.largeImageUrl || info.mediumImageUrl || info.smallImageUrl || '';
      const bio         = info.biography ?? '';
      const wikiUrl     = info.wikiUrl ?? '';
      const allMusicUrl = info.allMusicUrl ?? '';

      bioSlot.className = '';   // remove shimmer regardless of outcome

      if (!imgUrl && !bio && !wikiUrl && !allMusicUrl) return;

      const block = document.createElement('div');
      block.className = 'artist-bio';

      if (imgUrl) {
         const img = document.createElement('img');
         img.className = 'artist-bio-img';
         img.src = imgUrl;
         img.alt = artistName;
         block.appendChild(img);
         }

      let bioP = null;
      if (bio) {
         const body = document.createElement('div');
         body.className = 'artist-bio-body';
         bioP = document.createElement('p');
         bioP.className = 'artist-bio-text';
         bioP.innerHTML = bio;   // Last.fm-supplied HTML
         body.appendChild(bioP);

         // Links + 'more' toggle below the description, inside the text column.
         const linksRow = document.createElement('div');
         linksRow.className = 'links-row';
         if (wikiUrl) {
            const a = document.createElement('a');
            a.className = 'wiki-link';
            a.href = wikiUrl;
            a.target = '_blank';
            a.rel = 'noopener';
            a.textContent = 'Wikipedia';
            linksRow.appendChild(a);
            }
         if (allMusicUrl) {
            const a = document.createElement('a');
            a.className = 'wiki-link';
            a.href = allMusicUrl;
            a.target = '_blank';
            a.rel = 'noopener';
            a.textContent = 'AllMusic';
            linksRow.appendChild(a);
            }
         const toggle = document.createElement('span');
         toggle.className = 'bio-toggle';
         toggle.textContent = 'more';
         toggle.addEventListener('click', () => {
            const expanded = bioP.classList.toggle('expanded');
            toggle.textContent = expanded ? 'less' : 'more';
            });
         linksRow.appendChild(toggle);
         // Hide the toggle if the text fits without clamping.
         requestAnimationFrame(() => {
            if (bioP.scrollHeight <= bioP.clientHeight) toggle.hidden = true;
            });
         body.appendChild(linksRow);
         block.appendChild(body);
         }
      else if (wikiUrl || allMusicUrl) {
         // No bio text — just show the links directly on the block.
         const linksRow = document.createElement('div');
         linksRow.className = 'links-row';
         if (wikiUrl) {
            const a = document.createElement('a');
            a.className = 'wiki-link';
            a.href = wikiUrl;
            a.target = '_blank';
            a.rel = 'noopener';
            a.textContent = 'Wikipedia';
            linksRow.appendChild(a);
            }
         if (allMusicUrl) {
            const a = document.createElement('a');
            a.className = 'wiki-link';
            a.href = allMusicUrl;
            a.target = '_blank';
            a.rel = 'noopener';
            a.textContent = 'AllMusic';
            linksRow.appendChild(a);
            }
         block.appendChild(linksRow);
         }

      bioSlot.appendChild(block);
      }).catch(() => { bioSlot.className = ''; });   // server may not support getArtistInfo2
}

function fmtDuration(secs) {
   const m = Math.floor(secs / 60);
   const s = String(secs % 60).padStart(2, '0');
   return `${m}:${s}`;
}

// ── Player ───────────────────────────────────────────────────────────────────

// Id of the currently active cast device, or null when not casting.
let castDeviceId = null;
let castPollTimer = null;
let castPolling   = false;   // prevent overlapping polls

async function pollCastStatus() {
   if (castPolling) return;
   castPolling = true;
   try {
      const sr = await apiCall('getCastStatus');
      const s  = sr.castStatus;
      if (!s || s.playerState === 'IDLE') return;
      const seek = document.getElementById('player-seek');
      const time = document.getElementById('player-time');
      if (!seek.dataset.seeking) {
         seek.max   = Math.floor(s.duration);
         seek.value = Math.floor(s.currentTime);
         }
      time.textContent =
         `${fmtDuration(Math.floor(s.currentTime))} / ${fmtDuration(Math.floor(s.duration))}`;
      document.getElementById('player-playpause').textContent =
         s.playerState === 'PAUSED' ? '▶' : '⏸';
      } catch (_) {
      // best-effort; don't spam the log on transient failures
      } finally {
      castPolling = false;
      }
   }

async function openCastModal() {
   const modal   = document.getElementById('cast-modal');
   const list    = document.getElementById('cast-device-list');
   const stopRow = document.getElementById('cast-stop-row');

   list.textContent = 'Discovering…';
   stopRow.classList.toggle('hidden', castDeviceId === null);
   modal.classList.remove('hidden');

   try {
      const sr      = await apiCall('listCastDevices');
      const devices = sr.castDevices ?? [];
      list.textContent = '';
      if (devices.length === 0) {
         list.textContent = 'No devices found.';
         return;
         }
      for (const dev of devices) {
         const btn = document.createElement('button');
         btn.textContent = dev.name;
         btn.addEventListener('click', () => selectCastDevice(dev.id, dev.name));
         list.appendChild(btn);
         }
      } catch (err) {
      list.textContent = `Error: ${err.message}`;
      }
   }

async function selectCastDevice(id) {
   try {
      await apiCall('startCast', {id});
      castDeviceId = id;
      document.getElementById('player-cast').classList.add('active');
      document.getElementById('cast-modal').classList.add('hidden');
      // Poll the Chromecast for playback position and state.
      castPollTimer = setInterval(pollCastStatus, 500);
      } catch (err) {
      alert(`Cast failed: ${err.message}`);
      }
   }

async function stopCast() {
   if (castPollTimer !== null) {
      clearInterval(castPollTimer);
      castPollTimer = null;
      }
   try {
      await apiCall('stopCast');
      } catch (_) {
      // best-effort stop
      }
   castDeviceId = null;
   document.getElementById('player-cast').classList.remove('active');
   document.getElementById('cast-modal').classList.add('hidden');
   }

const player = {
   audio: new Audio(),
   queue: [],   // song objects from getAlbum
   index: -1,   // current position in queue
   autoFrom: 0, // entries at indices >= autoFrom were auto-generated by playerLoad
   albumCtx: null, // {albumId, albumTitle, artistId, artistName} of the loaded album
};

function playerLoad(songs, startIndex) {
   document.querySelectorAll('.track-row.queued').forEach(r => r.classList.remove('queued'));
   player.queue    = [...songs];
   player.index    = startIndex;
   player.autoFrom = startIndex + 1;   // everything after current track is auto
   playerPlay();
}

// Append song to the queue after trimming any auto-generated tail.
function playerEnqueue(song) {
   if (player.autoFrom < player.queue.length)
      player.queue.splice(player.autoFrom);
   player.queue.push(song);
   player.autoFrom = player.queue.length;   // new entry is manual; no auto tail
}

function playerPlay() {
   const song = player.queue[player.index];
   if (!song) return;
   player.audio.src = apiUrl('stream', {id: song.id});
   player.audio.play().catch(err => console.warn('[player] play failed', err));
   playerUpdateUI();
}

function playerUpdateUI() {
   const song = player.queue[player.index];
   if (!song) return;

   document.getElementById('player-title').textContent  = song.title;
   document.getElementById('player-artist').textContent = song.artist ?? '';

   const cover = document.getElementById('player-cover');
   cover.src = song.coverArt ? apiUrl('getCoverArt', {id: song.coverArt, size: 64}) : '';

   // Highlight active row in track list if it is currently visible.
   document.querySelector('.track-row.playing')?.classList.remove('playing');
   const activeRow = document.querySelector(`.track-row[data-id="${song.id}"]`);
   activeRow?.classList.remove('queued');
   activeRow?.classList.add('playing');

   if ('mediaSession' in navigator) {
      navigator.mediaSession.metadata = new MediaMetadata({
         title:   song.title,
         artist:  song.artist ?? '',
         artwork: song.coverArt
            ? [{src: apiUrl('getCoverArt', {id: song.coverArt, size: 256}), sizes: '256x256'}]
            : [],
         });
      }
}

// Click cover art in the player bar to navigate to the album's track listing.
document.getElementById('player-cover').addEventListener('click', () => {
   const ctx = player.albumCtx;
   if (!ctx) return;
   history.pushState({view: 'tracks', albumId: ctx.albumId, albumTitle: ctx.albumTitle, artistId: ctx.artistId, artistName: ctx.artistName}, '');
   viewTracks(ctx.albumId, ctx.albumTitle, ctx.artistId, ctx.artistName);
   });

// Auto-advance to next track.
player.audio.addEventListener('ended', () => {
   if (player.index < player.queue.length - 1) {
      player.index++;
      playerPlay();
      }
   });

// Keep seek bar and time display in sync while playing.
player.audio.addEventListener('timeupdate', () => {
   const seek = document.getElementById('player-seek');
   const time = document.getElementById('player-time');
   const cur  = player.audio.currentTime;
   const dur  = player.audio.duration || 0;
   if (!seek.dataset.seeking) {
      seek.max   = Math.floor(dur);
      seek.value = Math.floor(cur);
      }
   time.textContent = `${fmtDuration(Math.floor(cur))} / ${fmtDuration(Math.floor(dur))}`;
   });

player.audio.addEventListener('play',  () => {
   document.getElementById('player-playpause').textContent = '⏸';
   });
player.audio.addEventListener('pause', () => {
   document.getElementById('player-playpause').textContent = '▶';
   });

// Wire control buttons and MediaSession handlers. Called once from showShell().
function setupPlayer() {
   document.getElementById('player-playpause').addEventListener('click', () => {
      if (castDeviceId !== null) {
         const btn = document.getElementById('player-playpause');
         // Flip immediately so the UI responds without waiting for the next poll.
         if (btn.textContent === '▶') {
            btn.textContent = '⏸';
            apiCall('castControl', {action: 'play'}).catch(() => {});
            } else {
            btn.textContent = '▶';
            apiCall('castControl', {action: 'pause'}).catch(() => {});
            }
         return;
         }
      if (player.audio.paused) player.audio.play();
      else                     player.audio.pause();
      });

   // Restart if more than 3 s in, otherwise go to previous track (standard UX).
   document.getElementById('player-prev').addEventListener('click', () => {
      if (player.audio.currentTime > 3) {
         player.audio.currentTime = 0;
         } else if (player.index > 0) {
         player.index--;
         playerPlay();
         }
      });

   document.getElementById('player-next').addEventListener('click', () => {
      if (player.index < player.queue.length - 1) {
         player.index++;
         playerPlay();
         }
      });

   // Prevent the seek bar from jumping while the user is dragging it.
   const seek = document.getElementById('player-seek');
   seek.addEventListener('mousedown',  () => { seek.dataset.seeking = '1'; });
   seek.addEventListener('touchstart', () => { seek.dataset.seeking = '1'; });
   seek.addEventListener('change', () => {
      if (castDeviceId !== null) {
         apiCall('castControl', {action: 'seek', time: seek.value}).catch(() => {});
         } else {
         player.audio.currentTime = Number(seek.value);
         }
      delete seek.dataset.seeking;
      });

   document.getElementById('player-cast').addEventListener('click', openCastModal);
   document.getElementById('cast-close-btn').addEventListener('click', () => {
      document.getElementById('cast-modal').classList.add('hidden');
      });
   document.getElementById('cast-stop-btn').addEventListener('click', stopCast);

   if ('mediaSession' in navigator) {
      navigator.mediaSession.setActionHandler('play',          () => player.audio.play());
      navigator.mediaSession.setActionHandler('pause',         () => player.audio.pause());
      navigator.mediaSession.setActionHandler('previoustrack', () => {
         document.getElementById('player-prev').click();
         });
      navigator.mediaSession.setActionHandler('nexttrack',     () => {
         document.getElementById('player-next').click();
         });
      navigator.mediaSession.setActionHandler('seekto', details => {
         player.audio.currentTime = details.seekTime;
         });
      }
}

async function viewTracks(albumId, albumTitle, artistId, artistName, autoPlayId = null) {
   console.log('[tracks] loading album', albumId, albumTitle);
   const pane = document.getElementById('pane-tracks');
   pane.innerHTML = '';

   const sr = await apiCall('getAlbum', {id: albumId});
   const album = sr.album ?? {};
   const songs = album.song ?? [];
   console.log('[tracks] got', songs.length, 'tracks');

   // Back link + headings + Edit link.
   const header = document.createElement('div');
   header.className = 'view-header';
   const back = document.createElement('span');
   back.className = 'back-link';
   back.dataset.pane = '2';
   back.textContent = `← ${artistName}`;
   back.addEventListener('click', () => history.back());
   const heading = document.createElement('h1');
   heading.className = 'view-title';
   heading.textContent = albumTitle;
   const editLink = document.createElement('span');
   editLink.className = 'edit-link';
   editLink.textContent = 'Edit';
   header.appendChild(back);
   header.appendChild(heading);
   header.appendChild(editLink);
   pane.appendChild(header);

   // Large cover art hero with carousel support for extra images.
   let heroImg = null;
   let carouselIdx = 0;
   let carouselCount = 1;
   const heroWrap = document.createElement('div');
   heroWrap.className = 'cover-hero-wrap';
   if (album.coverArt) {
      const prevBtn = document.createElement('button');
      prevBtn.className = 'carousel-btn carousel-prev';
      prevBtn.setAttribute('aria-label', 'Previous image');
      prevBtn.textContent = '\u2039';
      prevBtn.style.display = 'none';

      heroImg = document.createElement('img');
      heroImg.className = 'album-hero';
      heroImg.src = apiUrl('getCoverArt', {id: album.coverArt, size: 400});
      heroImg.alt = albumTitle;

      const nextBtn = document.createElement('button');
      nextBtn.className = 'carousel-btn carousel-next';
      nextBtn.setAttribute('aria-label', 'Next image');
      nextBtn.textContent = '\u203a';
      nextBtn.style.display = 'none';

      heroWrap.appendChild(prevBtn);
      heroWrap.appendChild(heroImg);
      heroWrap.appendChild(nextBtn);

      function showCarouselImage(idx) {
         carouselIdx = idx;
         heroImg.src = apiUrl('getCoverArt', {id: album.coverArt, size: 400, index: idx});
         prevBtn.disabled = idx === 0;
         nextBtn.disabled = idx === carouselCount - 1;
         }

      prevBtn.addEventListener('click', () => { if (carouselIdx > 0) showCarouselImage(carouselIdx - 1); });
      nextBtn.addEventListener('click', () => { if (carouselIdx < carouselCount - 1) showCarouselImage(carouselIdx + 1); });

      // Fetch image count; show arrows only when there are multiple images.
      apiCall('getAlbumImages', {id: album.coverArt}).then(data => {
         carouselCount = data.albumImages?.count ?? 1;
         if (carouselCount > 1) {
            prevBtn.style.display = '';
            nextBtn.style.display = '';
            prevBtn.disabled = true;   // start at index 0
            }
         });
      }
   pane.appendChild(heroWrap);

   // Placeholder for album notes + Wikipedia link, filled async.
   const infoSlot = document.createElement('div');
   infoSlot.className = 'album-notes-loading';
   pane.appendChild(infoSlot);

   const frag = document.createDocumentFragment();
   const multiDisc = new Set(songs.map(s => s.discNumber ?? 1)).size > 1;
   // If every track number is 0 or 1 the tags are useless; number sequentially.
   const useSeq = songs.every(s => (s.track ?? 0) <= 1);
   let currentDisc = null;

   for (let i = 0; i < songs.length; i++) {
      const song = songs[i];
      const disc = song.discNumber ?? 1;
      if (multiDisc && disc !== currentDisc) {
         currentDisc = disc;
         const dh = document.createElement('div');
         dh.className = 'disc-heading';
         dh.textContent = `Disc ${disc}`;
         frag.appendChild(dh);
         }

      const row = document.createElement('div');
      row.className = 'track-row';
      row.dataset.id   = song.id;
      row.dataset.year = song.year ?? '';
      row.dataset.dur  = song.duration ? fmtDuration(song.duration) : '';

      const icon = document.createElement('span');
      icon.className = 'track-icon';

      const num = document.createElement('span');
      num.className = 'track-num';
      num.textContent = useSeq ? (i + 1) : (song.track ?? '');

      const title = document.createElement('span');
      title.className = 'track-title';
      title.textContent = song.title;

      const dur = document.createElement('span');
      dur.className = 'track-dur';
      dur.textContent = row.dataset.dur;

      icon.addEventListener('click', e => {
         e.stopPropagation();
         playerEnqueue(songs[i]);
         row.classList.add('queued');
         });
      row.addEventListener('click', () => {
         if (row.classList.contains('editing')) return;
         player.albumCtx = {albumId, albumTitle, artistId, artistName};
         playerLoad(songs, i);
         });
      row.appendChild(icon);
      row.appendChild(num);
      row.appendChild(title);
      row.appendChild(dur);
      frag.appendChild(row);
      }

   pane.appendChild(frag);
   paneNav.slideTo(2);

   if (autoPlayId !== null) {
      const idx = songs.findIndex(s => s.id === autoPlayId);
      if (idx !== -1) {
         player.albumCtx = {albumId, albumTitle, artistId, artistName};
         playerLoad(songs, idx);
         }
      }

   // ── Edit mode ──────────────────────────────────────────────────────────────
   // Toggled by the "Edit" link in the header.
   let pendingCoverFile = null;  // File object selected by the picker, or null

   function enterEditMode() {
      // Swap "Edit" link for Save + Cancel buttons.
      editLink.textContent = '';
      const saveBtn   = document.createElement('button');
      saveBtn.className = 'edit-save-btn';
      saveBtn.textContent = 'Save';
      const cancelBtn = document.createElement('button');
      cancelBtn.className = 'edit-cancel-btn';
      cancelBtn.textContent = 'Cancel';
      editLink.appendChild(saveBtn);
      editLink.appendChild(cancelBtn);

      // Add pencil button over the cover art.
      const fileInput = document.createElement('input');
      fileInput.type = 'file';
      fileInput.accept = 'image/*';
      fileInput.hidden = true;
      heroWrap.appendChild(fileInput);

      const pencilBtn = document.createElement('button');
      pencilBtn.className = 'cover-edit-btn';
      pencilBtn.textContent = '✏';
      pencilBtn.setAttribute('aria-label', 'Change cover art');
      pencilBtn.addEventListener('click', () => fileInput.click());
      heroWrap.appendChild(pencilBtn);

      fileInput.addEventListener('change', () => {
         const file = fileInput.files[0];
         if (!file) return;
         pendingCoverFile = file;
         // Preview immediately; create img if it didn't exist before.
         if (!heroImg) {
            heroImg = document.createElement('img');
            heroImg.className = 'album-hero';
            heroImg.alt = albumTitle;
            heroWrap.insertBefore(heroImg, pencilBtn);
            }
         const reader = new FileReader();
         reader.onload = e => { heroImg.src = e.target.result; };
         reader.readAsDataURL(file);
         });

      // Replace track num/title spans with inputs; swap dur span for year input.
      pane.querySelectorAll('.track-row').forEach(row => {
         const numSpan   = row.querySelector('.track-num');
         const titleSpan = row.querySelector('.track-title');
         const durSpan   = row.querySelector('.track-dur');

         const numInput = document.createElement('input');
         numInput.type = 'number';
         numInput.min  = '0';
         numInput.className = 'track-num-input';
         numInput.value = numSpan.textContent;
         numInput.dataset.orig = numSpan.textContent;

         const titleInput = document.createElement('input');
         titleInput.type = 'text';
         titleInput.className = 'track-title-input';
         titleInput.value = titleSpan.textContent;
         titleInput.dataset.orig = titleSpan.textContent;

         const yearInput = document.createElement('input');
         yearInput.type = 'number';
         yearInput.min  = '0';
         yearInput.max  = '9999';
         yearInput.className = 'track-year-input';
         yearInput.value = row.dataset.year;
         yearInput.dataset.orig = row.dataset.year;

         row.replaceChild(numInput,   numSpan);
         row.replaceChild(titleInput, titleSpan);
         row.replaceChild(yearInput,  durSpan);

         // Prevent row click (play) while editing.
         row.classList.add('editing');
         });

      // 'all' button: copies the first track's year to all others on the same disc.
      function makeAllBtn(discHeading) {
         const btn = document.createElement('button');
         btn.className = 'year-all-btn';
         btn.textContent = 'all';
         btn.addEventListener('click', () => {
            // Walk forward from discHeading (or start of list) until the next disc.
            let el = discHeading ? discHeading.nextElementSibling
                                 : pane.querySelector('.track-row');
            let firstYear = null;
            while (el && !el.classList.contains('disc-heading')) {
               if (el.classList.contains('track-row')) {
                  const yi = el.querySelector('.track-year-input');
                  if (yi) {
                     if (firstYear === null) firstYear = yi.value;
                     else yi.value = firstYear;
                     }
                  }
               el = el.nextElementSibling;
               }
            });
         return btn;
         }

      const discHeadings = [...pane.querySelectorAll('.disc-heading')];
      if (discHeadings.length > 0) {
         // Multi-disc: add one 'all' button into each disc heading line.
         discHeadings.forEach(dh => {
            dh.classList.add('editing');
            dh.appendChild(makeAllBtn(dh));
            });
         }
      else {
         // Single disc: small header row above the track list.
         const editHeader = document.createElement('div');
         editHeader.className = 'track-edit-header';
         editHeader.appendChild(makeAllBtn(null));
         const firstTrackEl = pane.querySelector('.track-row');
         if (firstTrackEl) pane.insertBefore(editHeader, firstTrackEl);
         else pane.appendChild(editHeader);
         }

      saveBtn.addEventListener('click', async () => {
         saveBtn.disabled = true;
         cancelBtn.disabled = true;

         // Save cover art first, if changed.
         if (pendingCoverFile) {
            try {
               const {server, user, password} = creds.load();
               const p = new URLSearchParams({u: user, p: password,
                  v: '1.16.1', c: 'gaindrive-web', f: 'json', id: albumId});
               const fd = new FormData();
               fd.append('file', pendingCoverFile);
               const resp = await fetch(`${server}/rest/setCoverArt.view?${p}`,
                  {method: 'POST', body: fd});
               if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
               }
            catch (e) {
               console.error('[edit] cover upload failed', e);
               }
            }

         // Save changed tracks.
         for (const row of pane.querySelectorAll('.track-row')) {
            const songId     = row.dataset.id;
            const numInput   = row.querySelector('.track-num-input');
            const titleInput = row.querySelector('.track-title-input');
            const yearInput  = row.querySelector('.track-year-input');
            if (!numInput || !titleInput || !yearInput) continue;

            const params = {id: songId};
            let changed = false;
            if (numInput.value !== numInput.dataset.orig) {
               params.track = numInput.value;
               changed = true;
               }
            if (titleInput.value !== titleInput.dataset.orig) {
               params.title = titleInput.value;
               changed = true;
               }
            if (yearInput.value !== yearInput.dataset.orig) {
               params.year = yearInput.value;
               changed = true;
               }
            if (changed) {
               try { await apiCall('updateSong', params); }
               catch (e) { console.error('[edit] updateSong failed', e); }
               }
            }

         exitEditMode(true);
         });

      cancelBtn.addEventListener('click', () => {
         pendingCoverFile = null;
         exitEditMode(false);
         });
      }

   function exitEditMode(keepValues) {
      // Restore Edit link.
      editLink.textContent = 'Edit';

      // Remove pencil button and hidden file input from heroWrap.
      heroWrap.querySelector('.cover-edit-btn')?.remove();
      heroWrap.querySelector('input[type=file]')?.remove();

      // If cancelled, restore original cover state.
      if (!keepValues) {
         if (album.coverArt && heroImg) {
            heroImg.src = apiUrl('getCoverArt', {id: album.coverArt, size: 400});
            }
         else if (!album.coverArt && heroImg) {
            // A new image was previewed but the upload was cancelled — remove it.
            heroImg.remove();
            heroImg = null;
            }
         }

      pane.querySelector('.track-edit-header')?.remove();
      pane.querySelectorAll('.disc-heading.editing').forEach(dh => {
         dh.classList.remove('editing');
         dh.querySelector('.year-all-btn')?.remove();
         });

      // Replace inputs back to spans; restore dur column.
      pane.querySelectorAll('.track-row').forEach(row => {
         const numInput   = row.querySelector('.track-num-input');
         const titleInput = row.querySelector('.track-title-input');
         const yearInput  = row.querySelector('.track-year-input');
         if (!numInput || !titleInput || !yearInput) return;

         const numSpan = document.createElement('span');
         numSpan.className = 'track-num';
         numSpan.textContent = keepValues ? numInput.value : numInput.dataset.orig;

         const titleSpan = document.createElement('span');
         titleSpan.className = 'track-title';
         titleSpan.textContent = keepValues ? titleInput.value : titleInput.dataset.orig;

         const durSpan = document.createElement('span');
         durSpan.className = 'track-dur';
         durSpan.textContent = row.dataset.dur;

         // Update stored year if saved, so re-entering edit mode shows new value.
         if (keepValues) row.dataset.year = yearInput.value;

         row.replaceChild(numSpan,   numInput);
         row.replaceChild(titleSpan, titleInput);
         row.replaceChild(durSpan,   yearInput);
         row.classList.remove('editing');
         });
      }

   editLink.addEventListener('click', e => {
      // Only fire on the link itself, not the Save/Cancel children.
      if (e.target === editLink) enterEditMode();
      });

   // Re-apply the playing highlight if a track from this album is active.
   playerUpdateUI();

   // Fetch album notes without blocking the track listing.
   apiCall('getAlbumInfo2', {id: albumId}).then(srInfo => {
      const info        = srInfo?.albumInfo2 ?? {};
      infoSlot.className = '';   // remove shimmer regardless of outcome

      const notes       = info.notes       ?? '';
      const wikiUrl     = info.wikiUrl     ?? '';
      const allMusicUrl = info.allMusicUrl ?? '';
      if (!notes && !wikiUrl && !allMusicUrl) return;

      const block = document.createElement('div');
      block.className = 'album-notes';

      let notesP = null;
      if (notes) {
         notesP = document.createElement('p');
         notesP.className = 'artist-bio-text';   // reuse same clamp style
         notesP.textContent = notes;
         block.appendChild(notesP);
         }

      // Links + 'more' toggle on one line.
      const linksRow = document.createElement('div');
      linksRow.className = 'links-row';
      if (wikiUrl) {
         const a = document.createElement('a');
         a.className = 'wiki-link';
         a.href = wikiUrl;
         a.target = '_blank';
         a.rel = 'noopener';
         a.textContent = 'Wikipedia';
         linksRow.appendChild(a);
         }
      if (allMusicUrl) {
         const a = document.createElement('a');
         a.className = 'wiki-link';
         a.href = allMusicUrl;
         a.target = '_blank';
         a.rel = 'noopener';
         a.textContent = 'AllMusic';
         linksRow.appendChild(a);
         }
      if (notesP) {
         const toggle = document.createElement('span');
         toggle.className = 'bio-toggle';
         toggle.textContent = 'more';
         toggle.addEventListener('click', () => {
            const expanded = notesP.classList.toggle('expanded');
            toggle.textContent = expanded ? 'less' : 'more';
            });
         linksRow.appendChild(toggle);
         requestAnimationFrame(() => {
            if (notesP.scrollHeight <= notesP.clientHeight) toggle.hidden = true;
            });
         }
      if (linksRow.children.length > 0) block.appendChild(linksRow);

      infoSlot.appendChild(block);
      }).catch(() => { infoSlot.className = ''; });

   // Fetch liner-note text files without blocking the track list.
   apiCall('getAlbumTexts', {id: albumId}).then(srTxt => {
      const files = srTxt?.albumTexts?.textFile ?? [];
      if (files.length === 0) return;

      const section = document.createElement('div');
      section.className = 'liner-notes';

      // Header row: label + prev/next navigation (hidden when only one file).
      const nav = document.createElement('div');
      nav.className = 'liner-notes-nav';

      const label = document.createElement('span');
      label.className = 'liner-notes-label';

      const prev = document.createElement('button');
      prev.className = 'liner-notes-btn';
      prev.textContent = '‹';
      prev.setAttribute('aria-label', 'Previous text file');

      const next = document.createElement('button');
      next.className = 'liner-notes-btn';
      next.textContent = '›';
      next.setAttribute('aria-label', 'Next text file');

      if (files.length > 1) {
         nav.appendChild(prev);
         nav.appendChild(label);
         nav.appendChild(next);
         }
      else {
         nav.appendChild(label);
         }

      const body = document.createElement('div');
      body.className = 'liner-notes-body';

      section.appendChild(nav);
      section.appendChild(body);
      pane.appendChild(section);

      let current = 0;

      function loadFile(idx) {
         const name = files[idx].name;
         label.textContent = name;
         body.textContent = '';   // clear while loading
         fetch(apiUrl('getAlbumText', {id: albumId, name}))
            .then(r => r.ok ? r.text() : Promise.reject(r.status))
            .then(text => { body.textContent = text; })
            .catch(() => { body.textContent = '(could not load file)'; });
         if (files.length > 1) {
            prev.disabled = idx === 0;
            next.disabled = idx === files.length - 1;
            }
         }

      prev.addEventListener('click', () => { if (current > 0) loadFile(--current); });
      next.addEventListener('click', () => { if (current < files.length - 1) loadFile(++current); });

      loadFile(0);
      }).catch(() => {});
}

// ── Search ──────────────────────────────────────────────────────────────────

function openSearchBar() {
   document.getElementById('search-bar').classList.add('open');
   document.getElementById('search-input').focus();
}

function closeSearchBar() {
   document.getElementById('search-bar').classList.remove('open');
   document.getElementById('search-input').value = '';
   viewArtists().catch(err => console.error('[search] close error', err));
}

let _searchTimer = null;

function scheduleSearch() {
   clearTimeout(_searchTimer);
   _searchTimer = setTimeout(() => runSearch().catch(err => console.error('[search]', err)), 320);
}

async function runSearch() {
   const q = document.getElementById('search-input').value.trim();
   if (!q) {
      await viewArtists();
      return;
      }
   const wantArtists = document.getElementById('sf-artists').checked;
   const wantAlbums  = document.getElementById('sf-albums').checked;
   const wantSongs   = document.getElementById('sf-songs').checked;

   const sr = await apiCall('search3', {
      query:       q,
      artistCount: wantArtists ? 20 : 0,
      albumCount:  wantAlbums  ? 20 : 0,
      songCount:   wantSongs   ? 20 : 0,
      });
   renderSearchResults(sr.searchResult3 ?? {});
}

function renderSearchResults(res) {
   const pane = document.getElementById('pane-artists');
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';
   paneNav.slideTo(0);

   const artists = res.artist ?? [];
   const albums  = res.album  ?? [];
   const songs   = res.song   ?? [];

   const frag = document.createDocumentFragment();

   if (artists.length === 0 && albums.length === 0 && songs.length === 0) {
      const msg = document.createElement('p');
      msg.className = 'search-no-results';
      msg.textContent = 'No results found.';
      pane.replaceChildren(msg);
      return;
      }

   if (artists.length > 0) {
      const h = document.createElement('h2');
      h.className = 'index-heading';
      h.textContent = 'Artists';
      frag.appendChild(h);

      for (const artist of artists) {
         const row = document.createElement('div');
         row.className = 'artist-row';
         const name = document.createElement('span');
         name.className = 'artist-name';
         name.textContent = artist.name;
         row.appendChild(name);
         row.addEventListener('click', () => viewAlbums(artist.id, artist.name));
         frag.appendChild(row);
         }
      }

   if (albums.length > 0) {
      const h = document.createElement('h2');
      h.className = 'index-heading';
      h.textContent = 'Albums';
      frag.appendChild(h);

      for (const album of albums) {
         const row = document.createElement('div');
         row.className = 'album-row';

         const cover = document.createElement('img');
         cover.className = 'album-cover';
         cover.width  = 80;
         cover.height = 80;
         cover.alt    = '';
         if (album.coverArt)
            cover.src = apiUrl('getCoverArt', {id: album.coverArt, size: 80});

         const info = document.createElement('div');
         info.className = 'album-info';

         const title = document.createElement('span');
         title.className = 'album-title';
         title.textContent = album.title;

         const meta = document.createElement('span');
         meta.className = 'album-meta';
         const parts = [];
         if (album.artist) parts.push(album.artist);
         if (album.year)   parts.push(album.year);
         meta.textContent = parts.join(' · ');

         info.appendChild(title);
         info.appendChild(meta);
         row.appendChild(cover);
         row.appendChild(info);
         row.addEventListener('click', () => viewTracks(album.id, album.title, album.parent, album.artist));
         frag.appendChild(row);
         }
      }

   if (songs.length > 0) {
      const h = document.createElement('h2');
      h.className = 'index-heading';
      h.textContent = 'Songs';
      frag.appendChild(h);

      for (const song of songs) {
         const row = document.createElement('div');
         row.className = 'search-song-row';

         const info = document.createElement('div');
         info.className = 'search-song-info';

         const titleEl = document.createElement('span');
         titleEl.className = 'search-song-title';
         titleEl.textContent = song.title;

         const sub = document.createElement('span');
         sub.className = 'search-song-sub';
         const subParts = [];
         if (song.artist) subParts.push(song.artist);
         if (song.album)  subParts.push(song.album);
         sub.textContent = subParts.join(' · ');

         info.appendChild(titleEl);
         info.appendChild(sub);
         row.appendChild(info);

         if (song.duration) {
            const dur = document.createElement('span');
            dur.className = 'search-song-dur';
            dur.textContent = fmtDuration(song.duration);
            row.appendChild(dur);
            }

         row.addEventListener('click', () => viewTracks(song.parent, song.album, null, song.artist, song.id));
         frag.appendChild(row);
         }
      }

   pane.replaceChildren(frag);
}

function setupSearch() {
   document.getElementById('search-btn').addEventListener('click', openSearchBar);
   document.getElementById('search-btn-mobile').addEventListener('click', e => {
      e.preventDefault();
      openSearchBar();
      });
   document.getElementById('search-close').addEventListener('click', closeSearchBar);
   document.getElementById('search-input').addEventListener('input', scheduleSearch);

   for (const id of ['sf-artists', 'sf-albums', 'sf-songs'])
      document.getElementById(id).addEventListener('change', scheduleSearch);

   document.addEventListener('keydown', e => {
      const tag = document.activeElement?.tagName;
      const inInput = (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT');

      if (e.key === 'Escape') {
         if (document.getElementById('search-bar').classList.contains('open')) {
            closeSearchBar();
            e.preventDefault();
            }
         return;
         }

      if (!inInput && e.key.length === 1 && !e.ctrlKey && !e.metaKey && !e.altKey)
         openSearchBar();
      });
}

// ── Shell ───────────────────────────────────────────────────────────────────

async function showShell() {
   console.log('[shell] showing main shell');
   document.getElementById('login-screen').hidden = true;
   const shell = document.getElementById('app-shell');
   shell.hidden = false;
   console.log('[shell] app-shell hidden=', shell.hidden, 'display=', getComputedStyle(shell).display);

   setupPlayer();
   setupSearch();

   // Sync theme button label with current state and wire it up.
   const saved = localStorage.getItem('gd_theme') ?? 'auto';
   const btn   = document.getElementById('theme-toggle');
   btn.textContent = saved.charAt(0).toUpperCase() + saved.slice(1);
   btn.addEventListener('click', cycleTheme);

   // Wire up sidebar and bottom-nav links with history entries.
   shell.querySelectorAll('[data-view]').forEach(a => {
      a.addEventListener('click', e => {
         e.preventDefault();
         history.pushState({view: a.dataset.view}, '');
         showView(a.dataset.view).catch(err => console.error('[view] error', err));
         });
      });

   // Handle browser back/forward: re-render from the popped state.
   // On back navigation the target pane still has its previous content, so we
   // just slide to it. A full fetch is only needed if the pane is empty (e.g.
   // after a page refresh that landed on a deeper history entry).
   window.addEventListener('popstate', async e => {
      const s = e.state ?? {view: 'artists'};
      if (s.view === 'albums') {
         if (document.getElementById('pane-albums').children.length > 0)
            paneNav.slideTo(1);
         else
            await viewAlbums(s.artistId, s.artistName);
         } else if (s.view === 'tracks') {
         if (document.getElementById('pane-tracks').children.length > 0)
            paneNav.slideTo(2);
         else
            await viewTracks(s.albumId, s.albumTitle, s.artistId, s.artistName);
         } else {
         if (document.getElementById('pane-artists').children.length > 0)
            paneNav.slideTo(0);
         else
            await showView('artists');
         }
      });

   // Recalculate pane widths and strip offset on resize without animating.
   new ResizeObserver(() => paneNav.relayout()).observe(
      document.getElementById('pane-viewport'));

   // Record initial state so the browser can pop back to it.
   history.replaceState({view: 'artists'}, '');
   await showView('artists');
}

function showLogin() {
   console.log('[shell] showing login screen');
   document.getElementById('app-shell').hidden = true;
   document.getElementById('login-screen').hidden = false;
}

// ── Boot ────────────────────────────────────────────────────────────────────

document.getElementById('login-form').addEventListener('submit', async e => {
   e.preventDefault();
   const btn   = e.target.querySelector('button');
   const errEl = document.getElementById('login-error');
   errEl.hidden = true;
   btn.disabled = true;
   console.log('[boot] form submitted');

   try {
      await tryLogin(
         document.getElementById('server').value.trim(),
         document.getElementById('username').value.trim(),
         document.getElementById('password').value,
      );
      showShell();
   } catch (err) {
      console.error('[boot] login failed', err);
      errEl.textContent = err.message;
      errEl.hidden = false;
   } finally {
      btn.disabled = false;
   }
});

// On load: if we have saved credentials, verify them and skip the login form.
console.log('[boot] checking saved credentials');
(async () => {
   const {server, user, password} = creds.load();
   console.log('[boot] saved creds present:', !!(server && user && password));
   if (server && user && password) {
      try {
         await tryLogin(server, user, password);
         showShell();
      } catch (err) {
         console.warn('[boot] saved credentials failed, clearing', err);
         creds.clear();
         showLogin();
      }
   }
})();

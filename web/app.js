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
      const info   = srInfo?.artistInfo2 ?? {};
      const imgUrl = info.largeImageUrl || info.mediumImageUrl || info.smallImageUrl || '';
      const bio    = info.biography ?? '';

      bioSlot.className = '';   // remove shimmer regardless of outcome

      if (!imgUrl && !bio) return;

      const block = document.createElement('div');
      block.className = 'artist-bio';

      if (imgUrl) {
         const img = document.createElement('img');
         img.className = 'artist-bio-img';
         img.src = imgUrl;
         img.alt = artistName;
         block.appendChild(img);
         }

      if (bio) {
         const body = document.createElement('div');
         body.className = 'artist-bio-body';

         const p = document.createElement('p');
         p.className = 'artist-bio-text';
         p.innerHTML = bio;   // Last.fm-supplied HTML
         body.appendChild(p);

         const toggle = document.createElement('span');
         toggle.className = 'bio-toggle';
         toggle.textContent = 'more';
         toggle.addEventListener('click', () => {
            const expanded = p.classList.toggle('expanded');
            toggle.textContent = expanded ? 'less' : 'more';
            });
         body.appendChild(toggle);

         // Hide the toggle if the text fits without clamping.
         requestAnimationFrame(() => {
            if (p.scrollHeight <= p.clientHeight) toggle.hidden = true;
            });

         block.appendChild(body);
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

async function pollCastStatus() {
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
      castPollTimer = setInterval(pollCastStatus, 2000);
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
};

function playerLoad(songs, startIndex) {
   player.queue = songs;
   player.index = startIndex;
   playerPlay();
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
   document.querySelector(`.track-row[data-id="${song.id}"]`)?.classList.add('playing');

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
         if (btn.textContent === '▶') apiCall('castControl', {action: 'play'}).catch(() => {});
         else                         apiCall('castControl', {action: 'pause'}).catch(() => {});
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

async function viewTracks(albumId, albumTitle, artistId, artistName) {
   console.log('[tracks] loading album', albumId, albumTitle);
   const pane = document.getElementById('pane-tracks');
   pane.innerHTML = '';

   const sr = await apiCall('getAlbum', {id: albumId});
   const album = sr.album ?? {};
   const songs = album.song ?? [];
   console.log('[tracks] got', songs.length, 'tracks');

   // Back link + headings.
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
   header.appendChild(back);
   header.appendChild(heading);
   pane.appendChild(header);

   // Large cover art hero.
   if (album.coverArt) {
      const hero = document.createElement('img');
      hero.className = 'album-hero';
      hero.src = apiUrl('getCoverArt', {id: album.coverArt, size: 400});
      hero.alt = albumTitle;
      pane.appendChild(hero);
      }

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
      row.dataset.id = song.id;

      const num = document.createElement('span');
      num.className = 'track-num';
      num.textContent = useSeq ? (i + 1) : (song.track ?? '');

      const title = document.createElement('span');
      title.className = 'track-title';
      title.textContent = song.title;

      const dur = document.createElement('span');
      dur.className = 'track-dur';
      dur.textContent = song.duration ? fmtDuration(song.duration) : '';

      row.addEventListener('click', () => playerLoad(songs, i));
      row.appendChild(num);
      row.appendChild(title);
      row.appendChild(dur);
      frag.appendChild(row);
      }

   pane.appendChild(frag);
   paneNav.slideTo(2);

   // Fetch album notes without blocking the track listing.
   apiCall('getAlbumInfo2', {id: albumId}).then(srInfo => {
      const info = srInfo?.albumInfo2 ?? {};
      infoSlot.className = '';   // remove shimmer regardless of outcome

      const notes   = info.notes   ?? '';
      const wikiUrl = info.wikiUrl ?? '';
      if (!notes && !wikiUrl) return;

      const block = document.createElement('div');
      block.className = 'album-notes';

      if (notes) {
         const p = document.createElement('p');
         p.className = 'artist-bio-text';   // reuse same clamp style
         p.textContent = notes;
         block.appendChild(p);

         const toggle = document.createElement('span');
         toggle.className = 'bio-toggle';
         toggle.textContent = 'more';
         toggle.addEventListener('click', () => {
            const expanded = p.classList.toggle('expanded');
            toggle.textContent = expanded ? 'less' : 'more';
            });
         block.appendChild(toggle);

         requestAnimationFrame(() => {
            if (p.scrollHeight <= p.clientHeight) toggle.hidden = true;
            });
         }

      if (wikiUrl) {
         const a = document.createElement('a');
         a.className = 'wiki-link';
         a.href = wikiUrl;
         a.target = '_blank';
         a.rel = 'noopener';
         a.textContent = 'Wikipedia';
         block.appendChild(a);
         }

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

// ── Shell ───────────────────────────────────────────────────────────────────

async function showShell() {
   console.log('[shell] showing main shell');
   document.getElementById('login-screen').hidden = true;
   const shell = document.getElementById('app-shell');
   shell.hidden = false;
   console.log('[shell] app-shell hidden=', shell.hidden, 'display=', getComputedStyle(shell).display);

   setupPlayer();

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

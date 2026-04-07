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

// ── Views ───────────────────────────────────────────────────────────────────

async function showView(name) {
   console.log('[view] showView', name);
   document.querySelectorAll('#sidebar a').forEach(a => {
      a.classList.toggle('active', a.dataset.view === name);
   });
   const content = document.getElementById('content');
   content.innerHTML = '';

   if (name === 'artists') {
      await viewArtists(content);
   } else {
      content.innerHTML = `<p style="color:var(--text-dim)">${name}</p>`;
   }
}

async function viewArtists(container) {
   console.log('[artists] loading');
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
            history.pushState({view: 'albums', artistId: artist.id, artistName: artist.name}, '');
            viewAlbums(artist.id, artist.name, container);
            });
         frag.appendChild(row);
      }
   }
   container.appendChild(frag);
}

async function viewAlbums(artistId, artistName, container) {
   console.log('[albums] loading artist', artistId, artistName);
   container.innerHTML = '';

   const sr = await apiCall('getArtist', {id: artistId});
   const albums = sr.artist?.album ?? [];
   console.log('[albums] got', albums.length, 'albums');

   const frag = document.createDocumentFragment();

   // Back link + artist heading.
   const header = document.createElement('div');
   header.className = 'view-header';
   const back = document.createElement('span');
   back.className = 'back-link';
   back.textContent = '← Artists';
   back.addEventListener('click', () => history.back());
   const heading = document.createElement('h1');
   heading.className = 'view-title';
   heading.textContent = artistName;
   header.appendChild(back);
   header.appendChild(heading);
   frag.appendChild(header);

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
         history.pushState({view: 'tracks', albumId: album.id, albumTitle: album.title, artistId, artistName}, '');
         viewTracks(album.id, album.title, artistId, artistName, container);
         });
      frag.appendChild(row);
      }

   container.appendChild(frag);
}

function fmtDuration(secs) {
   const m = Math.floor(secs / 60);
   const s = String(secs % 60).padStart(2, '0');
   return `${m}:${s}`;
}

// ── Player ───────────────────────────────────────────────────────────────────

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
      player.audio.currentTime = Number(seek.value);
      delete seek.dataset.seeking;
      });

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

async function viewTracks(albumId, albumTitle, artistId, artistName, container) {
   console.log('[tracks] loading album', albumId, albumTitle);
   container.innerHTML = '';

   const sr = await apiCall('getAlbum', {id: albumId});
   const songs = sr.album?.song ?? [];
   console.log('[tracks] got', songs.length, 'tracks');

   const frag = document.createDocumentFragment();

   // Back link + headings.
   const header = document.createElement('div');
   header.className = 'view-header';
   const back = document.createElement('span');
   back.className = 'back-link';
   back.textContent = `← ${artistName}`;
   back.addEventListener('click', () => history.back());
   const heading = document.createElement('h1');
   heading.className = 'view-title';
   heading.textContent = albumTitle;
   header.appendChild(back);
   header.appendChild(heading);
   frag.appendChild(header);

   for (const song of songs) {
      const row = document.createElement('div');
      row.className = 'track-row';
      row.dataset.id = song.id;

      const num = document.createElement('span');
      num.className = 'track-num';
      num.textContent = song.track ?? '';

      const title = document.createElement('span');
      title.className = 'track-title';
      title.textContent = song.title;

      const dur = document.createElement('span');
      dur.className = 'track-dur';
      dur.textContent = song.duration ? fmtDuration(song.duration) : '';

      row.addEventListener('click', () => playerLoad(songs, songs.indexOf(song)));
      row.appendChild(num);
      row.appendChild(title);
      row.appendChild(dur);
      frag.appendChild(row);
      }

   container.appendChild(frag);
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

   // Wire up sidebar links with history entries.
   shell.querySelectorAll('[data-view]').forEach(a => {
      a.addEventListener('click', e => {
         e.preventDefault();
         history.pushState({view: a.dataset.view}, '');
         showView(a.dataset.view).catch(err => console.error('[view] error', err));
      });
   });

   // Handle browser back/forward: re-render from the popped state.
   window.addEventListener('popstate', async e => {
      const s = e.state ?? {view: 'artists'};
      const content = document.getElementById('content');
      if (s.view === 'albums')
         await viewAlbums(s.artistId, s.artistName, content);
      else if (s.view === 'tracks')
         await viewTracks(s.albumId, s.albumTitle, s.artistId, s.artistName, content);
      else
         await showView('artists');
      });

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

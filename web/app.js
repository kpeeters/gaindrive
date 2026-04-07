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
   back.addEventListener('click', () => showView('artists'));
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

      const title = document.createElement('span');
      title.className = 'album-title';
      title.textContent = album.title;

      const meta = document.createElement('span');
      meta.className = 'album-meta';
      const parts = [];
      if (album.year)      parts.push(album.year);
      if (album.songCount) parts.push(`${album.songCount} tracks`);
      meta.textContent = parts.join(' · ');

      row.appendChild(title);
      row.appendChild(meta);
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

   // Wire up sidebar links.
   shell.querySelectorAll('[data-view]').forEach(a => {
      a.addEventListener('click', e => {
         e.preventDefault();
         showView(a.dataset.view).catch(err => console.error('[view] error', err));
      });
   });

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

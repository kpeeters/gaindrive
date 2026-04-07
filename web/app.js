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

   const p = new URLSearchParams({
      u: user,
      p: password,
      v: '1.16.1',
      c: 'gaindrive-web',
      f: 'json',
   });
   const resp = await fetch(`${server}/rest/ping.view?${p}`);
   if (!resp.ok)
      throw new Error(`Server returned HTTP ${resp.status}`);
   const data = await resp.json();
   const sr = data['subsonic-response'];
   if (sr.status !== 'ok')
      throw new Error(sr.error?.message ?? 'Authentication failed');

   creds.save(server, user, password);
}

// ── Views ───────────────────────────────────────────────────────────────────

// Placeholder — individual view modules will fill this out.
function showView(name) {
   document.querySelectorAll('#sidebar a').forEach(a => {
      a.classList.toggle('active', a.dataset.view === name);
   });
   document.getElementById('content').innerHTML =
      `<p style="color:var(--text-dim)">${name}</p>`;
}

// ── Shell ───────────────────────────────────────────────────────────────────

function showShell() {
   document.getElementById('login-screen').hidden = true;
   const shell = document.getElementById('app-shell');
   shell.hidden = false;

   // Wire up sidebar links.
   shell.querySelectorAll('[data-view]').forEach(a => {
      a.addEventListener('click', e => {
         e.preventDefault();
         showView(a.dataset.view);
      });
   });

   showView('artists');
}

function showLogin() {
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

   try {
      await tryLogin(
         document.getElementById('server').value.trim(),
         document.getElementById('username').value.trim(),
         document.getElementById('password').value,
      );
      showShell();
   } catch (err) {
      errEl.textContent = err.message;
      errEl.hidden = false;
   } finally {
      btn.disabled = false;
   }
});

// On load: if we have saved credentials, verify them and skip the login form.
(async () => {
   const {server, user, password} = creds.load();
   if (server && user && password) {
      try {
         await tryLogin(server, user, password);
         showShell();
      } catch {
         // Credentials stale or server unreachable — show login.
         creds.clear();
         showLogin();
      }
   }
})();

'use strict';

// ── Credential storage ──────────────────────────────────────────────────────
//
// The password is never stored and, after login, never sent.
//
// Subsonic offers two ways to present credentials: `p=<password>` and the token
// scheme, `t=md5(password + salt)` with `s=<salt>`. This client used the first,
// which put the password into every request URL — and a URL is the least
// private thing in a browser. It reaches the server's access log, the reverse
// proxy's log, the browser's own history, the Referer header of anything the
// page loads, and every cache in between. Over HTTPS that is not an
// interception risk; it is a *copies* risk, and the copies outlive the session.
//
// So the salt and the token are computed once at login and those are what
// localStorage holds. The token is still equivalent to the password *for this
// server* — that is inherent to the scheme, and no client-side arithmetic can
// change it — but it is not the password itself, which is the part a person is
// liable to have reused somewhere that matters more than their music.
//
// The salt is per login rather than per request, matching AuthInterceptor.kt in
// the Android app and for the same reason recorded there: a per-request salt
// gives every cover-art URL a unique query string, so the browser's image cache
// misses on every scroll.
const creds = {
   load() {
      return {
         server: localStorage.getItem('gd_server'),
         user:   localStorage.getItem('gd_user'),
         salt:   localStorage.getItem('gd_salt'),
         token:  localStorage.getItem('gd_token'),
      };
   },
   // Stores the derived pair rather than the password, which never reaches
   // here. tryLogin() derives it, proves it against the server, and then saves
   // that exact pair — deriving a second time would store a token the login
   // never tested.
   save(server, user, salt, token) {
      localStorage.setItem('gd_server', server);
      localStorage.setItem('gd_user',   user);
      localStorage.setItem('gd_salt',   salt);
      localStorage.setItem('gd_token',  token);
   },
   clear() {
      localStorage.removeItem('gd_server');
      localStorage.removeItem('gd_user');
      localStorage.removeItem('gd_salt');
      localStorage.removeItem('gd_token');
      // Written by versions of this client that sent p=. Removed on any
      // logout or failed restore so an upgrade does not leave the password
      // sitting in localStorage for ever.
      localStorage.removeItem('gd_password');
   },
};

// This browser's identity, as far as casting is concerned.
//
// A cast session on the server belongs to one account *and* one client
// instance, and this is the second half. It cannot be the Subsonic `c=`
// parameter, which is `gaindrive-web` for every browser in the world: the
// server would then be unable to tell this browser from another of the same
// user's, and playing a track in one would push it onto the other's Chromecast
// — which is the bug this exists to fix, seen from one machine over.
//
// Deliberately outside `creds` and never cleared on logout. It identifies the
// browser, not the account, and ownership is the pair, so carrying it across a
// logout is harmless and keeps the id stable for as long as the profile lives.
// It is not a credential and authorises nothing.
function controllerId() {
   let id = localStorage.getItem('gd_controller');
   if (!id) {
      // The same eight bytes from the platform CSPRNG that deriveToken() takes
      // its salt from.
      const bytes = new Uint8Array(8);
      crypto.getRandomValues(bytes);
      id = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
      localStorage.setItem('gd_controller', id);
   }
   return id;
}

// The auth parameters for one request. Every URL this client builds goes
// through here, so there is one place that decides what is sent.
function authParams() {
   const {user, salt, token} = creds.load();
   return {u: user, t: token, s: salt};
}

// A salt and the token derived from it. The server requires at least six
// characters of salt (the Subsonic spec's floor); sixteen hex characters is
// eight bytes from the platform CSPRNG.
function deriveToken(password) {
   const bytes = new Uint8Array(8);
   crypto.getRandomValues(bytes);
   const salt = Array.from(bytes, b => b.toString(16).padStart(2, '0')).join('');
   return {salt, token: md5(password + salt)};
}

// ── MD5 ─────────────────────────────────────────────────────────────────────
//
// RFC 1321, and here only because the Subsonic token scheme specifies it.
// SubtleCrypto deliberately does not implement MD5, so there is nothing to
// call; this is the browser counterpart of src/md5.hh and produces the same
// lowercase hex the server compares against.
//
// The input is UTF-8 encoded first: a password with a non-ASCII character has
// to hash the same bytes the server hashes, and the server hashes the bytes it
// received.
function md5(str) {
   const S = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
              5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
              4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
              6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
   const K = new Uint32Array(64);
   for (let i = 0; i < 64; i++)
      K[i] = Math.floor(Math.abs(Math.sin(i + 1)) * 4294967296);

   const msg = new TextEncoder().encode(str);
   // Append 0x80, pad to 56 mod 64, then the 64-bit little-endian bit length.
   const padded = new Uint8Array((((msg.length + 8) >> 6) + 1) << 6);
   padded.set(msg);
   padded[msg.length] = 0x80;
   const bits = msg.length * 8;
   const dv = new DataView(padded.buffer);
   dv.setUint32(padded.length - 8, bits >>> 0, true);
   dv.setUint32(padded.length - 4, Math.floor(bits / 4294967296), true);

   let a0 = 0x67452301, b0 = 0xefcdab89, c0 = 0x98badcfe, d0 = 0x10325476;
   const M = new Uint32Array(16);
   const rotl = (x, c) => (x << c) | (x >>> (32 - c));

   for (let off = 0; off < padded.length; off += 64) {
      for (let i = 0; i < 16; i++) M[i] = dv.getUint32(off + i * 4, true);
      let A = a0, B = b0, C = c0, D = d0;
      for (let i = 0; i < 64; i++) {
         let F, g;
         if (i < 16)      { F = (B & C) | (~B & D);        g = i; }
         else if (i < 32) { F = (D & B) | (~D & C);        g = (5 * i + 1) & 15; }
         else if (i < 48) { F = B ^ C ^ D;                 g = (3 * i + 5) & 15; }
         else             { F = C ^ (B | ~D);              g = (7 * i) & 15; }
         F = (F + A + K[i] + M[g]) >>> 0;
         A = D; D = C; C = B;
         B = (B + rotl(F, S[i])) >>> 0;
      }
      a0 = (a0 + A) >>> 0; b0 = (b0 + B) >>> 0;
      c0 = (c0 + C) >>> 0; d0 = (d0 + D) >>> 0;
   }

   const hex = n => {
      let s = '';
      for (let i = 0; i < 4; i++)
         s += ((n >>> (i * 8)) & 0xff).toString(16).padStart(2, '0');
      return s;
   };
   return hex(a0) + hex(b0) + hex(c0) + hex(d0);
}

// ── Playback preferences ────────────────────────────────────────────────────

// "Play videos as audio only": ask the server for the soundtrack alone rather
// than the picture.  Backing out of the video surface does not do this — the
// whole stream still arrives — so it is a request-level choice, not a UI one.
const videoAudioOnly = {
   get()   { return localStorage.getItem('gd_video_audio_only') === '1'; },
   set(on) {
      if (on) localStorage.setItem('gd_video_audio_only', '1');
      else    localStorage.removeItem('gd_video_audio_only');
      },
   };

// ── Library preferences ────────────────────────────────────

// Which order the album pane lists an artist's albums in.  The server answers
// in year order and that is the default here, spelled as the *absent* key the
// way castLocalVideo's default is.
//
// Kept per library mode, because the two libraries are browsed for different
// reasons: a discography is chronological, while a film category is findable
// only by name.  libraryMode is declared further down the file — that is fine,
// it is read at call time and never at load time.
const albumSort = {
   get()  {
      return localStorage.getItem(`gd_album_sort_${libraryMode}`) === 'name'
         ? 'name' : 'year';
      },
   set(v) {
      const k = `gd_album_sort_${libraryMode}`;
      if (v === 'name') localStorage.setItem(k, 'name');
      else              localStorage.removeItem(k);
      },
   };

// The year arm reproduces the server's own ORDER BY exactly — year, then title
// case-insensitively — so toggling back to Year restores the list the server
// sent rather than a subtly different one.  Sorts a copy, leaving the fetched
// array as the server's answer.
function sortedAlbums(list, mode) {
   const name    = a => a.title ?? a.name ?? '';
   const cmpName = (a, b) =>
      name(a).localeCompare(name(b), undefined, {sensitivity: 'base'});
   return [...list].sort(mode === 'name'
      ? (a, b) => cmpName(a, b) || (a.year ?? 0) - (b.year ?? 0)
      : (a, b) => (a.year ?? 0) - (b.year ?? 0) || cmpName(a, b));
}

// ── Theme ────────────────────────────────────────────────────────────────────

// Cycles: auto (system preference) → light → dark → auto.
const THEME_CYCLE = ['auto', 'light', 'dark'];

function applyTheme(theme) {
   document.documentElement.classList.remove('light', 'dark');
   if (theme !== 'auto')
      document.documentElement.classList.add(theme);

   if (theme === 'auto') localStorage.removeItem('gd_theme');
   else                  localStorage.setItem('gd_theme', theme);

}

// ── Subsonic API wrapper ────────────────────────────────────────────────────

// Probe element used to ask the browser which MIME types it can decode.
// Reused across calls so we don't spin up an HTMLAudioElement per track.
const _canPlayProbe = document.createElement('audio');

// If the browser cannot play the format the server would otherwise send,
// return 'mp3' so the caller appends ?format=mp3 to the stream URL and the
// server transcodes. Returns null when direct serve is fine. mp3 is the
// universal fallback: HTMLAudioElement.canPlayType('audio/mpeg') is
// 'probably' in every modern browser.
//
// Only 'probably' bypasses transcode. 'maybe' is unreliable in practice —
// notably Firefox on Linux returns 'maybe' for audio/mp4 then fails on the
// AAC payload with a scary "could not be decoded" console message. Treating
// 'maybe' as a no preempts the error before Firefox emits it. The decode-
// error event listener below is still kept as a defensive safety net for
// 'probably' surprises (rare, but possible across browser/codec updates).
function pickStreamFormat(song) {
   const mime = song.transcodedContentType ?? song.contentType;
   if (!mime) return null;
   if (_canPlayProbe.canPlayType(mime) === 'probably') return null;
   return 'mp3';
}

// Which container to ask for when playing a video as audio only.
//
// Deliberately not pickStreamFormat(): that probes the song's own type, which
// for a video is a *video* container, so an audio element answers "no" and it
// would land on mp3 by accident rather than by decision.  Opus is the better
// answer where it decodes — a film's soundtrack is long, and this is a
// re-encode either way, so the container is a free choice.
// Keep the picture in this player when the sound is going to a receiver that
// cannot show it.  Default **on**: the alternative is a black panel, the film
// is being read off the server's disk either way, and the one cost — a second
// stream to this browser — is the thing the setting exists to decline.
const castLocalVideo = {
   get()   { return localStorage.getItem('gd_cast_local_video') !== '0'; },
   set(on) {
      if (on) localStorage.removeItem('gd_cast_local_video');
      else    localStorage.setItem('gd_cast_local_video', '0');
      },
   };

// How far the picture is held back from the receiver's reported position, in
// milliseconds, per device.
//
// This is the one quantity nothing here can compute.  What a receiver reports
// is where its *decoder* is, and the sound leaves the speakers some unknown
// time later — a Chromecast's own output buffer plus, on an amplifier, its
// DSP.  It is a constant for a given device, so it is calibrated once by the
// person watching and kept against that device's id.  Everything else about
// the drift is measured and corrected continuously; see castSyncTick().
const castSyncDelay = {
   get(id)     { return id ? +(localStorage.getItem(`gd_cast_sync_${id}`) || 0) : 0; },
   set(id, ms) {
      if (!id) return;
      if (ms) localStorage.setItem(`gd_cast_sync_${id}`, String(ms));
      else    localStorage.removeItem(`gd_cast_sync_${id}`);
      },
   };

// The staircase behind the two adjustment buttons, and the interlock that keeps
// a person from adjusting into a picture that has not finished moving.
//
// Declared here rather than beside SYNC_DEAD and the rest of the loop's
// constants, although they read as one family: castSyncStep is initialised at
// module load and those sit two thousand lines below, which is a temporal dead
// zone and a ReferenceError before the client has drawn anything.  The split is
// honest anyway — these describe the control, those describe the loop.
//
// castSyncPhase is what the buttons read:
//
//   'idle'      nothing in flight, the picture is where the setting says
//   'moving'    a step is being performed by the element; the loop stands off
//   'settling'  the step landed (or could not be made) and the loop is closing
//               the remainder; the buttons stay disabled until |err| is back
//               inside SYNC_DEAD
//
// The last state is the signal that was missing entirely.  The loop takes 5-15 s
// to absorb anything it is given, and nothing anywhere said so — so the natural
// thing to do was adjust again, and overshoot.  castSyncTick() is the settle
// detector because it is the only thing that knows.
// A press moves this far, halving on every reversal.  400 ms is big enough to
// be unmistakable on the first press and still only three presses from the
// floor; 25 ms is below one frame at any sane rate and below what an eye can
// judge, so refining past it would be asking for a decision nobody can make.
const SYNC_STEP_START = 400;
const SYNC_STEP_MIN   = 25;
// A settle that never completes must not leave the buttons dead for ever.
const SYNC_SETTLE_CAP = 20000;

let castSyncStep    = SYNC_STEP_START;
let castSyncLastDir = 0;
let castSyncRun     = 0;
let castSyncPhase   = 'idle';
// Latched per stream: a chunked re-encode cannot be seeked outside what it has
// buffered, and a refused seek is silent — no 'seeked' event, no error.
let castSyncNoSeek  = false;
let castSyncSettleAt = 0;

function audioOnlyFormat() {
   return _canPlayProbe.canPlayType('audio/ogg; codecs=opus') === 'probably'
      ? 'opus' : 'mp3';
}

// Build a subsonic API URL. Extra params can be passed as an object.
function apiUrl(endpoint, extra = {}) {
   const {server} = creds.load();
   const p = new URLSearchParams({
      ...authParams(),
      v: '1.16.1',
      c: 'gaindrive-web',
      // Sent on everything rather than threaded through the seven cast calls
      // by hand — the same bargain authParams() strikes, and stream.view needs
      // it too since that is where the server decides whether to redirect
      // playback to the Chromecast. It is constant, so it costs the image
      // cache nothing.
      castController: controllerId(),
      f: 'json',
      ...extra,
   });
   return `${server}/rest/${endpoint}.view?${p}`;
}

// getCoverArt's `size` is a count of pixels, and a CSS pixel is not a pixel:
// a 2x screen draws an 80px thumbnail into 160 physical ones and stretches an
// 80px image over the lot. So every call site names the CSS box it is filling
// -- a number a reader can check against style.css -- and this turns it into
// what the screen will actually draw.
//
// Quantised to 1x or 2x rather than following the ratio, because each distinct
// value is another blob in the server's cover_thumbs, which has no eviction;
// and capped at 2 because a 3x phone cannot show the difference at these sizes
// and would be paying 2.5x the bytes for it over a mobile link.
//
// Note the server fits the *short* edge, which is what makes this exact: at
// size=160 a 2:3 poster arrives 160x240, and the 80px square cell crops it to
// 160x160 device pixels with no scaling at all.
function coverPx(css) {
   return css * (window.devicePixelRatio > 1 ? 2 : 1);
}

// The CSS box .album-cover draws in, which is no longer what is fetched for
// it. The width/height attributes are the layout hint before the image lands
// and so are the box; dataset.coverSize is what to re-request and so is the
// device figure. They were one number until covers went 2x, and the
// cover-art-changed listener reads both.
const ALBUM_COVER_BOX = 80;

// .cover-hero-wrap's width: min(100%, 320px), which the hero image fills.
const HERO_BOX = 320;

// ---- Artist portraits -------------------------------------------------
//
// An artist portrait is not a file on the server, it is something the server
// has to go and find: MusicBrainz, then Wikidata, then Wikipedia, then a
// couple of others. That runs on a background thread now, so asking for one
// that has not been resolved yet gets a 404 rather than a stalled request —
// and the request itself is what pushes that artist to the front of the
// resolver's queue.
//
// So a 404 here means "not yet", and the only thing missing is a nudge to ask
// again. Without one the portraits would appear on the next navigation, which
// is a strange thing to ask a user to discover.
const portraitPending = {
   items: new Map(),          // id -> {img, size, tries}
   timer: null,

   add(id, img, size) {
      this.items.set(String(id), {img, size, tries: 0});
      if (this.timer === null)
         this.timer = setInterval(() => this.tick(), 15000);
      },

   tick() {
      for (const [id, e] of this.items) {
         // A pane that has been navigated away from and reused drops its
         // nodes. Nobody can see this image, so stop asking about it.
         if (!document.contains(e.img)) { this.items.delete(id); continue; }
         // Ten minutes is long enough for any backlog a person is waiting on.
         if (++e.tries > 40) { this.items.delete(id); continue; }
         // The cache-buster is not superstition: assigning the same string to
         // img.src is a no-op in every browser.
         e.img.src = apiUrl('getCoverArt', {id, size: e.size, _v: Date.now()});
         }
      if (this.items.size === 0) {
         clearInterval(this.timer);
         this.timer = null;
         }
      },
   };

// An <img> pointing at our own getCoverArt, which retries itself while the
// server is still working out who this artist is.
function artistPortrait(id, size, className, alt = '') {
   const img = document.createElement('img');
   img.className = className;
   img.alt = alt;
   img.addEventListener('load',  () => img.classList.remove('is-missing'));
   img.addEventListener('error', () => {
      img.classList.add('is-missing');
      portraitPending.add(id, img, size);
      });
   img.src = apiUrl('getCoverArt', {id, size});
   return img;
   }

// What the server said it is, for the About box. Taken from whatever response
// happens to arrive rather than asked for: every one carries the field, and
// this client is served by the binary it is reporting on, so the server's
// version is its own. Nothing else here needs a version, which is why there is
// no build-time substitution for one.
let serverVersion = null;

// Call a subsonic endpoint and return the parsed subsonic-response object.
// Throws on network error or non-ok subsonic status.
async function apiCall(endpoint, extra = {}) {
   const resp = await fetch(apiUrl(endpoint, extra));
   if (!resp.ok)
      throw new Error(`HTTP ${resp.status}`);
   const data = await resp.json();
   const sr = data['subsonic-response'];
   if (sr.serverVersion) serverVersion = sr.serverVersion;
   if (sr.status !== 'ok')
      throw new Error(sr.error?.message ?? 'Unknown error');
   return sr;
}

// ── Error dialog ─────────────────────────────────────────────────────────────

function showError(msg) {
   document.getElementById('error-modal-msg').textContent = msg;
   document.getElementById('error-modal').classList.remove('hidden');
   }

let _confirmYes = null;
function showConfirm(msg, onYes, {title='Confirm', yes='OK', no='Cancel'} = {}) {
   document.getElementById('confirm-modal-title').textContent = title;
   document.getElementById('confirm-modal-msg').textContent = msg;
   document.getElementById('confirm-yes-btn').textContent = yes;
   document.getElementById('confirm-no-btn').textContent = no;
   _confirmYes = onYes;
   document.getElementById('confirm-modal').classList.remove('hidden');
   }

let _coverArtCb = null;
function showCoverArtDialog(onPicked) {
   _coverArtCb = onPicked;
   document.getElementById('cover-art-file-name').textContent = '';
   document.getElementById('cover-art-url').value = '';
   const prev = document.getElementById('cover-art-url-preview');
   prev.removeAttribute('src');
   prev.classList.add('hidden');
   document.getElementById('cover-art-modal').classList.remove('hidden');
   }

function _closeCoverArtDialog() {
   document.getElementById('cover-art-modal').classList.add('hidden');
   _coverArtCb = null;
   }

// ---- Promoting an upload into the shared library --------------------------
//
// The destination is a root plus one level, and nothing more: both layouts are
// L1/L2/[L3]/files, the album being moved is L2, and L3 is only ever a disc or
// season directory inside it. Two controls, not a browser.
let _promoteCb = null;

// Suggestions for the chosen root, so that "type a name not in the list" is how
// a new artist or category is made — the server creates the directory, and
// there is no separate operation for it.
async function _fillPromoteFolders(rootId) {
   const list = document.getElementById('promote-folder-list');
   list.innerHTML = '';
   try {
      const r = await apiCall('getArtists', {musicFolderId: rootId});
      for (const idx of r.artists?.index ?? [])
         for (const a of idx.artist) {
            const opt = document.createElement('option');
            opt.value = a.name;
            list.appendChild(opt);
            }
      }
   catch { /* completion only; typing still works */ }
   }

// [artist] is what the batch is currently filed under, used as the default for
// an artists root only. Under a categories root L1 is a *category*, and the
// batch's artist is whatever the source called it — a channel name, usually,
// which is never the answer.
function showPromoteDialog(album, artist, onGo) {
   _promoteCb = onGo;
   document.getElementById('promote-what').textContent =
      `“${album}” is moved out of your uploads and onto the server's disk in `
      + 'the shared library, where everyone with an account can see it. This '
      + 'cannot be undone from here.';

   const rootSel = document.getElementById('promote-root');
   const folder  = document.getElementById('promote-folder');
   const label   = document.getElementById('promote-folder-label');
   rootSel.innerHTML = '';
   for (const f of musicFolders ?? []) {
      const opt = document.createElement('option');
      opt.value       = f.id;
      opt.textContent = f.contentType ? `${f.name} (${f.contentType})` : f.name;
      opt.dataset.contentType = f.contentType ?? '';
      rootSel.appendChild(opt);
      }

   // The server requires both halves of the destination, so the button is the
   // place that says so rather than a refusal after the fact. It is also what
   // catches a categories root left with the field empty, which used to send
   // nothing and file a documentary under the channel that published it.
   const goBtn = document.getElementById('promote-go-btn');
   const syncGo = () => {
      goBtn.disabled = !rootSel.value || !folder.value.trim();
      };

   const syncRoot = () => {
      const opt = rootSel.selectedOptions[0];
      const isCategories = opt?.dataset.contentType === 'categories';
      label.textContent = isCategories ? 'Category:' : 'Artist:';
      // Blank under a categories root, deliberately: L1 there is a category,
      // and the batch's artist is whatever the source called it.
      folder.value = isCategories ? '' : artist;
      folder.placeholder = isCategories ? 'Which category?' : artist;
      document.getElementById('promote-note').textContent = isCategories
         ? 'A category that does not exist yet is created.'
         : 'An artist that does not exist yet is created.';
      _fillPromoteFolders(rootSel.value);
      syncGo();
      };
   // Assigned, not addEventListener: this runs again every time the dialog is
   // opened, and adding would stack a fresh handler on the same element each
   // time.
   rootSel.onchange = syncRoot;
   folder.oninput   = syncGo;
   syncRoot();

   document.getElementById('promote-modal').classList.remove('hidden');
   }

function _closePromoteDialog() {
   document.getElementById('promote-modal').classList.add('hidden');
   _promoteCb = null;
   }

function showLightbox(src) {
   document.getElementById('cover-lightbox-img').src = src;
   document.getElementById('cover-lightbox').classList.remove('hidden');
   }

// ── Login ───────────────────────────────────────────────────────────────────

async function tryLogin(server, user, password) {
   // Normalise server URL: strip trailing slash.
   server = server.replace(/\/+$/, '');
   console.log('[login] attempting ping', server, user);

   // Derived once and proved before it is stored, so what ends up in
   // localStorage is a pair the server has already accepted. The password is
   // not sent and is not kept.
   const {salt, token} = deriveToken(password);
   const p = new URLSearchParams({
      u: user,
      t: token,
      s: salt,
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
   creds.save(server, user, salt, token);
}

// Verify credentials already in localStorage. Separate from tryLogin() because
// there is no password to derive from at this point — the stored token is the
// credential, and this only asks the server whether it still works.
async function verifySaved() {
   const {server, user, salt, token} = creds.load();
   if (!server || !user || !salt || !token) return false;
   const p = new URLSearchParams({
      u: user, t: token, s: salt, v: '1.16.1', c: 'gaindrive-web', f: 'json',
   });
   const resp = await fetch(`${server}/rest/ping.view?${p}`);
   if (!resp.ok) throw new Error(`Server returned HTTP ${resp.status}`);
   const sr = (await resp.json())['subsonic-response'];
   if (sr.status !== 'ok')
      throw new Error(sr.error?.message ?? 'Authentication failed');
   return true;
}

async function detectSubsonicOrigin() {
   if (window.location.protocol === 'file:') return false;
   try {
      const resp = await fetch('/rest/ping.view?v=1.16.1&c=gaindrive-web&f=json');
      if (!resp.ok) return false;
      const data = await resp.json();
      return 'subsonic-response' in data;
   } catch {
      return false;
   }
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
      } else if (name === 'playlists') {
      await viewPlaylists();
      } else if (name === 'settings') {
      await viewSettings();
      } else if (name === 'recents') {
      await viewRecents();
      } else {
      document.getElementById('pane-artists').innerHTML =
         `<p style="color:var(--text-dim)">${name}</p>`;
      document.getElementById('pane-albums').innerHTML = '';
      document.getElementById('pane-tracks').innerHTML = '';
      paneNav.slideTo(0);
      }
}

async function viewSettings() {
   const pane = document.getElementById('pane-artists');
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';
   pane.innerHTML = '';
   paneNav.slideTo(0);

   const hdr = document.createElement('div');
   hdr.className = 'view-header';
   const h1 = document.createElement('h1');
   h1.className = 'view-title';
   h1.textContent = 'Settings';
   hdr.appendChild(h1);
   pane.appendChild(hdr);

   // ── Theme section — visible to all users ────────────────────────────────

   const themeSection = document.createElement('div');
   themeSection.className = 'admin-section';

   const themeHeading = document.createElement('h2');
   themeHeading.className = 'admin-section-title';
   themeHeading.textContent = 'Appearance';
   themeSection.appendChild(themeHeading);

   const themeRow = document.createElement('div');
   themeRow.className = 'theme-btn-row';

   for (const t of THEME_CYCLE) {
      const btn = document.createElement('button');
      btn.textContent = t.charAt(0).toUpperCase() + t.slice(1);
      btn.className   = 'theme-option-btn';
      btn.id          = `theme-opt-${t}`;
      btn.addEventListener('click', () => { applyTheme(t); markActiveTheme(); });
      themeRow.appendChild(btn);
      }

   themeSection.appendChild(themeRow);
   pane.appendChild(themeSection);

   // ── Playback section — visible to all users ─────────────────────────────

   const playbackSection = document.createElement('div');
   playbackSection.className = 'admin-section';

   const playbackHeading = document.createElement('h2');
   playbackHeading.className = 'admin-section-title';
   playbackHeading.textContent = 'Playback';
   playbackSection.appendChild(playbackHeading);

   const audioOnlyRow   = document.createElement('div');
   audioOnlyRow.className = 'form-row';
   const audioOnlyLabel = document.createElement('label');
   const audioOnlyBox   = document.createElement('input');
   audioOnlyBox.type    = 'checkbox';
   audioOnlyBox.checked = videoAudioOnly.get();
   audioOnlyBox.addEventListener('change',
      () => videoAudioOnly.set(audioOnlyBox.checked));
   audioOnlyLabel.appendChild(audioOnlyBox);
   audioOnlyLabel.appendChild(
      document.createTextNode('Play videos as audio only'));
   audioOnlyRow.appendChild(audioOnlyLabel);
   playbackSection.appendChild(audioOnlyRow);

   const audioOnlyHint = document.createElement('p');
   audioOnlyHint.className = 'admin-hint';
   audioOnlyHint.textContent =
      'Streams the soundtrack instead of the picture, which is a fraction of '
      + 'the data. Takes effect on the next track.';
   playbackSection.appendChild(audioOnlyHint);

   const localVidRow   = document.createElement('div');
   localVidRow.className = 'form-row';
   const localVidLabel = document.createElement('label');
   const localVidBox   = document.createElement('input');
   localVidBox.type    = 'checkbox';
   localVidBox.checked = castLocalVideo.get();
   localVidBox.addEventListener('change',
      () => castLocalVideo.set(localVidBox.checked));
   localVidLabel.appendChild(localVidBox);
   localVidLabel.appendChild(
      document.createTextNode('Keep video here when casting to a speaker'));
   localVidRow.appendChild(localVidLabel);
   playbackSection.appendChild(localVidRow);

   const localVidHint = document.createElement('p');
   localVidHint.className = 'admin-hint';
   localVidHint.textContent =
      'A speaker or amplifier that cannot show a picture is sent the '
      + 'soundtrack. With this on the film also plays here, muted and kept in '
      + 'step with it — which means streaming it to this device as well. '
      + 'Takes effect on the next track.';
   playbackSection.appendChild(localVidHint);

   pane.appendChild(playbackSection);

   // Mark the currently active theme button.
   function markActiveTheme() {
      const current = localStorage.getItem('gd_theme') ?? 'auto';
      THEME_CYCLE.forEach(t => {
         const b = document.getElementById(`theme-opt-${t}`);
         if (b) b.classList.toggle('active', t === current);
         });
      }
   markActiveTheme();

   // Fetch current user's roles to decide what sections to show.
   let userInfo = null;
   try {
      const sr = await apiCall('getUser', {username: creds.load().user});
      userInfo = sr.user;
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      }

   // The archive-upload form used to sit here. It now lives at the top of the
   // Library view's Uploads mode — see makeUploadBar() — because that is the
   // listing it fills.

   // ── Users section — visible to admins only ──────────────────────────────

   if (userInfo?.adminRole) {

   const userSection = document.createElement('div');
   userSection.className = 'admin-section';

   const userHeading = document.createElement('h2');
   userHeading.className = 'admin-section-title';
   userHeading.textContent = 'Users';
   userSection.appendChild(userHeading);

   const userList = document.createElement('div');
   userList.className = 'user-list';
   userSection.appendChild(userList);

   const addBtn = document.createElement('button');
   addBtn.textContent = 'Add user';
   addBtn.className   = 'upload-btn';
   userSection.appendChild(addBtn);

   pane.appendChild(userSection);

   async function refreshUsers() {
      userList.innerHTML = '';
      try {
         const sr = await apiCall('getUsers');
         // getUsers returns {users: {user: [...]}} — user may be absent if empty.
         const users = sr.users?.user ?? [];
         const arr = Array.isArray(users) ? users : [users];
         for (const u of arr) {
            const row = document.createElement('div');
            row.className = 'user-row';

            const name = document.createElement('span');
            name.className   = 'user-row-name';
            name.textContent = u.username;
            row.appendChild(name);

            const badges = document.createElement('span');
            badges.className = 'user-badges';
            if (u.adminRole)  badges.appendChild(makeBadge('Admin',    'badge-admin'));
            if (u.uploadRole) badges.appendChild(makeBadge('Upload',   'badge-upload'));
            if (u.castRole)   badges.appendChild(makeBadge('Cast',     'badge-cast'));
            if (u.disabled)   badges.appendChild(makeBadge('Disabled', 'badge-disabled'));
            row.appendChild(badges);

            row.addEventListener('click', () => viewUserEdit(u));
            userList.appendChild(row);
            }
         }
      catch (e) {
         showError('Could not reach the server. Please check your connection.');
         userList.textContent = `Error loading users: ${e.message}`;
         }
      }

   addBtn.addEventListener('click', () => viewUserEdit(null, refreshUsers));

   // Attach refreshUsers so viewUserEdit can call it back.
   await refreshUsers();

   // Expose so viewUserEdit can trigger a refresh after save.
   pane._refreshUsers = refreshUsers;

   } // end users section

   // ── Server section — admin only ────────────────────────────────────────────

   if (userInfo?.adminRole) {

   const serverSection = document.createElement('div');
   serverSection.className = 'admin-section';

   const serverHeading = document.createElement('h2');
   serverHeading.className = 'admin-section-title';
   serverHeading.textContent = 'Server';
   serverSection.appendChild(serverHeading);

   const tokenLabel = document.createElement('p');
   tokenLabel.textContent = 'Discogs Personal Access Token:';
   serverSection.appendChild(tokenLabel);

   const tokenRow = document.createElement('div');
   tokenRow.className = 'token-row';

   const tokenInput = document.createElement('input');
   tokenInput.type        = 'password';
   tokenInput.className   = 'token-input';
   tokenInput.placeholder = '(not set)';
   tokenRow.appendChild(tokenInput);

   const tokenSave = document.createElement('button');
   tokenSave.textContent = 'Save';
   tokenSave.className   = 'upload-btn';
   tokenRow.appendChild(tokenSave);

   serverSection.appendChild(tokenRow);

   const tokenStatus = document.createElement('p');
   tokenStatus.className = 'upload-status';
   serverSection.appendChild(tokenStatus);

   const tmdbLabel = document.createElement('p');
   tmdbLabel.textContent = 'TMDB API key (posters and descriptions for video):';
   serverSection.appendChild(tmdbLabel);

   const tmdbRow = document.createElement('div');
   tmdbRow.className = 'token-row';

   const tmdbInput = document.createElement('input');
   tmdbInput.type        = 'password';
   tmdbInput.className   = 'token-input';
   tmdbInput.placeholder = '(not set)';
   tmdbRow.appendChild(tmdbInput);

   const tmdbSave = document.createElement('button');
   tmdbSave.textContent = 'Save';
   tmdbSave.className   = 'upload-btn';
   tmdbRow.appendChild(tmdbSave);

   serverSection.appendChild(tmdbRow);

   const tmdbStatus = document.createElement('p');
   tmdbStatus.className = 'upload-status';
   serverSection.appendChild(tmdbStatus);

   // Required by TMDB's terms of use; it has to be visible wherever the API is
   // used, not buried in a licence file.
   const tmdbAttrib = document.createElement('p');
   tmdbAttrib.className = 'upload-status';
   tmdbAttrib.textContent =
      'This product uses the TMDB API but is not endorsed or certified by TMDB.';
   serverSection.appendChild(tmdbAttrib);

   pane.appendChild(serverSection);

   try {
      const sr = await apiCall('getServerSettings');
      tokenInput.value = sr.serverSettings?.discogsToken ?? '';
      tmdbInput.value  = sr.serverSettings?.tmdbKey ?? '';
      }
   catch { /* server may not yet have this endpoint */ }

   tokenSave.addEventListener('click', async () => {
      tokenStatus.textContent = '';
      try {
         // Only this field is sent: saveServerSettings writes what it is given,
         // so sending both would let a stale input overwrite the other setting.
         await apiCall('saveServerSettings', {discogsToken: tokenInput.value});
         tokenStatus.textContent = 'Saved.';
         }
      catch (e) {
         tokenStatus.textContent = `Error: ${e.message}`;
         }
      });

   tmdbSave.addEventListener('click', async () => {
      tmdbStatus.textContent = '';
      try {
         await apiCall('saveServerSettings', {tmdbKey: tmdbInput.value});
         tmdbStatus.textContent = 'Saved. Applies on the next library scan.';
         }
      catch (e) {
         tmdbStatus.textContent = `Error: ${e.message}`;
         }
      });

   } // end server section
   }

function makeBadge(text, cls) {
   const b = document.createElement('span');
   b.className   = `user-badge ${cls}`;
   b.textContent = text;
   return b;
   }

async function viewUserEdit(user, refreshFn) {
   // refreshFn is a callback to reload the user list; if not provided, look for it
   // on the pane element (set by viewSettings).
   const adminPane = document.getElementById('pane-artists');
   const refresh = refreshFn ?? adminPane._refreshUsers;

   const pane = document.getElementById('pane-albums');
   document.getElementById('pane-tracks').innerHTML = '';
   pane.innerHTML = '';
   paneNav.slideTo(1);

   const isNew = (user === null);

   const hdr = document.createElement('div');
   hdr.className = 'view-header';
   const h1 = document.createElement('h1');
   h1.className = 'view-title';
   h1.textContent = isNew ? 'New user' : user.username;
   hdr.appendChild(h1);
   pane.appendChild(hdr);

   const form = document.createElement('div');
   form.className = 'user-edit-form';

   function addField(labelText, inputEl) {
      const row = document.createElement('div');
      row.className = 'form-row';
      const lbl = document.createElement('label');
      lbl.textContent = labelText;
      lbl.appendChild(inputEl);
      row.appendChild(lbl);
      form.appendChild(row);
      return inputEl;
      }

   function textInput(value, placeholder, readonly) {
      const el = document.createElement('input');
      el.type        = 'text';
      el.value       = value ?? '';
      el.placeholder = placeholder ?? '';
      if (readonly) el.readOnly = true;
      return el;
      }

   function pwInput(placeholder) {
      const el = document.createElement('input');
      el.type        = 'password';
      el.placeholder = placeholder ?? '';
      el.autocomplete = 'new-password';
      return el;
      }

   function checkInput(checked) {
      const el = document.createElement('input');
      el.type    = 'checkbox';
      el.checked = !!checked;
      return el;
      }

   function numInput(value) {
      const el = document.createElement('input');
      el.type  = 'number';
      el.min   = '0';
      el.value = value ?? 0;
      return el;
      }

   const fUsername   = addField('Username',       textInput(user?.username,  '',                   !isNew));
   const fPassword   = addField('Password',       pwInput(isNew ? '' : 'Leave blank to keep current'));
   const fEmail      = addField('Email',          textInput(user?.email,     ''));
   const fAdmin      = addField('Admin',          checkInput(user?.adminRole));
   const fUpload     = addField('Upload allowed', checkInput(user?.uploadRole));
   const fCast      = addField('Cast on server network allowed',   checkInput(user?.castRole));
   const isSelf = !isNew && user.username === creds.load().user;
   const fDisabled   = addField('Disabled',       checkInput(user?.disabled));
   if (isSelf) {
      fDisabled.disabled = true;
      fDisabled.title    = 'You cannot disable your own account.';
      }
   const fMaxBitrate = addField('Max bitrate (0 = unlimited)', numInput(user?.maxBitRate ?? 0));

   const actions = document.createElement('div');
   actions.className = 'form-actions';

   const saveBtn = document.createElement('button');
   saveBtn.textContent = 'Save';
   saveBtn.className   = 'upload-btn';
   actions.appendChild(saveBtn);

   const cancelBtn = document.createElement('button');
   cancelBtn.textContent = 'Cancel';
   cancelBtn.className   = 'form-cancel-btn';
   actions.appendChild(cancelBtn);

   const formStatus = document.createElement('p');
   formStatus.className = 'upload-status';
   actions.appendChild(formStatus);

   form.appendChild(actions);
   pane.appendChild(form);

   cancelBtn.addEventListener('click', () => {
      pane.innerHTML = '';
      paneNav.slideTo(0);
      });

   saveBtn.addEventListener('click', async () => {
      formStatus.textContent = '';
      saveBtn.disabled = true;

      const {server} = creds.load();
      const base = new URLSearchParams({
         ...authParams(), v: '1.16.1', c: 'gaindrive-web', f: 'json'
         });

      try {
         const params = new URLSearchParams(base);
         params.set('username',    fUsername.value.trim());
         params.set('email',       fEmail.value.trim());
         params.set('adminRole',   fAdmin.checked   ? 'true' : 'false');
         params.set('uploadRole',  fUpload.checked  ? 'true' : 'false');
         params.set('castRole',    fCast.checked    ? 'true' : 'false');
         params.set('disabled',    fDisabled.checked ? 'true' : 'false');
         params.set('maxBitRate',  fMaxBitrate.value);

         if (isNew) {
            if (!fUsername.value.trim()) { formStatus.textContent = 'Username is required.'; saveBtn.disabled = false; return; }
            if (!fPassword.value)        { formStatus.textContent = 'Password is required for new users.'; saveBtn.disabled = false; return; }
            params.set('password', fPassword.value);
            const r = await fetch(`${server}/rest/createUser.view?${params}`);
            const d = await r.json();
            if (d['subsonic-response'].status !== 'ok')
               throw new Error(d['subsonic-response'].error?.message ?? 'Unknown error');
            }
         else {
            if (fPassword.value) params.set('password', fPassword.value);
            const r = await fetch(`${server}/rest/updateUser.view?${params}`);
            const d = await r.json();
            if (d['subsonic-response'].status !== 'ok')
               throw new Error(d['subsonic-response'].error?.message ?? 'Unknown error');
            }

         formStatus.textContent = 'Saved.';
         if (refresh) await refresh();
         // Update panel title if we just renamed (new user).
         h1.textContent = fUsername.value.trim();
         }
      catch (e) {
         showError('The action could not be completed. Please check your connection.');
         formStatus.textContent = `Error: ${e.message}`;
         }
      finally {
         saveBtn.disabled = false;
         }
      });
   }

// Human labels for root content types. An unknown type falls back to its own
// name capitalised, so a server that grows a new kind of root still renders a
// sensible segment without a client change.
const LIBRARY_MODE_LABELS = {
   artists:    'Artists',
   categories: 'Categories',
   uploads:    'Uploads',
   };

// The segments to offer, in display order: one per distinct root content type,
// plus Uploads when the user may upload. Derived rather than hardcoded — that
// is what keeps the toggle correct as roots change.
//
// Uploads cannot be conditioned on an uploads root actually existing:
// getMusicFolders deliberately omits it, since it is per-user space rather
// than shared library. Offering it on the role alone matches what the old
// My Uploads button did.
function libraryModes() {
   const seen = [];
   for (const f of musicFolders ?? [])
      if (f.contentType && !seen.includes(f.contentType))
         seen.push(f.contentType);
   seen.sort();   // stable order regardless of how the server listed them
   if (currentUser?.uploadRole || currentUser?.adminRole)
      seen.push('uploads');
   return seen;
   }

// Signature of the personal library as last rendered, and the timer watching
// for it to change. Both are module-level because viewArtists() destroys the
// upload bar's DOM when it re-renders, so a timer owned by that node would be
// orphaned mid-poll.
let uploadsSignature = '';
let uploadsPollTimer = null;

// What this server can fetch from a URL: null until asked, [] when the feature
// is unavailable — no handlers configured, or the tool they name is not
// installed. An empty list is what keeps the row undrawn, so the client never
// offers something the server would only refuse.
//
// Module-level for the same reason the timer above is: viewArtists() destroys
// the upload bar's DOM whenever it re-renders.
let urlHandlers    = null;
let fetchPollTimer = null;
// The last states seen, keyed by job id, so a job reaching 'done' is noticed
// once rather than re-triggering a re-render on every tick.
let fetchLastState = {};
// Which call to pollFetchJobs() a tick belongs to. One timer slot is shared by
// every render of the upload bar, and a tick captures its host node *before*
// awaiting getFetchJobs — so a tick from an earlier render can wake after a
// newer call has installed its own interval, render into a detached node and
// then clearInterval the timer it never owned.
let fetchPollGen   = 0;
// A fetch finished while the user was reading an album, so the uploads listing
// on pane 0 is stale. Not redrawn there and then: viewArtists() slides back to
// pane 0, which would yank the album out from under them. Deferred to the next
// return to pane 0 rather than to "a later tick" — there is no later tick,
// because the poll stops on the very tick that sees the last job end.
let uploadsStale   = false;

// Whose uploads the Uploads mode shows.
//
// An admin gets everybody's, because an admin is the only account that can
// promote one into the shared library — without this a non-admin's upload is
// stranded, visible to its owner and to nobody who can act on it.
//
// The server groups the listing by owner for '*' instead of by first letter, so
// the index headings become usernames and the rendering needs no change at all:
// `index.name` was always just a label.
function uploadsScope() {
   return currentUser?.adminRole ? '*' : 'true';
   }

// Debounce for the "do I already have this?" check under the name fields.
let _dupeTimer = null;

// Every top-level name the library already knows, filled into [list] as it
// arrives and returned as a promise of a Map for the checks below.
//
// **Every slice, not the one being filed into.** A name that exists under a
// categories root is just as much a name already in the library as one under an
// artists root, and completing against only half of them would offer a fresh
// spelling of something that is already there. Staging is included too and
// marked as such: something fetched a fortnight ago and never promoted is the
// likeliest duplicate of all, and it is the one no library listing would show.
//
// The map is name → {id, label}, lowercased for lookup, first slice winning —
// the same "registry order decides" tie-break used everywhere a merge happens.
function loadNameSuggestions(list) {
   const types = [...new Set((musicFolders ?? [])
      .map(f => f.contentType).filter(Boolean))];
   // A server naming no kinds still has one library to complete against, and
   // sending no contentType is what asks for all of it.
   const slices = types.length ? types.map(t => ({params: {contentType: t}, label: t}))
                              : [{params: {}, label: 'the library'}];
   slices.push({params: {personal: 'true'}, label: 'staging'});

   return Promise.all(slices.map(async slice => {
      // A slice that fails is dropped, not reported: this is completion, and
      // half a list is worth more than an error where a suggestion should be.
      try {
         const r = await apiCall('getArtists', slice.params);
         return (r.artists?.index ?? []).flatMap(i =>
            i.artist.map(a => ({name: a.name, id: a.id, label: slice.label})));
         }
      catch { return []; }
      })).then(perSlice => {
         const known = new Map();
         for (const entry of perSlice.flat()) {
            const key = entry.name.toLowerCase();
            if (known.has(key)) continue;
            known.set(key, entry);
            const opt = document.createElement('option');
            opt.value = entry.name;
            // Shown as secondary text where the browser supports it, ignored
            // where it does not — the value is what gets inserted either way.
            opt.label = entry.label;
            list.appendChild(opt);
            }
         return known;
         });
   }

// What the library already holds under these names, as one line of prose, or ''.
//
// Strongest signal only. Three separate notes stacked under two text boxes is
// noise, and the strongest one subsumes the others: knowing the album is
// already there makes "that artist exists" beside the point.
async function checkExisting(knownPromise, artist, album) {
   const known = await knownPromise;
   const hit   = artist ? known.get(artist.toLowerCase()) : null;
   const where = hit && hit.label === 'staging'
      ? 'already in your uploads, not yet promoted'
      : hit && `already in ${hit.label}`;

   if (hit && album) {
      // That artist's own albums, which is the precise question — and cheap,
      // because it is one request against an id we already have.
      try {
         const r = await apiCall('getArtist', {id: hit.id});
         const match = (r.artist?.album ?? [])
            .find(a => (a.name ?? a.title ?? '').toLowerCase() === album.toLowerCase());
         if (match) return `You already have “${match.name ?? match.title}” under ` +
                           `${hit.name} — ${where}.`;
         }
      catch { /* fall through to the weaker signals */ }
      }
   if (hit) return `“${hit.name}” is ${where}.`;

   // Nothing matched by name, which is the case worth searching for: the same
   // record filed under a spelling you would not have typed. Searched on the
   // album rather than the artist because that is the distinctive string.
   if (album.length >= 3) {
      try {
         const r = await apiCall('search3',
            {query: album, artistCount: 0, albumCount: 3, songCount: 0});
         const found = r.searchResult3?.album ?? [];
         if (found.length) {
            const first = found[0];
            return `Possibly already there: “${first.name ?? first.title}”` +
                   (first.artist ? ` by ${first.artist}` : '') +
                   (found.length > 1 ? ` and ${found.length - 1} more.` : '.');
            }
         }
      catch { /* a search that fails is not worth reporting here */ }
      }
   return '';
   }

// name+albumCount per artist, not the artist count: a second upload usually
// adds an album to an artist who is already listed, which leaves the count
// untouched.
function personalSignature(indexes) {
   return (indexes ?? [])
      .flatMap(i => i.artist.map(a => `${a.name}:${a.albumCount}`))
      .join('|');
   }

// The upload response returns before the server has scanned — scan_dirs() runs
// on a detached thread — so there is nothing to show at the moment of success.
// Poll until the listing actually changes rather than re-rendering once into
// the same list.
function pollForUpload(status, files) {
   let tries = 0;
   clearInterval(uploadsPollTimer);
   uploadsPollTimer = setInterval(async () => {
      // The bar is gone once anything else has rewritten pane 0 — another
      // library mode, or Settings/Playlists/Recents — and a re-render then
      // would drag the user back here.
      if (!document.querySelector('#pane-artists .upload-bar')) {
         clearInterval(uploadsPollTimer);
         return;
         }
      let sr;
      // The same scope viewArtists() drew with, or the signature it is compared
      // against would be of a different list.
      try { sr = await apiCall('getArtists', {personal: uploadsScope()}); }
      catch { return; }   // a blip should not end the wait
      // viewArtists() slides back to pane 0, so hold off while the user is
      // reading an album; the try count still bounds the wait.
      if (personalSignature(sr.artists?.index) !== uploadsSignature
          && paneNav.depth === 0) {
         clearInterval(uploadsPollTimer);
         viewArtists();
         return;
         }
      if (++tries >= 10) {
         clearInterval(uploadsPollTimer);
         status.textContent =
            `Extracted ${files} file(s); the server is still scanning them.`;
         }
      }, 2000);
   }

// Watches the server's URL-fetch jobs and redraws their rows.
//
// Deliberately not pollForUpload(). That one watches the *listing* change,
// which is the right signal for an archive already on disk and waiting only for
// a scan, and its ten tries at two seconds are nowhere near long enough for a
// download. Here the server scans before it reports 'done', so 'done' already
// means the library is correct and the listing can simply be re-drawn.
function pollFetchJobs() {
   clearInterval(fetchPollTimer);
   // Each call takes a generation, and a tick that finds itself outdated returns
   // without touching the shared timer. See fetchPollGen.
   const gen = ++fetchPollGen;

   const tick = async () => {
      if (gen !== fetchPollGen) return;
      // Gone once anything else has rewritten pane 0 — another library mode, or
      // Settings/Playlists/Recents. Same guard pollForUpload() uses, and for the
      // same reason: a re-render then would drag the user back here.
      let host = document.querySelector('#pane-artists .fetch-jobs');
      if (!host) { clearInterval(fetchPollTimer); return; }

      let r;
      try { r = await apiCall('getFetchJobs'); }
      catch { return; }   // a blip should not end the wait
      if (gen !== fetchPollGen) return;
      // Looked up again after the await: a re-render during the request has
      // replaced the node found above, and writing into the detached one is
      // silent — the rows are built, appended to nothing, and never seen.
      host = document.querySelector('#pane-artists .fetch-jobs');
      if (!host) { clearInterval(fetchPollTimer); return; }
      const jobs = r.fetchJobs?.fetchJob ?? [];

      host.innerHTML = '';
      let live = false, finished = false;
      for (const j of jobs) {
         if (j.state === 'queued' || j.state === 'running'
             || j.state === 'scanning')
            live = true;
         if (j.state === 'done' && fetchLastState[j.id] !== 'done')
            finished = true;
         fetchLastState[j.id] = j.state;

         const row = document.createElement('div');
         row.className = 'fetch-job';

         const head = document.createElement('div');
         head.className = 'fetch-head';
         const name = document.createElement('span');
         name.className = 'fetch-name';
         // The typed names when there were any, so a queued job says what it
         // will be filed as rather than only which tool is doing it.
         name.textContent = (j.artist || j.album)
            ? `${j.artist || '…'} · ${j.album || '…'}`
            : `${j.handler} · ${j.mode}`;
         head.appendChild(name);
         const state = document.createElement('span');
         state.className = 'fetch-state';
         state.textContent = j.state === 'done'
            ? `done — ${j.files} file(s)`
            : (j.state === 'error' ? (j.error || 'error') : j.state);
         head.appendChild(state);
         if (j.state === 'queued' || j.state === 'running') {
            const cancel = document.createElement('button');
            cancel.className   = 'fetch-cancel';
            cancel.textContent = 'Cancel';
            cancel.addEventListener('click', async () => {
               cancel.disabled = true;
               try { await apiCall('cancelFetch', {id: j.id}); } catch {}
               });
            head.appendChild(cancel);
            }
         row.appendChild(head);

         if (j.state === 'running' || j.state === 'scanning') {
            const bar = document.createElement('progress');
            bar.max   = 100;
            bar.value = j.percent ?? 0;
            row.appendChild(bar);
            }
         if (j.detail) {
            const d = document.createElement('p');
            d.className   = 'fetch-detail';
            d.textContent = j.detail;
            row.appendChild(d);
            }
         host.appendChild(row);
         }

      // viewArtists() slides back to pane 0, so hold off while the user is
      // reading an album.
      //
      // Deferred to a flag rather than to a later tick, which is what this used
      // to claim and could not do: `finished` is derived from fetchLastState,
      // which the loop above has already overwritten with 'done', and the poll
      // stops two lines below because nothing is live any more. So the listing
      // was never redrawn at all for anyone who was deeper in when a fetch
      // landed. pollForUpload() gets away with the same shape only because its
      // trigger — a changed listing signature — is not consumed by reading it.
      if (finished) {
         if (paneNav.depth === 0) {
            clearInterval(fetchPollTimer);
            viewArtists();
            return;
            }
         uploadsStale = true;
         }
      if (!live) clearInterval(fetchPollTimer);
      };

   fetchPollTimer = setInterval(tick, 2000);
   // Deferred rather than called: makeUploadBar() starts the poll while its own
   // node is still detached, so a tick right now would find no .fetch-jobs and
   // stop the interval it just set. A macrotask is enough — the caller appends
   // the fragment before yielding.
   setTimeout(tick, 0);
   }

// Landing back on pane 0 without re-rendering it — which is what Back does,
// since viewAlbums() only ever writes pane 1 and the popstate handler slides
// rather than redraws when pane 0 already has children.
//
// Two things the slide alone does not do. A batch that landed while the user
// was deeper in leaves the listing stale, and the poll that would have noticed
// a *new* job stopped itself the moment nothing was live — so a fetch started
// meanwhile, here or in another client, is never picked up. Restarting it costs
// one getFetchJobs and it stops itself again if there is nothing to watch.
async function returnedToArtists() {
   if (uploadsStale) { await viewArtists(); return; }   // which clears the flag
   if (libraryMode === 'uploads'
       && document.querySelector('#pane-artists .fetch-jobs'))
      pollFetchJobs();
   }

// The archive-upload form, drawn at the top of the Uploads listing. Returns the
// node rather than appending it, so the caller places it.
function makeUploadBar() {
   const bar = document.createElement('div');
   bar.className = 'upload-bar';

   const row = document.createElement('div');
   row.className = 'upload-row';
   bar.appendChild(row);

   const fileInput = document.createElement('input');
   fileInput.type   = 'file';
   fileInput.accept = '.zip,.tar,.tar.gz,.tgz';
   row.appendChild(fileInput);

   const uploadBtn = document.createElement('button');
   uploadBtn.textContent = 'Upload';
   uploadBtn.className   = 'upload-btn';
   row.appendChild(uploadBtn);

   const hint = document.createElement('p');
   hint.className   = 'admin-hint';
   hint.textContent = 'Music archive: zip, tar, tar.gz';
   // Says where an upload actually goes, because "Uploads" is a place people
   // reasonably expect to be the library itself. It is not: an admin has to
   // move it, and until then only this account can see it.
   const stagingHint = document.createElement('p');
   stagingHint.className   = 'admin-hint';
   stagingHint.textContent =
      'Everything here stays in your own uploads until an admin moves it into '
      + 'the shared library. The names below apply to both an archive and a URL.';
   bar.appendChild(stagingHint);
   bar.appendChild(hint);

   // Created here but appended below the URL row: both producers report through
   // the same status line, and it reads as belonging to whichever was used last
   // only if it sits under both of them.
   const progress = document.createElement('progress');
   progress.value  = 0;
   progress.max    = 100;
   progress.hidden = true;

   const uploadStatus = document.createElement('p');
   uploadStatus.className = 'upload-status';

   // ---- The names, which belong to both producers ----
   //
   // Above the URL row rather than inside it: an archive and a fetched URL are
   // the same batch by the time the server normalises them, and both take the
   // same overrides. Having these only under the URL box was the earlier shape
   // and made an uploaded zip the one thing that could not be named.
   //
   // A row of their own rather than two more fields beside the URL: .upload-row
   // is one flex line, and a URL box squeezed between two name boxes and a
   // button is unusable on a phone.
   //
   // The names override whatever the source would have called this — for a
   // fetch, what the handler parsed out of the video title; for an archive, its
   // own folders and the files' tags. Blank keeps that, which is why neither
   // field is marked required.
   const nameRow = document.createElement('div');
   nameRow.className = 'upload-row fetch-names';
   bar.appendChild(nameRow);

   // Completion comes from the whole library, so `list` is shared: the point is
   // to stop a near-duplicate spelling of a name that already exists, and which
   // slice it exists in does not change that.
   const nameList = document.createElement('datalist');
   nameList.id = 'upload-name-list';
   bar.appendChild(nameList);

   // Sticky, unlike the URL: fetching six tracks off one concert should mean
   // typing the names once. That is also why they are not cleared on success —
   // only the URL is, since that one genuinely differs every time. The check
   // below is what keeps a stale name from being applied unnoticed.
   const makeNameInput = (key, placeholder, list) => {
      const el = document.createElement('input');
      el.type        = 'text';
      el.className   = 'token-input';
      el.placeholder = placeholder;
      el.value       = localStorage.getItem(key) || '';
      if (list) el.setAttribute('list', list);
      el.addEventListener('change', () =>
         localStorage.setItem(key, el.value.trim()));
      nameRow.appendChild(el);
      return el;
      };
   const artistInput = makeNameInput('gd_fetch_artist',
                                     'Artist or category (from the source if empty)',
                                     nameList.id);
   const albumInput  = makeNameInput('gd_fetch_album',
                                     'Album or name (from the source if empty)');

   const clearBtn = document.createElement('button');
   clearBtn.className   = 'name-clear';
   clearBtn.textContent = 'Clear';
   clearBtn.title       = 'Forget these names';
   clearBtn.addEventListener('click', () => {
      artistInput.value = albumInput.value = '';
      localStorage.removeItem('gd_fetch_artist');
      localStorage.removeItem('gd_fetch_album');
      dupeNote.textContent = '';
      });
   nameRow.appendChild(clearBtn);

   // What the library already has under these names. Advisory and never
   // blocking: nothing can collide here — the batch is a fresh UUID directory —
   // and the destination root is not chosen until an admin promotes it, so
   // whether that will be refused is genuinely unpredictable from here.
   const dupeNote = document.createElement('p');
   dupeNote.className = 'upload-dupe';
   bar.appendChild(dupeNote);

   const known = loadNameSuggestions(nameList);
   // Debounced the way the search box is, with a module-level timer rather than
   // a generic helper — same reason, which is that every keystroke would
   // otherwise be a request.
   const recheck = () => {
      clearTimeout(_dupeTimer);
      _dupeTimer = setTimeout(() => {
         checkExisting(known, artistInput.value.trim(), albumInput.value.trim())
            .then(msg => { dupeNote.textContent = msg; })
            .catch(() => { dupeNote.textContent = ''; });
         }, 400);
      };
   artistInput.addEventListener('input', recheck);
   albumInput.addEventListener('input', recheck);
   // Immediately too, because the fields arrive already filled from last time
   // and a stale name is exactly what this is here to surface.
   recheck();

   // The URL row, drawn only when the server says it can fetch something. An
   // empty handler list is the server saying the feature is unavailable — no
   // handlers configured, or the tool they name is not installed — and offering
   // a box that can only be refused would be worse than offering nothing.
   if (urlHandlers?.length) {
      const urlRow = document.createElement('div');
      urlRow.className = 'upload-row';
      bar.appendChild(urlRow);

      const urlInput = document.createElement('input');
      urlInput.type        = 'url';
      urlInput.className   = 'token-input';
      urlInput.placeholder = 'https://…';
      urlRow.appendChild(urlInput);

      // Only offered when some handler can actually do both; with one mode
      // available there is no choice to make and a toggle would be furniture.
      const canAudio = urlHandlers.some(h => h.audio);
      const canVideo = urlHandlers.some(h => h.video);
      let fetchMode = localStorage.getItem('gd_fetch_mode') || 'audio';
      if (fetchMode === 'audio' && !canAudio) fetchMode = 'video';
      if (fetchMode === 'video' && !canVideo) fetchMode = 'audio';

      if (canAudio && canVideo) {
         // The same segmented control the library modes use — it is the same
         // kind of choice, and it already has an active state.
         const seg = document.createElement('div');
         seg.className = 'library-modes';
         for (const m of ['audio', 'video']) {
            const btn = document.createElement('button');
            btn.className = 'library-mode' + (m === fetchMode ? ' active' : '');
            btn.textContent = m === 'audio' ? 'Audio' : 'Video';
            btn.addEventListener('click', () => {
               fetchMode = m;
               localStorage.setItem('gd_fetch_mode', m);
               for (const b of seg.children)
                  b.classList.toggle('active', b === btn);
               });
            seg.appendChild(btn);
            }
         urlRow.appendChild(seg);
         }

      const fetchBtn = document.createElement('button');
      fetchBtn.textContent = 'Fetch';
      fetchBtn.className   = 'upload-btn';
      urlRow.appendChild(fetchBtn);

      const names = urlHandlers.map(h => h.name).join(', ');
      const urlHint = document.createElement('p');
      urlHint.className   = 'admin-hint';
      urlHint.textContent =
         `Or paste a URL — handled by: ${names}. `
         + 'Leave the names above blank to use the ones the site supplies.';
      bar.appendChild(urlHint);

      const submit = async () => {
         const url = urlInput.value.trim();
         if (!url) { uploadStatus.textContent = 'No URL entered.'; return; }
         fetchBtn.disabled = true;
         uploadStatus.textContent = '';
         try {
            // An untouched field sends nothing at all rather than an empty
            // value: the request line carries the URL already, and the server
            // is entitled to treat a present-but-empty name as a mistake.
            const p = {url, mode: fetchMode};
            const a = artistInput.value.trim();
            const b = albumInput.value.trim();
            if (a) p.artist = a;
            if (b) p.album  = b;
            // apiCall, not XHR: there is no upload progress to report here —
            // the server does the fetching, and getFetchJobs reports on it.
            await apiCall('fetchUrl', p);
            // Only the URL is cleared, so a re-render cannot resurrect a stale
            // one into a second fetch. The names deliberately survive.
            urlInput.value = '';
            pollFetchJobs();
            }
         catch (e) {
            uploadStatus.textContent = `Error: ${e.message ?? e}`;
            }
         fetchBtn.disabled = false;
         };
      fetchBtn.addEventListener('click', submit);
      // Enter submits from any of the three, since artist → album → Enter is
      // the natural way to fill this in.
      for (const el of [urlInput, artistInput, albumInput])
         el.addEventListener('keydown', e => {
            if (e.key === 'Enter') submit();
            });
      }

   bar.appendChild(progress);
   bar.appendChild(uploadStatus);

   uploadBtn.addEventListener('click', () => {
      const file = fileInput.files[0];
      if (!file) { uploadStatus.textContent = 'No file selected.'; return; }

      const {server} = creds.load();
      const p = new URLSearchParams({
         ...authParams(), v: '1.16.1', c: 'gaindrive-web', f: 'json'
         });
      // The same rule the fetch uses: an untouched field sends nothing at all
      // rather than an empty value, because the server is entitled to treat a
      // present-but-empty name as a mistake. Query parameters rather than form
      // fields — the multipart body is the archive, and the handler reads these
      // from req.params.
      const a = artistInput.value.trim();
      const b = albumInput.value.trim();
      if (a) p.set('artist', a);
      if (b) p.set('album',  b);
      const url = `${server}/upload?${p}`;

      const fd = new FormData();
      fd.append('file', file);

      const xhr = new XMLHttpRequest();
      xhr.open('POST', url);

      clearInterval(uploadsPollTimer);
      progress.hidden = false;
      progress.value  = 0;
      uploadStatus.textContent = '';

      xhr.upload.onprogress = e => {
         if (e.lengthComputable)
            progress.value = Math.round(e.loaded / e.total * 100);
         };

      xhr.onload = () => {
         progress.hidden = true;
         try {
            const j = JSON.parse(xhr.responseText);
            if (j.status === 'ok') {
               uploadStatus.textContent = `Extracted ${j.files} file(s); scanning…`;
               pollForUpload(uploadStatus, j.files);
               }
            else
               uploadStatus.textContent = `Error: ${j.message}`;
            }
         catch {
            uploadStatus.textContent = `Unexpected response (HTTP ${xhr.status}).`;
            }
         };

      xhr.onerror = () => {
         progress.hidden = true;
         uploadStatus.textContent = 'Network error during upload.';
         };

      xhr.send(fd);
      });

   if (urlHandlers?.length) {
      const jobs = document.createElement('div');
      jobs.className = 'fetch-jobs';
      bar.appendChild(jobs);
      // Unconditionally, not only after a Fetch: a page reload part-way through
      // a ten-minute download would otherwise show nothing at all.
      pollFetchJobs();
      }

   return bar;
   }

async function viewArtists() {
   // Whatever a finished fetch left behind is about to be re-read, however the
   // user got here — so the deferred redraw is owed to nobody any more.
   uploadsStale = false;
   const pane = document.getElementById('pane-artists');
   pane.innerHTML = '';
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   // Roots are static for the life of the server; fetch once.
   if (musicFolders === null) {
      try {
         const mf = await apiCall('getMusicFolders');
         musicFolders = mf.musicFolders?.musicFolder ?? [];
         }
      catch { musicFolders = []; }
      }

   // Static for the life of the server, like the roots above. Asked only when
   // the user could act on the answer, so an account with no upload rights
   // never sends the request at all.
   if (urlHandlers === null
       && (currentUser?.uploadRole || currentUser?.adminRole)) {
      try {
         const r = await apiCall('getUrlHandlers');
         urlHandlers = r.urlHandlers?.urlHandler ?? [];
         }
      catch { urlHandlers = []; }
      }

   // Fall back if the stored mode is no longer offered — a root may have been
   // removed, or upload rights revoked, since it was chosen.
   const modes = libraryModes();
   if (modes.length && !modes.includes(libraryMode)) libraryMode = modes[0];
   console.log('[library] loading, mode=', libraryMode);

   let sr;
   try {
      sr = await apiCall('getArtists',
         libraryMode === 'uploads' ? {personal: uploadsScope()}
                                   : {contentType: libraryMode});
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }

   const indexes = sr.artists?.index ?? [];
   console.log('[artists] got', indexes.reduce((n, i) => n + i.artist.length, 0), 'artists');

   if (libraryMode === 'uploads') uploadsSignature = personalSignature(indexes);

   const frag = document.createDocumentFragment();
   const header = document.createElement('div');
   header.className = 'view-header';
   const title = document.createElement('h1');
   title.className = 'view-title';
   title.textContent = 'Library';
   header.appendChild(title);

   // Segmented control, one segment per available mode. Hidden entirely when
   // there is only one — a music-only server with no upload rights then looks
   // exactly as it did before this control existed.
   //
   // It goes *inside* the header, which is position:sticky, so it stays put
   // while the list scrolls under it.
   if (modes.length > 1) {
      const seg = document.createElement('div');
      seg.className = 'library-modes';
      for (const m of modes) {
         const btn = document.createElement('button');
         btn.className = 'library-mode' + (m === libraryMode ? ' active' : '');
         btn.textContent = LIBRARY_MODE_LABELS[m]
            ?? (m.charAt(0).toUpperCase() + m.slice(1));
         btn.addEventListener('click', () => {
            if (m === libraryMode) return;
            libraryMode = m;
            localStorage.setItem('gd_library_mode', m);
            viewArtists();
            });
         seg.appendChild(btn);
         }
      header.appendChild(seg);
      }

   frag.appendChild(header);

   // The form belongs to the library it fills, so it is drawn only in that
   // mode. Role-gated rather than root-gated for the same reason
   // libraryModes() is: getMusicFolders omits the uploads root, so the client
   // cannot tell whether one is configured — the server says so on submit.
   // The role test is not redundant with libraryModes(): libraryMode is
   // restored from localStorage, and the fallback above only fires when some
   // other mode is on offer, so revoked upload rights can still land here.
   if (libraryMode === 'uploads'
       && (currentUser?.uploadRole || currentUser?.adminRole))
      frag.appendChild(makeUploadBar());

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
            const isCategory = libraryMode === 'categories';
            if (paneNav.willSlide(1))
               history.pushState({view: 'albums', artistId: artist.id,
                                  artistName: artist.name, isCategory}, '');
            viewAlbums(artist.id, artist.name, isCategory);
            });
         frag.appendChild(row);
         }
      }
   pane.appendChild(frag);
   paneNav.slideTo(0);
}

async function viewPlaylists() {
   console.log('[playlists] loading');
   const pane = document.getElementById('pane-artists');
   pane.innerHTML = '';
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   let sr;
   try {
      sr = await apiCall('getPlaylists');
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }
   const lists = sr.playlists?.playlist ?? [];
   console.log('[playlists] got', lists.length, 'playlists');

   const frag = document.createDocumentFragment();
   const header = document.createElement('div');
   header.className = 'view-header';
   const title = document.createElement('h1');
   title.className = 'view-title';
   title.textContent = 'Playlists';
   header.appendChild(title);
   frag.appendChild(header);

   for (const pl of lists) {
      const row = document.createElement('div');
      row.className = 'playlist-row';
      row.dataset.id = pl.id;

      const name = document.createElement('span');
      name.className = 'playlist-name';
      name.textContent = pl.name;

      const meta = document.createElement('span');
      meta.className = 'playlist-meta';
      const parts = [`${pl.songCount} tracks`];
      if (pl.duration) parts.push(fmtDuration(pl.duration));
      meta.textContent = parts.join(' · ');

      row.appendChild(name);
      row.appendChild(meta);
      row.addEventListener('click', () => {
         document.querySelectorAll('#pane-artists .playlist-row.selected')
            .forEach(r => r.classList.remove('selected'));
         row.classList.add('selected');
         if (paneNav.willSlide(1))
            history.pushState({view: 'playlist-tracks', playlistId: pl.id, playlistName: pl.name}, '');
         viewPlaylistTracks(pl.id, pl.name);
         });
      frag.appendChild(row);
      }
   pane.appendChild(frag);

   // ── Starred albums and tracks ───────────────────────────────────────────
   const starredContainer = document.createElement('div');
   starredContainer.id = 'starred-sections';
   pane.appendChild(starredContainer);
   await refreshStarredSections(starredContainer);

   paneNav.slideTo(0);
}

async function viewRecents() {
   console.log('[recents] loading');
   const pane = document.getElementById('pane-artists');
   pane.innerHTML = '';
   document.getElementById('pane-albums').innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   let sr;
   try {
      sr = await apiCall('getRecentSongs', {size: 50});
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }

   const songs = sr.recentSongs?.song ?? [];
   console.log('[recents] got', songs.length, 'songs');

   const frag = document.createDocumentFragment();
   const hdr = document.createElement('div');
   hdr.className = 'view-header';
   const h1 = document.createElement('h1');
   h1.className = 'view-title';
   h1.textContent = 'Recents';
   hdr.appendChild(h1);
   frag.appendChild(hdr);

   if (songs.length === 0) {
      const msg = document.createElement('p');
      msg.style.color = 'var(--text-dim)';
      msg.style.padding = '1rem';
      msg.textContent = 'No recently played songs.';
      frag.appendChild(msg);
      } else {
      for (const song of songs) {
         const row = document.createElement('div');
         row.className = 'search-song-row';

         // .recent-cover shrinks the box to 40px, so this over-fetches by a
         // factor of two on top of coverPx's. Left alone: the list is a
         // handful of rows and the cost of splitting makeAlbumCover in two is
         // more than the bytes.
         const cover = makeAlbumCover({id: song.parent, coverArt: song.coverArt});
         cover.classList.add('recent-cover');
         row.appendChild(cover);

         const info = document.createElement('div');
         info.className = 'search-song-info';

         const titleEl = document.createElement('span');
         titleEl.className = 'search-song-title';
         titleEl.textContent = song.title;

         const sub = document.createElement('span');
         sub.className = 'search-song-sub';
         const parts = [];
         if (song.artist) parts.push(song.artist);
         if (song.album)  parts.push(song.album);
         sub.textContent = parts.join(' · ');

         info.appendChild(titleEl);
         info.appendChild(sub);
         row.appendChild(info);

         if (song.lastPlayed) {
            const ago = document.createElement('span');
            ago.className = 'search-song-dur';
            ago.textContent = timeAgo(song.lastPlayed);
            row.appendChild(ago);
            }

         row.addEventListener('click', () =>
            viewTracksFromSearch(song.parent, song.album, null, song.artist, song.id));
         frag.appendChild(row);
         }
      }

   pane.appendChild(frag);
   paneNav.slideTo(0);
}

async function viewPlaylistTracks(playlistId, playlistName) {
   console.log('[playlist-tracks] loading playlist', playlistId, playlistName);
   const pane = document.getElementById('pane-albums');
   pane.innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   let sr;
   try {
      sr = await apiCall('getPlaylist', {id: playlistId});
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }
   const pl = sr.playlist;
   const songs = pl.entry ?? [];
   console.log('[playlist-tracks] got', songs.length, 'tracks');

   const header = document.createElement('div');
   header.className = 'view-header';
   const back = document.createElement('span');
   back.className = 'back-link';
   back.dataset.pane = '1';
   back.textContent = '← Playlists';
   back.addEventListener('click', () => history.back());
   const heading = document.createElement('h1');
   heading.className = 'view-title';
   heading.textContent = pl.name;
   const publicBadge = document.createElement('span');
   publicBadge.className = 'playlist-public-badge';
   publicBadge.textContent = 'Public';
   if (pl.public !== true) publicBadge.hidden = true;
   const isOwner = pl.owner && currentUser && pl.owner === currentUser.username;
   const editLink = isOwner ? document.createElement('span') : null;
   if (editLink) {
      editLink.className = 'edit-link';
      editLink.textContent = 'Edit';
      }
   header.appendChild(back);
   header.appendChild(heading);
   header.appendChild(publicBadge);
   if (editLink) header.appendChild(editLink);
   pane.appendChild(header);

   const commentP = document.createElement('p');
   commentP.className = 'playlist-comment';
   commentP.textContent = pl.comment ?? '';
   if (!pl.comment) commentP.hidden = true;
   pane.appendChild(commentP);

   const frag = document.createDocumentFragment();
   for (let i = 0; i < songs.length; i++) {
      const song = songs[i];
      const row = document.createElement('div');
      row.className = 'track-row';
      row.dataset.id  = song.id;
      row.dataset.dur = song.duration ? fmtDuration(song.duration) : '';

      const icon = document.createElement('span');
      icon.className = 'track-icon';

      const num = document.createElement('span');
      num.className = 'track-num';
      num.textContent = i + 1;

      // Title + artist stacked in a single cell.
      const titleWrap = document.createElement('span');
      titleWrap.className = 'track-title-wrap';
      const titleEl = document.createElement('span');
      titleEl.className = 'track-title';
      titleEl.textContent = song.title;
      const artistEl = document.createElement('span');
      artistEl.className = 'track-artist';
      artistEl.textContent = song.artist ?? '';
      titleWrap.appendChild(titleEl);
      if (song.artist) titleWrap.appendChild(artistEl);

      const dur = document.createElement('span');
      dur.className = 'track-dur';
      dur.textContent = row.dataset.dur;

      icon.addEventListener('click', e => {
         e.stopPropagation();
         playerEnqueue(songs[i]);
         row.classList.add('queued');
         });
      row.addEventListener('click', () => {
         player.albumCtx = {
            albumId:    song.parent,
            albumTitle: song.album ?? '',
            artistId:   null,
            artistName: song.artist ?? '',
            };
         playerLoad(songs, i);
         });
      const [starBtn, listBtn] = makeTrackActions(song, {playlistId, playlistName, index: i});
      row.appendChild(icon);
      row.appendChild(num);
      row.appendChild(titleWrap);
      row.appendChild(starBtn);
      row.appendChild(listBtn);
      row.appendChild(dur);
      frag.appendChild(row);
      }
   pane.appendChild(frag);
   paneNav.slideTo(1);

   // ── Edit mode ──────────────────────────────────────────────────────────────
   // Only the playlist's owner sees the Edit link; the server enforces the
   // same rule, so non-owners never get a working button.
   if (!editLink) return;

   function enterEditMode() {
      editLink.textContent = '';
      const saveBtn = document.createElement('button');
      saveBtn.className = 'edit-save-btn';
      saveBtn.textContent = 'Save';
      const cancelBtn = document.createElement('button');
      cancelBtn.className = 'edit-cancel-btn';
      cancelBtn.textContent = 'Cancel';
      editLink.appendChild(saveBtn);
      editLink.appendChild(cancelBtn);

      // Title → input.
      const nameInput = document.createElement('input');
      nameInput.type = 'text';
      nameInput.className = 'playlist-name-input';
      nameInput.value = heading.textContent;
      nameInput.dataset.orig = heading.textContent;
      heading.replaceWith(nameInput);

      // Public badge → checkbox toggle.
      const toggleLabel = document.createElement('label');
      toggleLabel.className = 'playlist-public-toggle';
      const toggle = document.createElement('input');
      toggle.type = 'checkbox';
      toggle.className = 'playlist-public-input';
      toggle.checked = pl.public === true;
      toggle.dataset.orig = toggle.checked ? 'true' : 'false';
      toggleLabel.appendChild(toggle);
      toggleLabel.appendChild(document.createTextNode('Public'));
      publicBadge.replaceWith(toggleLabel);

      // Comment → textarea (always shown in edit mode).
      const commentInput = document.createElement('textarea');
      commentInput.className = 'playlist-comment-input';
      commentInput.value = pl.comment ?? '';
      commentInput.dataset.orig = pl.comment ?? '';
      commentInput.placeholder = 'Comment';
      commentP.replaceWith(commentInput);

      saveBtn.addEventListener('click', async () => {
         saveBtn.disabled = true;
         cancelBtn.disabled = true;

         const params = {playlistId};
         if (nameInput.value !== nameInput.dataset.orig)
            params.name = nameInput.value;
         if (commentInput.value !== commentInput.dataset.orig)
            params.comment = commentInput.value;
         const newPublic = toggle.checked ? 'true' : 'false';
         if (newPublic !== toggle.dataset.orig)
            params.public = newPublic;

         try {
            if (Object.keys(params).length > 1)
               await apiCall('updatePlaylist', params);
            }
         catch (e) {
            console.error('[playlist-edit] updatePlaylist failed', e);
            saveBtn.disabled = false;
            cancelBtn.disabled = false;
            showError('Could not save playlist changes.');
            return;
            }

         // Update local cached playlist so re-entering edit mode sees the new
         // values, and propagate the rename to the playlists list row.
         pl.name    = nameInput.value;
         pl.comment = commentInput.value;
         pl.public  = toggle.checked;
         const listRow = document.querySelector(
            `#pane-artists .playlist-row[data-id="${playlistId}"] .playlist-name`);
         if (listRow) listRow.textContent = pl.name;

         exitEditMode(true, nameInput, toggleLabel, commentInput);
         });

      cancelBtn.addEventListener('click', () => {
         exitEditMode(false, nameInput, toggleLabel, commentInput);
         });
      }

   function exitEditMode(keepValues, nameInput, toggleLabel, commentInput) {
      editLink.textContent = 'Edit';

      const newName = keepValues ? nameInput.value : nameInput.dataset.orig;
      heading.textContent = newName;
      nameInput.replaceWith(heading);

      const toggle = toggleLabel.querySelector('.playlist-public-input');
      const isPublic = keepValues
         ? toggle.checked
         : (toggle.dataset.orig === 'true');
      publicBadge.hidden = !isPublic;
      toggleLabel.replaceWith(publicBadge);

      const newComment = keepValues
         ? commentInput.value
         : commentInput.dataset.orig;
      commentP.textContent = newComment;
      commentP.hidden = !newComment;
      commentInput.replaceWith(commentP);
      }

   editLink.addEventListener('click', e => {
      if (e.target === editLink) enterEditMode();
      });
}

// isCategory says this level-1 folder is a section of a categories root —
// Film, Series — rather than a performer. Such a folder has no biography and
// no portrait by construction: is_category_folder() on the server refuses the
// MusicBrainz lookup, so asking anyway only reserves a shimmer and an empty
// circle that never fill in.
//
// Passed in rather than read from libraryMode, for the reason Route.Albums on
// Android takes fromUploads: an artist id says which folder, never which
// section it was reached through. Search, and the sideways entries into
// viewTracks(), leave it defaulted and behave exactly as before.
async function viewAlbums(artistId, artistName, isCategory = false) {
   console.log('[albums] loading artist', artistId, artistName);
   const pane = document.getElementById('pane-albums');
   pane.innerHTML = '';
   document.getElementById('pane-tracks').innerHTML = '';

   let srArtist;
   try {
      srArtist = await apiCall('getArtist', {id: artistId});
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }
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
   // The label says the state, the tooltip says what pressing it does.
   const sortBtn = document.createElement('button');
   sortBtn.className = 'sort-btn';
   const labelSort = () => {
      const byName = albumSort.get() === 'name';
      sortBtn.textContent = byName ? 'Name' : 'Year';
      sortBtn.title = byName ? 'Sort by year' : 'Sort by name';
      };
   labelSort();
   const refreshBtn = document.createElement('button');
   refreshBtn.className = 'refresh-btn mi';
   refreshBtn.title = 'Reload artist info from MusicBrainz';
   refreshBtn.textContent = 'refresh';
   header.appendChild(back);
   header.appendChild(heading);
   header.appendChild(sortBtn);
   if (!isCategory) header.appendChild(refreshBtn);
   pane.appendChild(header);

   // Placeholder filled asynchronously once getArtistInfo2 responds.
   // The loading class reserves the same height as the collapsed bio block so
   // the album list does not jump when the bio arrives.
   // Kept as a detached-node reference so a stale callback can't corrupt a
   // pane that has already been reused for a different artist.
   // Not made at all for a category, which is the space this reserves and
   // nothing ever fills.
   const bioSlot = isCategory ? null : document.createElement('div');
   if (bioSlot) {
      bioSlot.className = 'artist-bio-loading';
      pane.appendChild(bioSlot);
      }

   // The rows are rebuilt in place when the sort is toggled, so the whole loop
   // lives in renderRows() rather than inline: each row carries the uploads-
   // mode Promote and Delete buttons and their handlers, and rows rebuilt any
   // other way would lose them.
   const list = document.createElement('div');
   list.className = 'album-list';
   pane.appendChild(list);

   function renderRows() {
      const frag = document.createDocumentFragment();
      for (const album of sortedAlbums(albums, albumSort.get())) {
         const row = document.createElement('div');
         row.className = 'album-row';
         row.dataset.id = album.id;

         const cover = makeAlbumCover(album);

         const info = document.createElement('div');
         info.className = 'album-info';

         const title = document.createElement('span');
         title.className = 'album-title';
         title.textContent = album.title;

         const meta = document.createElement('span');
         meta.className = 'album-meta';
         // A film or a season sits in the same list as a record, and the only
         // other warning is the picture taking over the screen. The count is in
         // the tooltip rather than the line: a folder with one bonus
         // documentary is not a folder of films, and the icon should not claim
         // it is.
         if (album.videoCount > 0) {
            const mark = document.createElement('span');
            mark.className = 'mi album-video';
            mark.textContent = 'movie';
            mark.title = album.videoCount === 1
               ? '1 video' : `${album.videoCount} videos`;
            meta.appendChild(mark);
            }
         const parts = [];
         if (album.year)      parts.push(album.year);
         // Pluralised like the artist row above, because a one-track album is
         // ordinary now: a loose file is its own album.
         if (album.songCount)
            parts.push(album.songCount === 1
               ? '1 track' : `${album.songCount} tracks`);
         if (parts.length) meta.appendChild(document.createTextNode(parts.join(' · ')));

         info.appendChild(title);
         info.appendChild(meta);
         row.appendChild(cover);
         row.appendChild(info);
         row.appendChild(makeAlbumStar(album));

         // Promote-to-library button (admin only, uploads mode only).
         if (libraryMode === 'uploads' && currentUser?.adminRole) {
            const promoteBtn = document.createElement('button');
            promoteBtn.className = 'promote-btn';
            promoteBtn.title = 'Move to shared library';
            promoteBtn.textContent = '→ Library';
            promoteBtn.addEventListener('click', e => {
               e.stopPropagation();
               // The dialog rather than a one-click move: the server requires a
               // destination root and folder, and there is nothing in an upload
               // that says which. It used to guess — the first artists root
               // declared, under the batch's own name — which could not reach a
               // categories root at all, so a film went into the music library and
               // was then looked up as a musical artist.
               //
               // artistName is the batch's own level-1 name, which the dialog
               // offers as the default under an artists root and leaves out under
               // a categories one, where it would be the channel rather than a
               // category.
               showPromoteDialog(album.title, artistName, async (rootId, folder) => {
                  promoteBtn.disabled = true;
                  promoteBtn.textContent = '…';
                  try {
                     // moveAlbum is the server's one mover; naming a root is what
                     // makes this a promote rather than a rename in place.
                     const sr = await apiCall('moveAlbum',
                        {id: album.id, musicFolderId: rootId, folder});
                     const moved = sr.movedAlbum ?? null;
                     // It is in the shared library now, so staying in Uploads
                     // would leave the user looking at the listing it just left.
                     // Switch to the destination root's own mode and walk in to
                     // where it landed — the ids to do that are what moveAlbum
                     // returns and promoteAlbum never did.
                     const destType = (musicFolders ?? [])
                        .find(f => String(f.id) === String(rootId))?.contentType;
                     if (moved?.id && moved?.parent && destType) {
                        libraryMode = destType;
                        localStorage.setItem('gd_library_mode', destType);
                        await viewArtists();
                        await viewAlbums(moved.parent, moved.artist,
                                         destType === 'categories');
                        await viewTracks(moved.id, moved.album, moved.parent,
                                         moved.artist);
                        }
                     else
                        viewArtists();
                     }
                  catch (err) {
                     promoteBtn.disabled = false;
                     promoteBtn.textContent = '→ Library';
                     // The server's own words: "already in that folder" and
                     // "outside the library" need different fixes, and a flat
                     // "Promote failed" told the user neither.
                     showError(err.message ?? 'Promote failed.');
                     }
                  });
               });
            row.appendChild(promoteBtn);
            }

         // Delete-from-uploads button. Uploads mode only, but *not* admin only,
         // unlike promote beside it: clearing out your own staging area after a
         // fetch went wrong is not an administrative act, and before this the only
         // way out of the uploads area was to promote into the shared library.
         if (libraryMode === 'uploads') {
            const deleteBtn = document.createElement('button');
            deleteBtn.className = 'promote-btn delete-btn';
            deleteBtn.title = 'Delete from your uploads';
            deleteBtn.textContent = 'Delete';
            deleteBtn.addEventListener('click', e => {
               e.stopPropagation();
               showConfirm(
                  `“${album.title}” and its files are removed from the server. `
                  + 'This cannot be undone.',
                  async () => {
                     deleteBtn.disabled = true;
                     deleteBtn.textContent = '…';
                     try {
                        await apiCall('deleteUpload', {id: album.id});
                        viewArtists();
                        }
                     catch (err) {
                        deleteBtn.disabled = false;
                        deleteBtn.textContent = 'Delete';
                        // The server's own words. "Item is not in your uploads"
                        // and a transport failure want different reactions, and a
                        // flat "Delete failed" distinguishes neither.
                        showError(err.message ?? 'Delete failed.');
                        }
                     },
                  {title: 'Delete from uploads?', yes: 'Delete'},
                  );
               });
            row.appendChild(deleteBtn);
            }

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
      list.replaceChildren(frag);
      }
   renderRows();

   sortBtn.addEventListener('click', () => {
      // Which album pane 2 is showing has to survive the reorder — the rows
      // are new nodes, so the highlight would otherwise be dropped on a
      // listing whose tracks are still on screen beside it.
      const sel = list.querySelector('.album-row.selected')?.dataset.id;
      albumSort.set(albumSort.get() === 'name' ? 'year' : 'name');
      labelSort();
      renderRows();
      if (sel)
         list.querySelector(`.album-row[data-id="${CSS.escape(sel)}"]`)
            ?.classList.add('selected');
      });

   // Marker so viewTracks() can tell whether pane 1 already shows this artist
   // and skip a redundant re-render when navigating artists → albums → tracks.
   pane.dataset.artistId = String(artistId);
   paneNav.slideTo(1);

   // Fetch artist info without blocking the album list.
   // Extracted into loadBio() so the refresh button can re-invoke with force=1.
   function loadBio(force) {
      bioSlot.className = 'artist-bio-loading';
      bioSlot.innerHTML = '';
      refreshBtn.disabled = true;
      const params = {id: artistId};
      if (force) params.force = '1';
      apiCall('getArtistInfo2', params).then(srInfo => {
         const info        = srInfo?.artistInfo2 ?? {};
         const bio         = info.biography ?? '';
         const wikiUrl     = info.wikiUrl ?? '';
         const allMusicUrl = info.allMusicUrl ?? '';

         bioSlot.className = '';   // remove shimmer regardless of outcome
         refreshBtn.disabled = false;

         // The portrait is served by us and is no longer conditional on what
         // this response says: the image and the words arrive from different
         // places now, so a bio with no picture still gets one when the
         // resolver catches up, and vice versa.
         const block = document.createElement('div');
         block.className = 'artist-bio';
         block.appendChild(
            artistPortrait(artistId, coverPx(120), 'artist-bio-img',
                           artistName));

         if (!bio && !wikiUrl && !allMusicUrl) {
            bioSlot.appendChild(block);   // the picture, with nothing to say
            return;
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
         }).catch(() => {
            bioSlot.className = '';
            refreshBtn.disabled = false;
            });   // server may not support getArtistInfo2
      }
   if (!isCategory) {
      refreshBtn.addEventListener('click', () => loadBio(true));
      loadBio(false);
      }
}

function fmtDuration(secs) {
   const m = Math.floor(secs / 60);
   const s = String(secs % 60).padStart(2, '0');
   return `${m}:${s}`;
}

function timeAgo(isoStr) {
   const mins = Math.floor((Date.now() - new Date(isoStr).getTime()) / 60000);
   if (mins < 60)  return `${mins}m ago`;
   const hrs = Math.floor(mins / 60);
   if (hrs  < 24)  return `${hrs}h ago`;
   return `${Math.floor(hrs / 24)}d ago`;
}

// ── Player ───────────────────────────────────────────────────────────────────

// Info for the currently logged-in user (populated in showShell).
let currentUser = null;

// Which kind of top-level entry the Library view is showing: a root content
// type ('artists', 'categories', …) or 'uploads' for the user's own files.
// A list must never contain more than one kind, so this is a single value
// rather than a set of flags — the server filters on it and the rendered list
// is single-kind by construction.
let libraryMode = localStorage.getItem('gd_library_mode') || 'artists';

// Roots as reported by getMusicFolders, fetched once. The client otherwise has
// no idea roots exist; the Library toggle is built from what is in here, so a
// server growing a new root type grows a new segment without a client change.
let musicFolders = null;

// Id of the currently active cast device, or null when not casting.
let castDeviceId        = null;
let castDeviceName      = '';    // friendly name, for the panel that replaces the picture
let castEventSrc        = null;   // EventSource receiving pushed status from server
let castStartOffset     = 0;      // timeOffset used when cast started (seconds)
let lastCastPosition    = 0;      // absolute position of last SSE push
let castWasPlaying      = false;  // true once the cast device has been seen playing
let castPlayerState     = 'IDLE'; // playerState from last SSE push (for interpolation)
let castBaseTime        = 0;      // s.currentTime from last SSE push
let castBaseAt          = 0;      // Date.now() (ms) when castBaseTime was recorded
let castSongDuration    = 0;      // total song duration; fallback when queue is not loaded
let castEndStallTime    = null;   // Date.now() when BUFFERING-at-end stall started
// True when the current LOAD sent the receiver a film's soundtrack rather than
// the film.  Purely a statement about the bytes on the wire — the info dialog
// and the "Preparing sound…" label are all that read it, since a screenless
// device may equally be sent the whole file.  Where the picture goes is
// castReceiverVideo below.
let castAudioOnly       = false;
// Whether a picture is appearing on the receiver.  Deliberately not
// !castAudioOnly: a screenless device set to `videoPref = send` is sent the
// whole film and shows none of it, so the two facts came apart.  Everything
// about *where the picture is* keys on this; castAudioOnly describes only what
// the server put on the wire, which is the info dialog's business.
//
// Defaults true so the first load of a session provisionally draws the panel,
// exactly as castAudioOnly = false used to.
let castReceiverVideo   = true;
// What the server said the receiver is actually being sent: contentType,
// container, bitrate and tier, as castLoad and castSession both report them.
// Null when nothing has been loaded this session.
//
// Held rather than recomputed because the decision is the server's — it is the
// one place holding both the song and the device, and the tier ladder it comes
// off lives in src/codecs.hh.  Working it out again here from the device list
// and the codec pair would be a second copy of that ladder in another
// language, which is the drift every predicate in codecs.hh warns about.
let castStream          = null;
// True while this session is showing the film here, muted, slaved to the
// receiver's clock — the mode castSyncTick() below drives.  Distinct from
// castReceiverVideo, which says whether a picture is appearing over there:
// this one can be declined by setting, and is impossible for an audio track.
let castVideoLocal      = false;
// Logged once per load rather than ten times a second, for the one case the
// loop cannot fix: a chunked stream that has drifted past what it has buffered.
let castSyncWarned      = false;
let castExpectedPosition = null;  // absolute position we asked the receiver to
                                  // seek to via the most recent LOAD; cleared
                                  // once the receiver reports playback near it

// Handle one MEDIA_STATUS push from the server SSE stream.
function onCastStatus(s) {
   // Server tells us where the served stream begins in the song.  Native
   // seek (MP3) keeps this at 0; server-side seek (FLAC/other) sets it to
   // the seek point so the absolute song position is startOffset + the
   // receiver's reported currentTime.
   if (typeof s.startOffset === 'number') castStartOffset = s.startOffset;
   // The stream description, repeated on every push.  castLoad's reply is the
   // *attempt*, and a receiver that refuses it gets a second, different load —
   // a film becoming its soundtrack — so the reply the client read can already
   // be describing something that is not playing.  Read unconditionally rather
   // than only on a change: it costs an assignment, and a version test would be
   // one more thing able to drift.
   if (typeof s.audioOnly === 'boolean') {
      castStream        = s;
      castAudioOnly     = s.audioOnly;
      castReceiverVideo = !!s.receiverShowsVideo;
      }
   // The receiver saying anything but IDLE means it has the stream, which for
   // an audio-only load is the end of a transcode that ran before the LOAD was
   // sent.  Cleared here, above the transient gate below: that gate drops the
   // statuses of the outgoing session, and a load that errors never produces a
   // matching one — the notice would then sit there for ever.
   if (s.playerState !== 'IDLE' || s.idleReason === 'ERROR')
      videoPreparing(false);
   // After every LOAD the receiver emits a transient sequence: an
   // IDLE/INTERRUPTED for the OLD media session (currentTime=0), then
   // BUFFERING/PLAYING with a small t for the NEW session, then finally
   // PLAYING at the real seek position.  Any of those would reset
   // castBaseTime and make the seek bar flash back to 0 (or near 0).
   // While castExpectedPosition is set we ignore anything more than a few
   // seconds away from it.  IDLE/FINISHED is the one IDLE we must NOT
   // drop — it drives the end-of-track auto-advance below.
   if (castExpectedPosition !== null) {
      if (s.playerState === 'IDLE') {
         if (s.idleReason !== 'FINISHED') return;
         } else {
         const absCurrent = castStartOffset + s.currentTime;
         if (Math.abs(absCurrent - castExpectedPosition) > 3) return;
         castExpectedPosition = null;
         }
      }

   castBaseTime    = s.currentTime;
   castBaseAt      = Date.now();
   castPlayerState = s.playerState;

   if (s.playerState === 'IDLE') {
      castEndStallTime = null;
      // Advance on a clean end-of-track (FINISHED) or on ERROR when the
      // last known position was within 10 s of the end — the Chromecast
      // sometimes raises IDLE/ERROR instead of IDLE/FINISHED for OGG/FLAC
      // streams whose HTTP connection closes without a recognised EOS frame.
      const song      = player.queue[player.index];
      const totalSecs = (song?.duration ?? 0) || castSongDuration;
      const nearEnd   = totalSecs > 0 && lastCastPosition >= totalSecs - 10;
      if ((s.idleReason === 'FINISHED' ||
           (s.idleReason === 'ERROR' && nearEnd)) &&
          castWasPlaying && player.index < player.queue.length - 1) {
         castWasPlaying   = false;
         castStartOffset  = 0;
         lastCastPosition = 0;
         scrobbleCurrentSong();
         player.index++;
         playerPlay();
         }
      return;
      }

   castWasPlaying = true;
   const absCurrent = castStartOffset + s.currentTime;
   lastCastPosition = absCurrent;

   // Detect the "stuck BUFFERING at t≈dur" Chromecast quirk: the receiver
   // parks in BUFFERING with currentTime==duration instead of going to
   // IDLE/FINISHED.  After 5 s of this, treat it as end-of-track.
   const song = player.queue[player.index];
   const totalSecs = (song?.duration ?? 0) || castSongDuration;
   if (totalSecs > 0 && absCurrent / totalSecs >= 0.5) scrobbleCurrentSong();
   if (s.playerState === 'BUFFERING' && totalSecs > 0 &&
       absCurrent >= totalSecs - 2) {
      if (castEndStallTime === null)
         castEndStallTime = Date.now();
      else if (Date.now() - castEndStallTime >= 5000 &&
               castWasPlaying &&
               player.index < player.queue.length - 1) {
         castEndStallTime = null;
         castWasPlaying   = false;
         castStartOffset  = 0;
         lastCastPosition = 0;
         scrobbleCurrentSong();
         player.index++;
         playerPlay();
         return;
         }
      } else {
      castEndStallTime = null;
      }
   const seek = document.getElementById('player-seek');
   if (!seek.dataset.seeking) {
      if (totalSecs > 0) seek.max = totalSecs;
      seek.value = Math.floor(absCurrent);
      }
   playerPlayGlyph(s.playerState === 'PAUSED' ? 'play_arrow' : 'pause');
   }

// Open (or re-open) the SSE connection for cast status events.
function startCastEvents() {
   if (castEventSrc) castEventSrc.close();
   const src = castEventSrc = new EventSource(apiUrl('castEvents'));
   src.onmessage = (e) => {
      try { onCastStatus(JSON.parse(e.data)); } catch (_) {}
      };
   // The session can end without this client asking: another of the user's
   // devices calls startCast and takes it over, and the server then drops this
   // stream. EventSource retries once, gets the 204 castEvents answers a
   // non-owner with, and stops for good.
   //
   // Without this handler nothing would notice. castDeviceId would stay set,
   // the cast button would stay lit, and the interpolation timer below would
   // go on advancing a seek bar for a session that is now someone else's —
   // which reads as the player having frozen rather than as having been taken
   // over. No local resume: the user is at another device, and starting audio
   // here would be a surprise.
   src.onerror = () => {
      // Only a permanent close. onerror also fires when the connection merely
      // dropped and EventSource is about to retry, which is readyState
      // CONNECTING and is what a momentary network blip looks like — acting on
      // that would throw the user out of cast mode for a hiccup. A takeover
      // reaches CLOSED, because the retry is answered 204 and the spec fails
      // the connection for good on any non-200.
      if (src.readyState !== EventSource.CLOSED) return;
      // A stale handler from a stream already replaced by a newer one has
      // nothing to say.
      if (castEventSrc !== src || castDeviceId === null) return;
      const where = castDeviceName ? ` on ${castDeviceName}` : '';
      castExit();
      videoSurfaceSet(null);
      showError(`The cast session${where} was taken over by another device.`);
      };
   }

// ── Picture here, sound on the receiver ──────────────────────────────────────
//
// When the receiver cannot show a film (see castReceiverVideo) the picture can
// stay in this player instead of being lost — whether the receiver was sent
// the soundtrack alone or the whole file it can only hear.  The two then have
// to be kept together, and the reason that is tractable at all is that **the
// local element is muted**: the correction knob is
// playbackRate, and a few percent on a picture with no sound is invisible.
// Rate-matching audio is what makes this hard everywhere else, and there is no
// audio here to pitch-shift.
//
// The receiver is the clock and the picture is the follower, never the other
// way round.  Nothing here can ask a Chromecast to speed up, and would not want
// to: the sound is what a listener notices.

const SYNC_DEAD = 0.03;   // s — inside this, leave the rate alone
const SYNC_HARD = 1.00;   // s — beyond this, jump rather than crawl
const SYNC_GAIN = 0.20;   // rate change per second of error
const SYNC_MAX  = 0.05;   // ±5%, comfortably below what an eye can see

// Whether the element can be moved to `t` at all.  A Range-capable stream can:
// the browser re-requests whatever it needs.  A re-encoded one is chunked with
// no Range support, so a seek outside what it can reach does nothing —
// silently, which is why this is a test and not an attempt.
//
// The test is `seekable` and not `buffered`, which is not a distinction without
// a difference: `seekable` is what the browser will honour, and for a chunked
// response of unknown duration it can be empty while `buffered` holds seconds
// of decoded video.  Asking the wrong one reports yes where the seek is then
// refused — and a refused seek fires no event and raises nothing, so it was
// invisible from here.  castSyncNoSeek latches on the read-back check in
// castSyncStepMove(), which is the only thing that ever finds out.
function castSyncCanSeek(el, t) {
   if (castSyncNoSeek) return false;
   if (!player.streamIsTranscoded) return true;
   for (let i = 0; i < el.seekable.length; i++)
      if (t >= el.seekable.start(i) && t <= el.seekable.end(i)) return true;
   return false;
}

// ---- The two adjustment buttons ---------------------------------------
//
// What this replaced was a slider feeding the loop's setpoint, and it could not
// be used.  Moving it changed `target` and nothing else, so a 25 ms move — the
// slider's own step — sat inside SYNC_DEAD and was discarded for ever, anything
// under a second crawled in over 5-15 s at ±5%, and only a move past SYNC_HARD
// produced the jump the control appeared to promise.  Nothing distinguished
// "settled" from "still moving", so the natural response to seeing nothing was
// to move it again, and overshoot.
//
// Two things fix it.  An adjustment is applied as a *step*, by the element,
// leaving the rate loop the job it was written for — absorbing drift over the
// length of a film — rather than being the mechanism by which a person's input
// arrives.  And the buttons are disabled until the picture has actually got
// there, so adjusting into an unsettled picture is not possible.

// Phase changes go through here, so the panel can never disagree with the
// machinery: videoSyncButton() is what greys the buttons out.
function castSyncSetPhase(phase) {
   castSyncPhase = phase;
   if (phase === 'settling') castSyncSettleAt = Date.now();
   videoSyncButton();
}

function castSyncDone() {
   if (castSyncPhase !== 'idle') castSyncSetPhase('idle');
}

// One press.  `dir` is +1 for "the sound is late" and -1 for "the sound is
// early" — the symptom, never the correction.  A positive delay holds the
// picture back, so late sound *increases* it; wired the other way round the
// control diverges under someone who is pressing correctly.
//
// A staircase rather than a bisection.  Bisection needs a bracket nobody has,
// so its first move would be a jump to the middle of the whole range, and one
// mistaken press near the end is unrecoverable.  Halving the step on every
// change of direction needs no bracket, converges in six to eight presses from
// anywhere, and a mistake is undone by the next press — which also refines it.
function castSyncAdjust(dir) {
   if (!castVideoLocal || castSyncPhase !== 'idle') return;
   if (castSyncLastDir && dir !== castSyncLastDir) {
      castSyncStep = Math.max(castSyncStep / 2, SYNC_STEP_MIN);
      castSyncRun  = 0;
      }
   else if (++castSyncRun >= 3) {
      // Halving alone is a one-way ratchet, so one mistaken press early on
      // would cap the step for the rest of the calibration and leave a long
      // haul to be walked in 25 ms increments.  Three presses the same way is
      // not homing in on anything — it is travelling — so let it coarsen again.
      castSyncStep = Math.min(castSyncStep * 2, SYNC_STEP_START);
      castSyncRun  = 0;
      }
   castSyncLastDir = dir;

   const was = castSyncDelay.get(castDeviceId);
   // The bounds the slider carried.  An amplifier's DSP puts the useful range
   // well to the positive side of zero.
   const want = Math.max(-1000, Math.min(3000, was + dir * castSyncStep));
   if (want === was) return;
   castSyncDelay.set(castDeviceId, want);
   videoSyncButton();
   console.log(`[cast] sync ${want} ms (step ${castSyncStep} ms)`);

   const el = player.videoEl;
   // Nothing is running, so there is nothing to step: castSyncTick()'s resume
   // branch plants the picture at the setting when the receiver next reports
   // PLAYING, which applies the adjustment by the other route.
   if (!el || !el.currentSrc || castPlayerState !== 'PLAYING' || el.paused)
      return;
   castSyncStepMove(el, (want - was) / 1000);
}

// Add `delta` seconds of delay to the picture, now, by moving the element.
//
// Self-consistent with the loop by construction: the setting moved `target` by
// -delta and this moves currentTime by -delta, so `err` is unchanged and there
// is nothing to undo when the loop resumes.
function castSyncStepMove(el, delta) {
   const to = Math.max(0, el.currentTime - delta);

   if (castSyncCanSeek(el, to)) {
      castSyncSetPhase('moving');
      let done = false;
      const finish = (ok) => {
         if (done) return;
         done = true;
         // A step outlives its stream if the cast ends mid-seek, and the
         // element has had its src removed by then.
         if (!castVideoLocal) { castSyncPhase = 'idle'; return; }
         el.removeEventListener('seeking', onSeeking);
         el.removeEventListener('seeked',  onSeeked);
         if (ok) { castSyncSetPhase('settling'); return; }
         // A seek the browser will not perform raises nothing and fires
         // nothing — the failure this control had no way of noticing.  Latch
         // it so the next press does not pay the timeout again, and take the
         // route that needs no seek.
         castSyncNoSeek = true;
         console.warn('[cast] sync: seek refused, falling back');
         castSyncPhase = 'idle';
         castSyncStepMove(el, delta);
         };
      // `seeking` is the acknowledgement and `seeked` the completion.  Waiting
      // for `seeked` alone would call a slow seek a refusal, since it does not
      // fire until the data is there.
      let accepted = false;
      const onSeeking = () => { accepted = true; };
      const onSeeked  = () => finish(Math.abs(el.currentTime - to) < 0.5);
      el.addEventListener('seeking', onSeeking);
      el.addEventListener('seeked',  onSeeked);
      setTimeout(() => { if (!accepted) finish(false); }, 200);
      // And a cap, or a seek that is accepted and never completes leaves the
      // buttons disabled for the rest of the film.
      setTimeout(() => finish(true), 5000);
      el.currentTime = to;
      return;
      }

   if (delta > 0) {
      // No seek available, but holding the picture still while the receiver
      // plays on *is* delaying it by that much: exact, and asking nothing of
      // the stream.  It is also what a viewer expects a positive adjustment to
      // look like — a brief freeze.
      castSyncSetPhase('moving');
      const from = el.currentTime;
      const at   = Date.now();
      el.pause();
      setTimeout(() => {
         if (!castVideoLocal) { castSyncPhase = 'idle'; return; }
         // What the freeze achieved, rather than what the timer was asked for.
         // setTimeout jitter and decode-resume latency are a few ms each; the
         // loop closes the remainder, so this only has to be reported.
         const got = (Date.now() - at) / 1000 - (el.currentTime - from);
         if (Math.abs(got - delta) > 0.005)
            console.log(`[cast] sync hold ${got.toFixed(3)} of ` +
                        `${delta.toFixed(3)} s`);
         el.play().catch(e => console.warn('[cast] local picture', e));
         castSyncSetPhase('settling');
         }, delta * 1000);
      return;
      }

   // Negative, on a stream that will not seek: there is nothing but the rate
   // loop, and it is allowed to be slow.  What it is not allowed to be is
   // invisible, which is the whole of what 'settling' buys.
   castSyncSetPhase('settling');
}

// One step of the loop, off the interpolation timer below.  `absCurrent` is
// the receiver's extrapolated position — the same value the seek bar is drawn
// from, so there is one clock here and not two.
function castSyncTick(absCurrent) {
   if (!castVideoLocal) return;
   const el = player.videoEl;
   if (!el || !el.currentSrc) return;

   // A deliberate step owns the element: it is mid-seek, or being held still to
   // add delay.  The loop must not touch playbackRate or call play() under it.
   if (castSyncPhase === 'moving') return;

   // The delay is the one quantity nothing can measure: what the receiver
   // reports is where its decoder is, and the sound leaves the speakers some
   // unknown time later.  Positive holds the picture back.
   const target = absCurrent - castSyncDelay.get(castDeviceId) / 1000;

   // Anything but PLAYING and there is no clock to follow.  This is also what
   // holds the picture still through the soundtrack transcode, which for a
   // feature film is the first minute of the session.
   if (castPlayerState !== 'PLAYING') {
      if (!el.paused) el.pause();
      // Nothing is advancing, so there is no error to close and nothing for the
      // buttons to wait on.  The resume branch below plants the picture at the
      // setting, which is the adjustment applied by another route.
      castSyncDone();
      return;
      }

   const localOffset = player.localOffset || 0;

   if (el.paused) {
      // Not `ended`: play() on a finished element restarts it from zero, and
      // the picture always runs out while the receiver is still reporting
      // PLAYING through the last seconds of the soundtrack.  The film would
      // begin again under it.  The receiver owns the advance either way.
      if (el.ended) return;
      // The receiver has started, or resumed. Jump to it and go.
      const want = Math.max(0, target - localOffset);
      if (castSyncCanSeek(el, want)) el.currentTime = want;
      el.playbackRate = 1;
      castSyncWarned = false;
      castSyncDone();
      el.play().catch(err => console.warn('[cast] local picture', err));
      return;
      }

   const err = target - (el.currentTime + localOffset);

   // The loop is the settle detector, and that is the signal the control never
   // had.  A change takes 5-15 s to be absorbed when it cannot be stepped, and
   // nothing said so — so the natural thing to do was change it again.  The
   // buttons stay disabled until the picture is actually where the setting
   // says, or until the cap, because a settle that never converges must not
   // leave them dead.
   if (castSyncPhase === 'settling' &&
       (Math.abs(err) < SYNC_DEAD ||
        Date.now() - castSyncSettleAt > SYNC_SETTLE_CAP))
      castSyncDone();

   if (Math.abs(err) > SYNC_HARD) {
      const want = Math.max(0, target - localOffset);
      if (castSyncCanSeek(el, want)) {
         console.log('[cast] resync', err.toFixed(2), 's');
         el.currentTime  = want;
         el.playbackRate = 1;
         return;
         }
      // Nothing to do but let the rate close it, which at ±5% takes twenty
      // seconds per second of error.  Said once, because the alternative is
      // ten lines a second for as long as it lasts.
      if (!castSyncWarned) {
         castSyncWarned = true;
         console.warn('[cast] picture', err.toFixed(2),
                      's out and cannot seek — this stream is chunked');
         }
      }

   const rate = Math.abs(err) < SYNC_DEAD
      ? 1
      : 1 + Math.max(-SYNC_MAX, Math.min(SYNC_MAX, err * SYNC_GAIN));
   // Ten times a second, so only when it has actually moved.
   if (Math.abs(el.playbackRate - rate) > 0.002) el.playbackRate = rate;
}

// Which of the two things a cast video does with its picture: keep it here, or
// show the panel saying where the sound went.  One function because the load
// reply and the page-reload restore have to reach the same state, and they had
// no way of agreeing on it other than repeating the condition.
function castApplyLocalVideo(song, offset) {
   // "Is a picture appearing over there", never "was only sound sent".  A
   // screenless device set to `videoPref = send` is handed the whole film to
   // save extracting its soundtrack, and shows none of it — so keying this on
   // audioOnly threw the picture away precisely where it was still wanted.
   const local = !castReceiverVideo && castLocalVideo.get();
   if (local) {
      videoCastPanel(false);
      castLocalVideoStart(song, offset);
      // The picture is here, so the subtitles are ours to draw again.  Any
      // tracks the receiver was declared are inert, having no screen to put
      // them on, and videoSelectCaption() keys on castVideoLocal so it never
      // tries to switch one.
      videoLoadCaptions(song);
      videoLoadChapters(song);
      videoPreparing(true, castAudioOnly ? 'Preparing sound…' : 'Preparing…');
      } else {
      castLocalVideoStop();
      videoCastPanel(true, !castReceiverVideo);
      // No picture anywhere means no subtitles to offer, and an unselected
      // <track> costs a request either way — so the picker is not drawn rather
      // than drawn and inert.
      if (!castReceiverVideo) videoClearCaptions(song);
      else                    videoLoadCaptions(song);
      // Chapters are wanted here even when captions are not, and the two part
      // company deliberately: a screenless receiver has nowhere to draw a
      // subtitle, but a concert playing through an amplifier is precisely when
      // "which song is this" is the whole point of the feature.
      videoLoadChapters(song);
      }
}

// Loads the film into the local element without starting it: castSyncTick()
// does that when the receiver reports PLAYING, which for a soundtrack is after
// a transcode that can take a minute.
//
// Everything about the stream is decided exactly as playerPlay()'s local
// branch decides it, because it *is* that stream — a divergence here would be
// a second answer to "which tier does this video take".
function castLocalVideoStart(song, offset) {
   // castRedirect=false is not optional here.  apiUrl() puts castController on
   // every URL, and stream.view answers the owner of a live cast session with
   // 204 — pushing the track to the receiver instead, on the assumption that
   // an owner asking for a stream is about to play it a second time.  This
   // request is the opposite: it is the picture belonging to the soundtrack
   // the receiver is already playing.  Without the parameter the picture never
   // arrives, and worse, the 204 path re-issues the LOAD.
   const streamParams = {id: song.id, castRedirect: 'false'};
   const chunked = song.nativeSeek === false;
   if (chunked && offset > 0) streamParams.timeOffset = Math.floor(offset);
   player.streamIsTranscoded = chunked;
   player.streamFormat       = null;
   player.localOffset        = (chunked && offset > 0) ? offset : 0;

   castVideoLocal = true;
   castSyncWarned = false;
   // Per stream, not per device: whether a seek lands is a property of the tier
   // this film is being served at, and the staircase starts coarse again for a
   // fresh calibration.  The delay itself is deliberately kept — it is the
   // device's, and lives in gd_cast_sync_<id>.
   castSyncNoSeek  = false;
   castSyncStep    = SYNC_STEP_START;
   castSyncLastDir = 0;
   castSyncRun     = 0;
   castSyncPhase   = 'idle';
   playerSelectMedia(true);
   // Not a courtesy to the room: this element and the amplifier are playing
   // the same film, and the whole design rests on only one of them being
   // audible — muting is what makes playbackRate a free correction.
   player.videoEl.muted = true;
   player.videoEl.playbackRate = 1;
   player.videoEl.src = apiUrl('stream', streamParams);
   if (offset > 0 && !chunked) player.videoEl.currentTime = offset;
   videoSyncButton();
}

// Called wherever the mode ends: the cast stopping, the setting being off for
// the next track, or the picture failing to decode.  Without the src clear a
// muted film goes on downloading with nobody watching it.
function castLocalVideoStop() {
   if (!castVideoLocal) return;
   castVideoLocal = false;
   // Clearing this first is what makes the guards in castSyncStepMove()'s
   // timers work: a step in flight has no element to finish against.
   castSyncPhase  = 'idle';
   const el = player.videoEl;
   if (el) {
      el.pause();
      el.removeAttribute('src');   // never src='' — that resolves to GET /
      el.load();
      el.playbackRate = 1;
      el.muted = false;
      }
   videoSyncButton();
}

// Local interpolation timer — keeps the progress bar smooth between SSE pushes.
// Only active while casting; reads local vars, makes no network requests.
setInterval(() => {
   if (castDeviceId === null || castBaseAt === 0) return;
   // Only extrapolate position while actually playing; freeze it when paused.
   const elapsed = castPlayerState === 'PLAYING'
      ? (Date.now() - castBaseAt) / 1000
      : 0;
   const absCurrent = castStartOffset + castBaseTime + elapsed;
   const song = player.queue[player.index];
   const totalSecs = (song?.duration ?? 0) || castSongDuration;
   const seek = document.getElementById('player-seek');
   const time = document.getElementById('player-time');
   if (!seek.dataset.seeking) seek.value = Math.floor(absCurrent);
   time.textContent = `${fmtDuration(Math.floor(absCurrent))} / ${fmtDuration(totalSecs)}`;
   // Same clock, two consumers more: the picture follows what the bar draws,
   // and so does the chapter label -- the local timeupdate handler returns
   // early while casting, so without this the label freezes when a cast starts.
   castSyncTick(absCurrent);
   videoChapterTick(absCurrent);
   }, 100);

async function openInfoModal() {
   const cur = player.queue[player.index];
   if (!cur) return;
   const list  = document.getElementById('info-modal-list');
   const play  = document.getElementById('info-playback-list');
   const modal = document.getElementById('info-modal');
   list.innerHTML = '';
   play.innerHTML = '';
   modal.classList.remove('hidden');

   // Re-fetch via getSong so transcoded* fields reflect the user's current
   // max_bitrate setting, not whatever was true when the queue was loaded.
   let song = cur;
   try {
      const sr = await apiCall('getSong', {id: cur.id});
      if (sr.song) song = sr.song;
      } catch { /* keep cached song */ }

   const rows = [
      ['Title',              song.title],
      ['Artist',             song.artist],
      ['Album',              song.album],
      ['Track',              song.track],
      ['Year',               song.year],
      ['Server file format', song.suffix],
      ['Server bitrate',     song.bitRate ? `${song.bitRate} kbps` : null],
      ['Length',             song.duration ? fmtDuration(song.duration) : null],
      ['Starred',            song.starred ? 'Yes' : 'No'],
      ];

   // Second section: how the sound is reaching the speaker, which is a
   // different question from what the file is.  Matches the Android app's
   // track info dialog, which has had it since the cast route became
   // something a session could get wrong silently.
   const casting = castDeviceId !== null;
   const dev     = castDeviceName || 'the receiver';
   let output;
   if (!casting)
      output = 'This browser';
   else if (castVideoLocal)
      // The one case where the two halves of a playback are in two places,
      // and the only reason this row is not a constant.  Android spells it as
      // a separate Route row because a phone can also relay or serve a
      // downloaded copy; the server-driven path has none of those, so a row
      // of its own would read the same sentence on every cast.
      output = `Chromecast “${dev}” — sound fetched from this `
             + 'server, picture playing in this browser';
   else
      output = `Chromecast “${dev}” — fetching from this server`;

   // What is actually going out.  While casting that is the server's answer,
   // read back rather than worked out again here: a cast URL carries no
   // format, no maxBitRate and no timeOffset, and the account ceiling is
   // exempt for a cast token, so none of the transcoded* fields describe it.
   let sent = null;
   if (casting && castStream) {
      const kbps = castStream.sentBitRate > 0
         ? `${castStream.sentSuffix?.toUpperCase() ?? ''} ${castStream.sentBitRate} kbps`.trim()
         : (castStream.sentSuffix?.toUpperCase() ?? '');
      // A soundtrack still says which tier produced it.  That is the whole
      // question this row exists to answer — "why does this sound worse on
      // the television" — and a copied track and a 320 kbps re-encode are
      // exactly the two answers, so collapsing them into one phrase would
      // leave the row unable to say the thing it is for.
      const how = castStream.audioOnly
                     ? (castStream.tier === 'remux'
                           ? 'soundtrack only, copied'
                           : 'soundtrack only, re-encoded')
                : castStream.tier === 'remux'  ? 'remuxed to MP4'
                : castStream.tier === 'encode' ? 're-encoded as it plays'
                :                                'as stored';
      sent = kbps ? `${kbps} — ${how}` : how;
      }
   else {
      // Local playback.  Two independent transcode triggers exist: (a) the
      // server caps bitrate per user — reflected in the transcoded* fields;
      // (b) the browser asked for format=mp3 because it cannot decode the
      // source codec (or fell back after a decode error), which only the
      // client knows about.  player.streamFormat is the format the player
      // asked for on the most recent playerPlay() call.
      let suffix, bitRate;
      if (player.streamFormat) {
         // Streamer's format_change branch: target bitrate is max_bitrate when
         // the user has one set and it is below 320, else 320 kbps.
         suffix  = player.streamFormat;
         const cap = currentUser?.maxBitRate || 0;
         bitRate = (cap > 0 && cap < 320) ? cap : 320;
         }
      else {
         suffix  = song.transcodedSuffix  ?? song.suffix;
         bitRate = song.transcodedBitRate ?? song.bitRate;
         }
      if (suffix || bitRate)
         sent = bitRate ? `${(suffix ?? '').toUpperCase()} ${bitRate} kbps`.trim()
                        : (suffix ?? '').toUpperCase();
      }

   const playRows = [
      ['Output',        output],
      ['Sent',          sent],
      // What the receiver picks its decode pipeline from, and not the same
      // thing as the container: everything the remux or encode tier touches
      // is announced as video/mp4.
      ['Declared type', casting ? castStream?.contentType : null],
      ];

   const fill = (dl, pairs) => {
      for (const [label, value] of pairs) {
         if (value === null || value === undefined || value === '') continue;
         const dt = document.createElement('dt');
         dt.textContent = label;
         const dd = document.createElement('dd');
         dd.textContent = value;
         dl.appendChild(dt);
         dl.appendChild(dd);
         }
      };
   fill(list, rows);
   // The section is unconditional, and so is its heading: the modal opens only
   // with a current track, and Output always has an answer for one — "this
   // browser" is as much a fact as a device name. The other two rows drop
   // themselves when empty, as every row in the first list does.
   fill(play, playRows);
   }

// One row of the cast picker, laid out as the Android sheet lays it out: the
// friendly name, and under it the model and address joined with " · ". The
// port is deliberately absent — it is never the thing that tells two devices
// apart, and 8009 on every row is noise.
function castDeviceButton(dev) {
   const btn = document.createElement('button');
   const label = dev.name || dev.address;
   const connected = dev.id === castDeviceId;

   const icon = document.createElement('span');
   icon.className = 'mi cast-row-icon';
   icon.textContent = connected ? 'cast_connected' : 'cast';

   const text = document.createElement('span');
   text.className = 'cast-row-text';
   const primary = document.createElement('span');
   primary.className = 'cast-row-name';
   primary.textContent = label;
   text.appendChild(primary);

   // "Sound only" on a receiver that announced no screen, so a film cast to
   // an amplifier is a known choice rather than a surprise after the first
   // load — which is the only place it showed before.  Tested against an
   // explicit false: video_out() reports true for a device that announced
   // nothing, which is every manually configured one, and marking those would
   // be a guess presented as a fact.
   //
   // `send` is deliberately absent from this: it changes what the device is
   // sent, not what it can show, so a row marked "sound only" stays marked
   // that way.  Only `sound` changes the answer, by saying a device that
   // announced a screen has not got one.
   const shows = dev.videoPref === 'sound' ? false : dev.videoOut;
   const sub = [dev.model, dev.address,
                shows === false ? 'sound only' : null]
      .filter(Boolean).join(' · ');
   if (sub) {
      const secondary = document.createElement('span');
      secondary.className = 'cast-row-sub';
      secondary.textContent = sub;
      text.appendChild(secondary);
      }

   btn.classList.toggle('connected', connected);
   btn.appendChild(icon);
   btn.appendChild(text);
   btn.addEventListener('click', () => selectCastDevice(dev.id, label));

   // The row is a wrapper rather than the button itself, so the picker can
   // carry a control beside the name.  A <select> inside a <button> is invalid
   // and behaves unpredictably, and the whole button is a "start casting"
   // target — as a sibling the menu cannot start a cast by being clicked.
   const row = document.createElement('div');
   row.className = 'cast-row';
   row.appendChild(btn);

   // What to do with a video on this device.  Two options rather than three,
   // and which two depends on which side of its own announcement the device
   // is: only one of "send" and "sound" can change anything for it, and
   // offering the inert one would invite the question of what it does.
   //
   // The label says what is *saved*, not what is shown.  A screenless device
   // cannot show a picture whichever of these is chosen — the difference is
   // that "send" hands it the file and lets it ignore the picture, instead of
   // demuxing the soundtrack out first and making the listener wait for it.
   // And "if it can" is not hedging: the file goes out untouched or not at
   // all, so on a DVD rip or an HEVC film this genuinely does nothing.
   const opts = dev.videoOut === false
      ? [['auto', 'Extract the sound'], ['send', 'Send the file if it can']]
      : [['auto', 'Video'],             ['sound', 'Soundtrack only']];
   const sel = document.createElement('select');
   sel.className = 'cast-row-pref';
   for (const [value, text] of opts) {
      const opt = document.createElement('option');
      opt.value = value;
      opt.textContent = text;
      sel.appendChild(opt);
      }
   // Anything the two options above cannot express reads as auto, which is
   // also what the server does with a value it does not recognise.
   sel.value = opts.some(o => o[0] === dev.videoPref) ? dev.videoPref : 'auto';
   sel.addEventListener('change', async () => {
      try {
         await apiCall('setCastDevicePref',
                       {deviceId: dev.id, videoPref: sel.value});
         dev.videoPref = sel.value;
         // Redraw: the sub-line now says something different.
         renderCastDevices(castDeviceCache);
         }
      catch (err) { showError(err.message); }
      });
   row.appendChild(sel);

   return row;
   }

// The list last drawn, kept so a per-device control can redraw the rows after
// changing one without re-asking the server for a list it just supplied.
let castDeviceCache = [];

function renderCastDevices(devices) {
   const list = document.getElementById('cast-device-list');
   castDeviceCache = devices;
   list.textContent = '';

   // Discovered first, sorted by name, then the configured ones under a
   // heading of their own. Which devices arrived by which route is the most
   // useful fact on this list when discovery is the thing that has failed,
   // and it is how the Android picker is arranged.
   const found  = devices.filter(d => !d.manual)
                         .sort((a, b) => (a.name || a.address).toLowerCase()
                            .localeCompare((b.name || b.address).toLowerCase()));
   const manual = devices.filter(d => d.manual);

   if (found.length === 0 && manual.length === 0) {
      list.textContent = 'No devices found.';
      return;
      }

   for (const dev of found) list.appendChild(castDeviceButton(dev));

   if (manual.length > 0) {
      const heading = document.createElement('div');
      heading.className = 'cast-list-heading';
      heading.textContent = 'Added manually';
      list.appendChild(heading);
      for (const dev of manual) list.appendChild(castDeviceButton(dev));
      }
   }

async function openCastModal() {
   const modal   = document.getElementById('cast-modal');
   const list    = document.getElementById('cast-device-list');
   const stopRow = document.getElementById('cast-stop-row');

   list.textContent = 'Looking for devices…';
   stopRow.classList.toggle('hidden', castDeviceId === null);
   modal.classList.remove('hidden');

   try {
      renderCastDevices((await apiCall('listCastDevices')).castDevices ?? []);
      } catch (err) {
      list.textContent = `Error: ${err.message}`;
      return;
      }

   // listCastDevices answers from the *previous* discovery pass and starts a
   // fresh one, so a device that has only just been switched on would appear
   // no earlier than the next time the modal was opened. Ask once more when
   // that pass has had time to finish.
   setTimeout(async () => {
      if (modal.classList.contains('hidden')) return;
      try {
         renderCastDevices((await apiCall('listCastDevices')).castDevices ?? []);
         } catch (err) {
         // The list already on screen is better than replacing it with an
         // error, but a refresh that never succeeds should not be invisible.
         console.warn('cast device refresh failed:', err);
         }
      }, 5000);
   }

// The cast button is both the state and the control: the filled
// "cast_connected" glyph is what says a session is live, and the accent colour
// alone did not — a red "cast" reads as an available device on every other
// platform. One helper, because the three callers that toggle it (start,
// teardown, page-reload restore) must not be able to set glyph and class apart.
function castButtonState(on) {
   const btn = document.getElementById('player-cast');
   btn.classList.toggle('active', on);
   btn.textContent = on ? 'cast_connected' : 'cast';
   }

async function selectCastDevice(id, label = '') {
   try {
      await apiCall('startCast', {id});
      castDeviceId   = id;
      castDeviceName = label;
      castButtonState(true);
      document.getElementById('cast-modal').classList.add('hidden');
      // Stop local playback and re-issue the stream request so the server
      // can redirect it to the cast device (the redirect only fires on a
      // new request; if audio was already playing it never re-requested).
      // Capture the current position so the cast device resumes from there.
      // For transcoded streams (FLAC played in-browser) audio.currentTime is
      // relative to the start of the current stream chunk; localOffset holds
      // the absolute track position where that chunk begins.
      castWasPlaying = false;
      if (player.index >= 0) {
         const offset = (player.localOffset || 0) + player.media.currentTime;
         castStartOffset  = offset;
         lastCastPosition = offset;
         player.media.pause();
         // removeAttribute, never src = '': an empty src resolves against the
         // page and makes the browser fetch GET /, which is the whole SPA.
         player.media.removeAttribute('src');
         player.media.load();
         playerPlay(offset);
         }
      // Open SSE stream to receive pushed Chromecast status updates.
      startCastEvents();
      } catch (err) {
      alert(`Cast failed: ${err.message}`);
      }
   }

// Leave cast mode locally: close the event stream, reset the cast globals and
// put the UI back. Says nothing to the server.
//
// Split out of stopCast() because there are now two ways out of cast mode. One
// is the user pressing stop, which also tells the server. The other is being
// displaced by another client taking the session over, where there is nothing
// to tell the server — the session is already someone else's, and stopCast
// from a non-owner is a no-op by design.
//
// Returns the absolute position the receiver had reached, so a caller that
// wants to resume locally can.
function castExit() {
   if (castEventSrc !== null) {
      castEventSrc.close();
      castEventSrc = null;
      }
   // Capture the position tracked by the SSE stream before tearing down state.
   const elapsed = castPlayerState === 'PLAYING'
      ? (Date.now() - castBaseAt) / 1000
      : 0;
   const resumeOffset = lastCastPosition || (castStartOffset + castBaseTime + elapsed);
   castDeviceId         = null;
   castDeviceName       = '';
   castStartOffset      = 0;
   lastCastPosition     = 0;
   castWasPlaying       = false;
   castBaseTime         = 0;
   castBaseAt           = 0;
   castExpectedPosition = null;
   // Before castAudioOnly is cleared, and before any resume: a muted film left
   // with a src goes on downloading for nobody.  stopCast({resumeLocal:false})
   // — logging out — has nothing else that would stop it.  The surface's own
   // close button calls castLocalVideoStop() directly, since it no longer
   // comes through here.
   castLocalVideoStop();
   castAudioOnly    = false;
   castReceiverVideo = true;
   castStream       = null;
   castPlayerState  = 'IDLE';
   castSongDuration = 0;
   castButtonState(false);
   document.getElementById('cast-modal').classList.add('hidden');
   // Cleared before any resume, which composes the surface again: leaving the
   // class on would put the panel over a picture that is now local.
   videoCastPanel(false);
   return resumeOffset;
   }

async function stopCast({resumeLocal = true} = {}) {
   // Local teardown first, so the position is read before the SSE stream can
   // push another status into the state it is derived from.
   const resumeOffset = castExit();
   try {
      await apiCall('stopCast');
      } catch (_) {
      // best-effort stop
      }
   if (resumeLocal && player.index >= 0) {
      playerPlay(resumeOffset);
      } else {
      // Nothing is going to be played here, so nothing will hide the surface
      // on its way past.
      videoSurfaceSet(null);
      }
   }

// ── Star synchronisation across panes ─────────────────────────────────────
// Each star toggle tags itself with data-star-kind / data-star-id and emits
// a 'star-changed' event after a successful API call. A single document
// listener then brings every other matching toggle in any pane into sync,
// and rebuilds the Starred sections in the playlists pane so rows appear
// or disappear in step. Avoids ad-hoc per-pane wiring.

function dispatchStarChanged(kind, id, starred) {
   document.dispatchEvent(new CustomEvent('star-changed', {
      detail: {kind, id, starred}
      }));
   }

// After a successful cover-art update, refresh every cover element on the
// page that points at this album. Cover elements (img and placeholder div)
// tag themselves with data-album-id; the listener rebuilds their src with a
// cache-busting timestamp, and replaces placeholders with real images.
function dispatchCoverArtChanged(albumId) {
   document.dispatchEvent(new CustomEvent('cover-art-changed', {
      detail: {albumId}
      }));
   }

document.addEventListener('cover-art-changed', e => {
   const {albumId} = e.detail;
   if (albumId === undefined || albumId === null || albumId === '') return;
   const v = Date.now();
   const sel = `[data-album-id="${albumId}"]`;
   for (const el of document.querySelectorAll(sel)) {
      const size = el.dataset.coverSize;
      const src  = apiUrl('getCoverArt', {id: albumId, size, _v: v});
      if (el.tagName === 'IMG') {
         el.src = src;
         continue;
         }
      // Placeholder div — replace with a real image, preserving size class.
      const isHero = el.classList.contains('album-hero');
      const img = document.createElement('img');
      img.className = isHero ? 'album-hero' : 'album-cover';
      img.dataset.albumId   = albumId;
      img.dataset.coverSize = size;
      if (!isHero) {
         // The box, not the fetched size — those parted company at 2x.
         img.width  = ALBUM_COVER_BOX;
         img.height = ALBUM_COVER_BOX;
         }
      img.alt = '';
      img.src = src;
      el.replaceWith(img);
      }
   });

document.addEventListener('star-changed', e => {
   const {kind, id, starred} = e.detail;
   const sel = `[data-star-kind="${kind}"][data-star-id="${id}"]`;
   for (const el of document.querySelectorAll(sel)) {
      el.classList.toggle('starred', starred);
      const on  = el.dataset.titleStarred;
      const off = el.dataset.titleUnstarred;
      if (on && off) el.title = starred ? on : off;
      }
   const starredContainer = document.getElementById('starred-sections');
   if (starredContainer) refreshStarredSections(starredContainer);
   });

async function refreshStarredSections(container) {
   container.innerHTML = '';
   let starSr;
   try {
      starSr = await apiCall('getStarred');
      }
   catch (e) {
      console.warn('[playlists] could not load starred items:', e);
      return;
      }
   const albumsRaw = starSr.starred?.album ?? [];
   const songsRaw  = starSr.starred?.song  ?? [];
   const albums    = Array.isArray(albumsRaw) ? albumsRaw : [albumsRaw];
   const songs     = Array.isArray(songsRaw)  ? songsRaw  : [songsRaw];

   if (albums.length > 0) {
      const heading = document.createElement('h2');
      heading.className = 'playlist-section-heading';
      heading.textContent = 'Starred albums';
      container.appendChild(heading);

      for (const album of albums) {
         const row = document.createElement('div');
         row.className = 'playlist-row';

         const name = document.createElement('span');
         name.className = 'playlist-name';
         name.textContent = album.title;

         const meta = document.createElement('span');
         meta.className = 'playlist-meta';
         if (album.artist) meta.textContent = album.artist;

         row.appendChild(name);
         row.appendChild(meta);
         row.addEventListener('click', () =>
            viewTracksFromSearch(album.id, album.title, album.parent, album.artist));
         container.appendChild(row);
         }
      }

   if (songs.length > 0) {
      const heading = document.createElement('h2');
      heading.className = 'playlist-section-heading';
      heading.textContent = 'Starred tracks';
      container.appendChild(heading);

      for (const song of songs) {
         const row = document.createElement('div');
         row.className = 'playlist-row';

         const name = document.createElement('span');
         name.className = 'playlist-name';
         name.textContent = song.title;

         const meta = document.createElement('span');
         meta.className = 'playlist-meta';
         const parts = [];
         if (song.artist) parts.push(song.artist);
         if (song.duration) parts.push(fmtDuration(song.duration));
         meta.textContent = parts.join(' · ');

         row.appendChild(name);
         row.appendChild(meta);
         row.addEventListener('click', () =>
            viewTracksFromSearch(song.parent, song.album, null, song.artist, song.id));
         container.appendChild(row);
         }
      }
   }

// ── Album star toggle ──────────────────────────────────────────────────────

function makeAlbumCover(album) {
   if (album.coverArt) {
      const img = document.createElement('img');
      img.className = 'album-cover';
      img.dataset.albumId   = album.id;
      img.dataset.coverSize = coverPx(ALBUM_COVER_BOX);
      img.width  = ALBUM_COVER_BOX;
      img.height = ALBUM_COVER_BOX;
      img.alt    = '';
      img.src = apiUrl('getCoverArt',
                       {id: album.coverArt, size: coverPx(ALBUM_COVER_BOX)});
      return img;
      }
   const div = document.createElement('div');
   div.className = 'album-cover album-cover-placeholder mi';
   div.dataset.albumId   = album.id;
   div.dataset.coverSize = coverPx(ALBUM_COVER_BOX);
   div.textContent = 'music_note';
   return div;
   }

function makeAlbumStar(album) {
   const btn = document.createElement('span');
   btn.className = 'album-star mi' + (album.starred ? ' starred' : '');
   btn.title = album.starred ? 'Unstar album' : 'Star album';
   btn.textContent = 'star';
   btn.dataset.starKind       = 'album';
   btn.dataset.starId         = album.id;
   btn.dataset.titleStarred   = 'Unstar album';
   btn.dataset.titleUnstarred = 'Star album';

   btn.addEventListener('click', async e => {
      e.stopPropagation();
      const isStarred = btn.classList.contains('starred');
      try {
         await apiCall(isStarred ? 'unstar' : 'star', {albumId: album.id});
         dispatchStarChanged('album', album.id, !isStarred);
         }
      catch {
         showError('The action could not be completed. Please check your connection.');
         }
      });

   return btn;
   }

// ── Track action icons (star / add-to-playlist) ────────────────────────────

function makeTrackActions(song, ctx) {
   const starBtn = document.createElement('span');
   starBtn.className = 'track-action mi' + (song.starred ? ' starred' : '');
   starBtn.title = song.starred ? 'Unstar' : 'Star';
   starBtn.textContent = 'star';
   starBtn.dataset.starKind       = 'track';
   starBtn.dataset.starId         = song.id;
   starBtn.dataset.titleStarred   = 'Unstar';
   starBtn.dataset.titleUnstarred = 'Star';

   starBtn.addEventListener('click', async e => {
      e.stopPropagation();
      const isStarred = starBtn.classList.contains('starred');
      try {
         await apiCall(isStarred ? 'unstar' : 'star', {id: song.id});
         dispatchStarChanged('track', song.id, !isStarred);
         }
      catch {
         showError('The action could not be completed. Please check your connection.');
         }
      });

   if (ctx?.playlistId != null) {
      const removeBtn = document.createElement('span');
      removeBtn.className = 'track-action track-action-remove mi';
      removeBtn.title = 'Remove from playlist';
      removeBtn.textContent = 'close';

      removeBtn.addEventListener('click', e => {
         e.stopPropagation();
         const album = song.album ?? '';
         const msg = album
            ? `Remove "${song.title}" (from album "${album}") from this playlist?`
            : `Remove "${song.title}" from this playlist?`;
         showConfirm(msg, async () => {
            removeBtn.style.pointerEvents = 'none';
            try {
               await apiCall('updatePlaylist', {playlistId: ctx.playlistId, songIndexToRemove: ctx.index});
               await viewPlaylistTracks(ctx.playlistId, ctx.playlistName);
               }
            catch {
               showError('The action could not be completed. Please check your connection.');
               }
            }, {title: 'Remove track', yes: 'Remove'});
         });

      return [starBtn, removeBtn];
      }

   const listBtn = document.createElement('span');
   listBtn.className = 'track-action mi';
   listBtn.title = 'Add to playlist';
   listBtn.textContent = 'playlist_add';

   listBtn.addEventListener('click', e => {
      e.stopPropagation();
      openPlaylistModal(song.id);
      });

   return [starBtn, listBtn];
   }

let playlistTargetId = null;

function openPlaylistModal(songId) {
   playlistTargetId = songId;
   const list = document.getElementById('playlist-modal-list');
   list.textContent = 'Loading…';
   document.getElementById('playlist-modal').classList.remove('hidden');
   document.getElementById('playlist-new-name').value = '';

   apiCall('getPlaylists').then(sr => {
      const playlists = sr.playlists?.playlist ?? [];
      list.textContent = '';
      if (playlists.length === 0) {
         list.textContent = 'No playlists yet.';
         return;
         }
      for (const pl of playlists) {
         const btn = document.createElement('button');
         btn.textContent = pl.name;
         btn.addEventListener('click', async () => {
            await apiCall('updatePlaylist', {playlistId: pl.id, songIdToAdd: playlistTargetId});
            document.getElementById('playlist-modal').classList.add('hidden');
            });
         list.appendChild(btn);
         }
      });
   }

const player = {
   // Two concrete elements, one active pointer.  `media` is whichever element
   // is currently playing and is what every control below drives; audioEl and
   // videoEl are the elements themselves.  Splitting it this way means the
   // audio path runs exactly the code it ran before video existed — the
   // alternative, teaching one element to do both jobs, would have put video's
   // quirks in the way of audio playback.
   audioEl: new Audio(),
   videoEl: null,   // the <video> inside #video-surface; set by setupPlayer()
   media: null,     // → audioEl or videoEl; initialised just below
   queue: [],   // song objects from getAlbum
   index: -1,   // current position in queue
   autoFrom: 0, // entries at indices >= autoFrom were auto-generated by playerLoad
   scrobbled: false,  // true once submission scrobble has fired for current track
   albumCtx: null, // {albumId, albumTitle, artistId, artistName} of the loaded album
   // Transcoded chunked streams have no Range support, so the browser
   // can't seek into un-buffered audio.  When streamIsTranscoded is true,
   // every seek re-fetches from the server with ?timeOffset=N (same trick
   // as the Cast path).  localOffset is the seek point we asked the server
   // to start at — added to audio.currentTime for absolute-time display.
   streamIsTranscoded: false,
   localOffset: 0,
   streamFallbackTried: false,
   // Format actually requested from the server for the current track, or null
   // if the source is being served as-is.  Used by the info dialog to show
   // the format the user is actually hearing rather than the on-disk format.
   streamFormat: null,
   // The chosen subtitle track, and the song it was chosen for.  Held across a
   // re-fetch because a seek on a transcoded stream is one: without this, every
   // seek would silently turn the subtitles off.
   captionIndex: null,
   captionSong: null,
   // The caption list getVideoInfo returned, kept so a menu position can be
   // turned back into the server's own caption id. The cast API numbers these
   // 1..n in this order.
   captions: [],
   // The marker list getChapters returned, and the song it belongs to. Held
   // across a re-fetch for the same reason the caption list is: a seek on a
   // transcoded stream comes back through playerPlay, and asking the server
   // again on every seek would be a file read per seek for a list that cannot
   // have changed. This is also what Cancel restores to.
   chapters: [],
   chapterSong: null,
};
player.media = player.audioEl;

// Points player.media at the element the given song needs, stopping the other
// one first — leaving the outgoing element with a live src means two streams
// playing at once, and the second one is inaudible but still downloading.
// Returns true when the active element changed.
function playerSelectMedia(isVideo) {
   const want = isVideo ? player.videoEl : player.audioEl;
   if (!want || want === player.media) return false;
   player.media.pause();
   player.media.removeAttribute('src');
   player.media.load();   // drops the buffered stream and cancels the fetch
   player.media = want;
   return true;
}

// ── Video surface ───────────────────────────────────────────────────────────

// Shows or hides the surface and switches between the two states.  state is
// 'theatre' or 'minimised'; passing null hides it entirely.
function videoSurfaceSet(state) {
   const el = document.getElementById('video-surface');
   if (!state) {
      // #video-close is inside #video-frame and so is reachable in fullscreen,
      // which it was not while fullscreen was requested on the <video> itself.
      // display:none on an ancestor does not take the document out of
      // fullscreen, so without this the viewer is left staring at an empty
      // black screen with the film stopped behind it.
      if (document.fullscreenElement && el.contains(document.fullscreenElement))
         document.exitFullscreen?.().catch(() => {});
      el.classList.add('hidden');
      return;
      }
   el.classList.remove('hidden');
   el.dataset.state = state;
   document.getElementById('video-restore').hidden = (state !== 'minimised');
   // The button does two different things and must not claim otherwise: it
   // stops local playback, but while casting it only puts the picture away.
   // Set here rather than in the handler because a title is read before the
   // click, and this runs on every load and on the reload restore — which is
   // every path that can change which of the two is in force.
   const close = document.getElementById('video-close');
   const label = castDeviceId !== null ? 'Close' : 'Stop and close';
   close.title = label;
   close.setAttribute('aria-label', label);
}

function videoSurfaceCaption(song) {
   document.getElementById('video-caption').textContent =
      song ? [song.title, song.album].filter(Boolean).join(' — ') : '';
}

// Swaps the surface between showing the picture and saying where the picture
// went.
//
// The surface is composed while casting rather than hidden, and that is not
// decoration: #video-bar is where the subtitle picker lives, and a film on a
// television is exactly when someone wants it. It also fixes a stranded state —
// starting a cast mid-film used to leave the surface up with a <video> whose
// source had been taken away, showing black under a live-looking title.
// `note` is the receiver's inability to show a picture, which is a different
// thing from the film not being ready: it is permanent for this device, so it
// is stated on the panel rather than left to be inferred from a picture that
// never arrives.
function videoCastPanel(on, note = false) {
   const surf = document.getElementById('video-surface');
   surf.classList.toggle('casting', on);
   document.getElementById('video-cast-name').textContent =
      castDeviceName ? `Playing on ${castDeviceName}` : 'Playing on your TV';
   const noteEl = document.getElementById('video-cast-note');
   noteEl.hidden = !(on && note);
   noteEl.textContent = castDeviceName
      ? `${castDeviceName} cannot show video — playing the soundtrack only.`
      : 'This device cannot show video — playing the soundtrack only.';
   // Fullscreen would ask the browser to blow up an element with no source,
   // and close means "stop the film", which while casting is the television's
   // film and not this element's.
   document.getElementById('video-fullscreen').hidden = on;
}

// The remux tier blocks on ffmpeg copying the whole file into the transcode
// cache before it sends a byte, which for a feature-length film is tens of
// seconds.  Show that rather than a black rectangle that looks broken.
function videoPreparing(on, label = 'Preparing…') {
   const el = document.getElementById('video-preparing');
   el.hidden = !on;
   if (on) el.textContent = label;
}

// Shows or hides the A/V delay control, and puts the current device's value in
// it.  Called wherever the mode or the device can change, since the value is
// per device and a slider showing another one's would be worse than none.
function videoSyncButton() {
   document.getElementById('video-sync-wrap').hidden = !castVideoLocal;
   if (!castVideoLocal) {
      document.getElementById('video-sync-menu').classList.add('hidden');
      return;
      }
   const ms   = castSyncDelay.get(castDeviceId);
   const busy = castSyncPhase !== 'idle';
   document.getElementById('video-sync-value').textContent = `${ms} ms`;
   // Disabled *is* the feedback: it says the picture has not finished moving,
   // which is the one thing the operator could not previously tell and the
   // reason the old control was impossible to calibrate with.
   document.getElementById('video-sync-later').disabled   = busy;
   document.getElementById('video-sync-earlier').disabled = busy;
   document.getElementById('video-sync-busy').hidden      = !busy;
}

// How far one press of the skip buttons, or of an arrow key, moves.
const SKIP_SECS = 10;

// The two play/pause glyphs are one piece of state, so they are written
// together.  #player-playpause stays the source of truth — the cast branch of
// its own click handler reads its textContent back to decide which way to
// flip — and this only keeps the copy on the picture in step with it.
function playerPlayGlyph(name) {
   document.getElementById('player-playpause').textContent = name;
   const v = document.getElementById('video-play');
   v.textContent = name;
   // The ligature is the button's text, so a screen reader would read the glyph
   // name; the accessible name is the aria-label, and it has to move with the
   // glyph or it says "Play" over a pause symbol for the whole film.
   const label = (name === 'pause') ? 'Pause' : 'Play';
   v.title = label;
   v.setAttribute('aria-label', label);
}

// The absolute second the listener is at, wherever the sound is coming from.
//
// While casting the receiver is the clock and the local element is either
// silent or absent, so this is the same extrapolation the 100 ms interpolation
// timer draws the seek bar from — one clock, one more consumer.  castBaseAt is
// in the guard as well as the state: it is 0 until the first status arrives,
// and Date.now() - 0 is fifty-six years of playback.  Locally the element's own
// clock is rebased whenever the server started ffmpeg at a seek point, which is
// what localOffset adds back.
function playerPosition() {
   if (castDeviceId !== null) {
      const elapsed = (castPlayerState === 'PLAYING' && castBaseAt)
         ? (Date.now() - castBaseAt) / 1000
         : 0;
      return castStartOffset + castBaseTime + elapsed;
      }
   return player.media.currentTime + (player.localOffset || 0);
}

// One definition of "go to this absolute second", shared by the seek bar, the
// skip buttons and the arrow keys.  The three branches are not interchangeable:
//
//  * Casting, re-LOAD with a server-side timeOffset rather than sending a
//    Chromecast SEEK.  SEEK relies on the device seeking within its buffered
//    byte stream, which fails silently for formats without clean seek points
//    (FLAC without a seektable, etc.).  playerPlay resets cast state and
//    triggers a fresh stream from the right position.
//  * A chunked transcode has no Range, so the server has to start ffmpeg at the
//    seek point instead.  The paused state is preserved, or seeking while
//    paused would unexpectedly resume playback.
//  * Anything else is Range-capable and the element can seek itself.
//
// The flag to test is player.streamIsTranscoded rather than song.nativeSeek:
// the latter is undefined for audio and for an audio-only request, which is why
// playerPlay computes `chunked` instead of reading it.
function playerSeekTo(target) {
   if (castDeviceId !== null) {
      playerPlay(target);
      } else if (player.streamIsTranscoded) {
      const wasPaused = player.media.paused;
      playerPlay(target, true);
      if (wasPaused) {
         player.media.addEventListener('canplay',
            () => player.media.pause(), {once: true});
         }
      } else {
      player.media.currentTime = target;
      }
}

// Relative seek, for the buttons on the picture and the arrow keys.
//
// Clamped short of the end rather than to it: a timeOffset at the duration
// produces an empty stream, and a forward skip is not a request to end the
// track.  The seek bar is written here as well because a chunked seek re-fetches
// and reports nothing until the new stream starts, so the bar would otherwise
// sit at the old position for a second after the press.
function playerSkip(delta) {
   const song = player.queue[player.index];
   if (!song) return;
   const dur = song.duration || castSongDuration || player.media.duration || 0;
   const target = Math.max(0, Math.min(playerPosition() + delta,
                                       Math.max(0, dur - 1)));
   playerSeekTo(target);
   const seek = document.getElementById('player-seek');
   if (!seek.dataset.seeking) seek.value = Math.floor(target);
}

// Stops the current stream and returns the transport bar to its idle state.
// Closing the surface has to stop playback, not just hide it: a hidden video
// element still holding a stream would leave the bar offering play, pause and
// seek for something the user can no longer see, and would go on downloading.
// The queue is deliberately left intact, so Next still works.
function playerStop() {
   player.media.pause();
   player.media.removeAttribute('src');
   player.media.load();   // drops the buffered data and cancels the fetch
   player.localOffset = 0;
   player.scrobbled   = false;
   videoSurfaceSet(null);
   // Cleared at the source too: playerStop ends the film, so the next one that
   // starts must not inherit its subtitle choice.
   player.captionSong  = null;
   player.captionIndex = null;
   player.captions     = [];
   // Same reasoning for the markers: the film has ended, so the next one must
   // not start under this one's song list.
   videoClearChapters();
   videoCastPanel(false);
   videoCaptionsMenu([]);

   playerPlayGlyph('play_arrow');
   document.getElementById('player-title').textContent     = '—';
   document.getElementById('player-artist').textContent    = '';
   document.getElementById('player-info-btn').disabled     = true;
   document.getElementById('player-time').textContent      = '0:00 / 0:00';
   const seek = document.getElementById('player-seek');
   seek.value = 0;
   seek.max   = 0;
   // '' resolves to GET / and must never be assigned.
   document.getElementById('player-cover').removeAttribute('src');
   document.querySelector('.track-row.playing')?.classList.remove('playing');
}

// How long the overlay transport and the close button stay up after the
// pointer stops moving.  Long enough to aim at one of them from anywhere on
// the surface, short enough that a film is not watched through them.
const VIDEO_CONTROLS_IDLE = 3000;
let videoControlsTimer = null;

// Shown on movement, hidden after a pause — which is what :hover alone could
// not express.  A pointer resting anywhere over the picture counts as hovering
// it for as long as it sits there, and after using the transport that is
// precisely where it is, so the controls stayed drawn over the film for the
// rest of the film.
//
// The class goes on #video-surface rather than on the two clusters because
// that is the element the CSS already descended from, and because fullscreen
// is requested on #video-frame *inside* it — an ancestor keeps matching, and
// pointer events from the frame keep bubbling here, so neither state needs a
// case of its own.
function videoControlsWake() {
   const surf = document.getElementById('video-surface');
   surf.classList.add('controls-on');
   clearTimeout(videoControlsTimer);
   videoControlsTimer = setTimeout(
      () => surf.classList.remove('controls-on'), VIDEO_CONTROLS_IDLE);
   }

function videoControlsSleep() {
   clearTimeout(videoControlsTimer);
   videoControlsTimer = null;
   document.getElementById('video-surface').classList.remove('controls-on');
   }

// Requested on #video-frame rather than on the video element, so everything
// absolutely positioned inside the frame goes fullscreen with the picture: the
// transport cluster and the close button.  #video-bar is outside the frame and
// so is still out of reach — the subtitle picker included, so choose the track
// before going in.  Cues keep drawing either way: the browser paints them into
// the video box, not us.
//
// A toggle rather than the one-way request this used to be.  The button had no
// second meaning, so pressing it again did nothing and the only way out was the
// browser's own Escape; 'f' is expected to close what 'f' opened, and a button
// that disagreed with the key would be the odd one out.  videoSurfaceSet(null)
// keeps its own exit — that one fires when the surface goes away underneath a
// fullscreen element, which is not a press of anything.
function videoFullscreenToggle() {
   const frame = document.getElementById('video-frame');
   if (document.fullscreenElement === frame) {
      document.exitFullscreen?.().catch(() => {});
      return;
      }
   frame.requestFullscreen?.()
      .catch(err => console.warn('[video] fullscreen refused', err));
}

function setupVideoSurface() {
   // pointermove rather than mousemove so a pen counts too.  Touch needs
   // nothing from this: the (hover: none) rules in style.css keep both
   // clusters drawn unconditionally, and the class merely re-asserts what is
   // already true there.
   //
   // Leaving the surface hides them at once, which is what :hover did and is
   // still the right answer — the pointer has gone somewhere else entirely.
   const surf = document.getElementById('video-surface');
   surf.addEventListener('pointermove', videoControlsWake);
   surf.addEventListener('pointerdown', videoControlsWake);
   surf.addEventListener('pointerleave', videoControlsSleep);

   // Closing the surface dismisses the picture; it does not stop the cast.
   //
   // It used to, on the reasoning that playerStop() would stop a local element
   // that was not playing anything and leave the Chromecast running "with no
   // way back to it".  The second half of that was never true — the cast
   // button's Stop row is shown whenever a session is active — and the first
   // half stopped being true when the picture began staying here, muted, while
   // the sound went to an amplifier.  Closing that is a request to stop
   // watching, not to stop listening, and answering it by killing the sound in
   // another room is the one thing it cannot have meant.
   //
   // The local element still has to be stopped, or a hidden <video> goes on
   // downloading a film for nobody — the same reason castExit() calls this.
   // castLocalVideoStop() returns immediately when no picture is running, so
   // the panel case needs no branch of its own.
   document.getElementById('video-close').addEventListener('click', () => {
      if (castDeviceId !== null) {
         castLocalVideoStop();
         videoSurfaceSet(null);
         return;
         }
      playerStop();
      });
   document.getElementById('video-minimise').addEventListener('click',
      () => videoSurfaceSet('minimised'));
   document.getElementById('video-restore').addEventListener('click',
      () => videoSurfaceSet('theatre'));
   document.getElementById('video-fullscreen').addEventListener('click',
      videoFullscreenToggle);

   // Delegating rather than repeating: #player-playpause's own handler is the
   // one place that knows a pause while casting is castControl and not
   // player.media.pause(), and it is what the space bar already clicks.
   document.getElementById('video-play').addEventListener('click',
      () => document.getElementById('player-playpause').click());
   document.getElementById('video-back10').addEventListener('click',
      () => playerSkip(-SKIP_SECS));
   document.getElementById('video-fwd10').addEventListener('click',
      () => playerSkip(SKIP_SECS));
   document.getElementById('video-captions').addEventListener('click', () =>
      document.getElementById('video-captions-menu').classList.toggle('hidden'));

   // Two toggles for one panel: the bar's, and a second inside #video-controls
   // because the bar is outside the element fullscreen is requested on. The
   // subtitle picker can live with that -- a track is chosen before the film
   // starts -- but markers are placed during it.
   document.getElementById('video-chapters-btn').addEventListener('click',
      videoChaptersToggle);
   document.getElementById('video-chapters-btn2').addEventListener('click',
      videoChaptersToggle);
   document.getElementById('video-chapters-close').addEventListener('click',
      videoChaptersToggle);
   document.getElementById('video-chapters-edit').addEventListener('click',
      videoChaptersEditToggle);
   document.getElementById('video-chapter-add').addEventListener('click',
      videoChapterAdd);
   // Deliberately not the player's own previous/next, which move through the
   // queue: inside a concert those still mean "the film before this one".
   document.getElementById('video-chapter-prev').addEventListener('click',
      videoChapterPrev);
   document.getElementById('video-chapter-next').addEventListener('click',
      videoChapterNext);
   document.getElementById('video-chapters-save').addEventListener('click',
      videoChaptersSave);
   document.getElementById('video-chapters-cancel').addEventListener('click',
      videoChaptersCancel);

   document.getElementById('video-sync').addEventListener('click', () =>
      document.getElementById('video-sync-menu').classList.toggle('hidden'));
   // Discrete presses rather than a drag, which is also why there is nothing to
   // debounce here: each one is a single, atomic, confirmable step.
   document.getElementById('video-sync-later').addEventListener('click',
      () => castSyncAdjust(1));
   document.getElementById('video-sync-earlier').addEventListener('click',
      () => castSyncAdjust(-1));

   // The player bar is auto-height on mobile, and the surface is anchored to
   // its top edge.  Measure it rather than trusting --player-h, which is only
   // the desktop value.
   const bar = document.getElementById('player');
   const sync = () => document.documentElement.style.setProperty(
      '--player-actual-h', `${bar.offsetHeight}px`);
   new ResizeObserver(sync).observe(bar);
   sync();

   // Not while casting.  The picture loads in seconds and the soundtrack it is
   // waiting for takes a minute, so clearing the notice here would clear it
   // while nothing is audible.  onCastStatus owns it for a cast session, on the
   // receiver's first status that is not IDLE.
   player.videoEl.addEventListener('loadeddata',
      () => { if (castDeviceId === null) videoPreparing(false); });
   player.videoEl.addEventListener('error',
      () => { if (castDeviceId === null) videoPreparing(false); });
}

// Attaches subtitle tracks from getVideoInfo.  Fire-and-forget: captions are a
// bonus, and a video with none (the common case) must not be held up waiting
// for the ffprobe round trip that answers the question.
//
// Note <track> is subject to CORS even when <video src> is not, so captions
// only load when the server and the page share an origin.  Fixing that means
// crossorigin="anonymous" on the video element, which would then also apply to
// the media request itself — not worth risking playback for subtitles.
// The other half of videoLoadCaptions: what to do when there are deliberately
// no captions to offer, i.e. a soundtrack on a receiver with no picture. It
// has to do the same bookkeeping the film-changed branch below does, or the
// previous film's picker stays on the bar and its <track> elements stay on an
// element that is no longer showing anything.
function videoClearCaptions(song) {
   player.videoEl.querySelectorAll('track').forEach(t => t.remove());
   player.captionSong  = song.id;
   player.captionIndex = null;
   player.captions     = [];
   videoCaptionsMenu([]);
}

async function videoLoadCaptions(song) {
   player.videoEl.querySelectorAll('track').forEach(t => t.remove());

   // A seek comes back through here for the *same* film — on a transcoded
   // stream because it re-fetches, while casting because a seek is a fresh
   // LOAD. Two things follow. The chosen track is forgotten only when the film
   // changes, or every seek would silently turn the subtitles off. And the
   // list itself is not asked for again: getVideoInfo runs ffprobe over the
   // container on every call, which is not a thing to do once per seek, and a
   // film's subtitle streams do not change while it is playing.
   let caps;
   if (player.captionSong === song.id) {
      caps = player.captions;
      } else {
      player.captionSong  = song.id;
      player.captionIndex = null;
      // Cleared here rather than only on success: both the early returns below
      // would otherwise leave the previous film's picker on the bar.
      player.captions = [];
      videoCaptionsMenu([]);
      let info;
      try { info = await apiCall('getVideoInfo', {id: song.id}); }
      catch { return; }
      // The track list arrived asynchronously; if the user has moved on since,
      // these captions belong to a video that is no longer playing.
      if (player.queue[player.index]?.id !== song.id) return;
      caps = info.videoInfo?.captions ?? [];
      player.captions = caps;
      }

   // <track> elements only where the picture is.  While casting to a receiver
   // that shows the film, that is the receiver: it fetches and renders its own
   // copy, and an element here would download a subtitle for a video this
   // browser is not showing.  When the picture has stayed here and only the
   // sound went out, it is us again.  The menu is built either way, because
   // choosing the track is this client's job whoever draws it.
   if (castDeviceId === null || castVideoLocal) {
      // Read now rather than in the handler: by the time a track loads, another
      // seek may have moved it, and these elements would then belong to the
      // stream before last.
      const offset = player.localOffset || 0;
      for (const c of caps) {
         const t = document.createElement('track');
         t.kind  = 'subtitles';
         t.label = c.name || 'Subtitles';
         t.src   = apiUrl('getCaptions', {id: song.id, captionId: c.id});
         t.addEventListener('load', () => videoShiftCues(t.track, offset));
         player.videoEl.appendChild(t);
         }
      }
   console.log('[captions]', caps.length, 'track(s) for', song.id,
               castDeviceId === null ? '(local)'
                                     : (castVideoLocal ? '(cast, picture here)'
                                                       : '(cast)'),
               caps.map(c => `${c.id}:${c.name}`).join(', '));
   // After the elements, so a menu index is a textTracks index.
   videoCaptionsMenu(caps);
}

// Rebases a caption file onto the stream actually being played.
//
// A <track> is timed against the media element's own clock, and for a
// transcoded seek that clock has been rebased: the server started ffmpeg at
// the seek point, so currentTime is zero there.  The caption file still
// describes the whole film, so without this a seek to twenty minutes shows the
// opening lines — which is what `localOffset` corrects everywhere else.
// Shifting the cues is the only lever, since nothing else about a <track> can
// be offset.
//
// On 'load' because that is the one moment the cues are known to exist and to
// be unshifted: a mode change that reuses already-parsed cues fires no such
// event, so this cannot apply twice.
function videoShiftCues(track, offset) {
   if (!offset || !track?.cues) return;
   // Snapshot first — removeCue mutates the live list underneath the loop.
   for (const cue of [...track.cues]) {
      if (cue.endTime <= offset) { track.removeCue(cue); continue; }
      cue.startTime = Math.max(0, cue.startTime - offset);
      cue.endTime  -= offset;
      }
}

// Fills the subtitle picker, or hides it when there is nothing to pick.
//
// Nothing is turned on: a <track> starts `disabled`, which is also what stops
// the browser fetching it, so an unselected caption costs no request. The
// Android app makes the same call — nothing here knows the viewer's language,
// and subtitles nobody asked for are more intrusive than subtitles one click
// away.
function videoCaptionsMenu(captions) {
   const menu = document.getElementById('video-captions-menu');
   menu.replaceChildren();
   menu.classList.add('hidden');
   document.getElementById('video-captions').classList.remove('on');
   document.getElementById('video-captions-wrap').hidden = captions.length === 0;
   if (!captions.length) return;

   const add = (label, index) => {
      const b = document.createElement('button');
      b.textContent   = label;
      b.dataset.index = index ?? '';
      b.addEventListener('click', () => videoSelectCaption(index));
      menu.appendChild(b);
      };
   add('Off', null);
   captions.forEach((c, i) => add(c.name || `Track ${i + 1}`, i));

   // Put back the track a seek interrupted.  A film that has lost the track it
   // had — a re-file, a different caption list — falls back to off rather than
   // to whatever now sits at that index.
   //
   // send:false because this is a redisplay, not a choice: the local path needs
   // the textTracks modes reapplied to freshly built elements, but the cast
   // path already carries the selection in its LOAD and must not be told again.
   const keep = player.captionIndex;
   if (keep !== null && keep < captions.length)
      videoSelectCaption(keep, {send: false});
   else
      menu.firstElementChild.classList.add('current');
}

// Shows the track at index, or none when it is null.  Assigning `mode` is also
// what makes the browser fetch the WebVTT, so a track is only ever loaded once
// someone asks for it.
function videoSelectCaption(index, {send = true} = {}) {
   player.captionIndex = index;
   // Keyed on where the picture is, not on whether a cast is running: with the
   // film here and only the sound on the receiver, the cues are ours to draw
   // and the receiver has no tracks to switch.
   if (castDeviceId !== null && !castVideoLocal) {
      // The receiver owns the rendering, so the only thing to do here is tell
      // it which track.  trackId numbers the tracks 1..n as getVideoInfo lists
      // them, and 0 is off — captionId could not say "off", since a missing
      // parameter and the sidecar file are both -1.
      //
      // `send` is false when the menu is merely being rebuilt with the same
      // choice already in force, which happens on every load; re-sending would
      // be an EDIT_TRACKS_INFO per track change the user did not make.
      const trackId = index === null ? 0 : index + 1;
      console.log('[captions] cast select index', index, '→ trackId', trackId,
                  send ? '(sending)' : '(redisplay only)');
      if (send) {
         apiCall('castControl', {action: 'captions', trackId})
            .catch(err => console.warn('[cast] captions failed', err));
         }
      } else {
      const tracks = player.videoEl.textTracks;
      for (let i = 0; i < tracks.length; i++)
         tracks[i].mode = (i === index) ? 'showing' : 'disabled';
      }

   const menu = document.getElementById('video-captions-menu');
   menu.querySelectorAll('button').forEach(b => b.classList.toggle(
      'current', (b.dataset.index === '' ? null : +b.dataset.index) === index));
   menu.classList.add('hidden');
   document.getElementById('video-captions')
      .classList.toggle('on', index !== null);
}

// ── Chapter markers ─────────────────────────────────────────────────────────
//
// The song boundaries inside one long video, from a sidecar text file beside
// it.  Editing is in-place in a panel over the picture rather than in a modal,
// because placing a marker means watching for the moment a song starts.
//
// Three pieces of state.  `player.chapters` is what the server last gave us,
// which is also what Cancel restores to and what the dirty test compares
// against — the role dataset.orig plays in album edit mode.  `chapterDraft` is
// the working copy.  `chapterPanelOpen` survives a track change so the panel
// does not shut itself every time the film advances.
let chapterDraft     = [];
let chapterWritable  = false;
let chapterSource    = 'none';
let chapterPanelOpen = false;
let chapterEditing   = false;
let chapterCurrent   = -1;   // index into chapterDraft, or -1

// H:MM:SS once past an hour, M:SS below it.  fmtDuration() is not reusable
// here: it prints 5400 seconds as "90:00", which is unreadable as a position
// in a two-hour concert.
function fmtChapterTime(secs) {
   const t = Math.max(0, Math.floor(secs));
   const h = Math.floor(t / 3600);
   const m = Math.floor((t % 3600) / 60);
   const s = String(t % 60).padStart(2, '0');
   return h > 0 ? `${h}:${String(m).padStart(2, '0')}:${s}` : `${m}:${s}`;
}

// The inverse, accepting either shape and a bare number of seconds.  Returns
// null for anything it cannot read, which is what puts the red border on the
// field rather than silently moving the marker somewhere else.
function parseChapterTime(str) {
   const parts = String(str).trim().split(':');
   if (parts.length === 0 || parts.length > 3) return null;
   let total = 0;
   for (const part of parts) {
      if (!/^\d*\.?\d*$/.test(part) || part === '') return null;
      total = total * 60 + parseFloat(part);
      }
   return Number.isFinite(total) && total >= 0 ? total : null;
}

// The file itself is the wire format: the server parses this body with the
// same code that reads the sidecar off disk, so there is one definition of
// what a chapter file means rather than two that can drift.
function chapterFileText(list) {
   return list.map(c => {
      const ms  = Math.round(c.start * 1000);
      const h   = Math.floor(ms / 3600000);
      const m   = Math.floor((ms % 3600000) / 60000);
      const s   = Math.floor((ms % 60000) / 1000);
      const f   = ms % 1000;
      const pad = (n, w) => String(n).padStart(w, '0');
      const t = `${pad(h, 2)}:${pad(m, 2)}:${pad(s, 2)}.${pad(f, 3)}`;
      return c.name ? `${t} ${c.name}` : t;
      }).join('\n') + (list.length ? '\n' : '');
}

function chapterDirty() {
   const norm = l => JSON.stringify(l.map(c => [Math.round(c.start * 1000),
                                                c.name]));
   return norm(chapterDraft) !== norm(player.chapters);
}

// Loads the marker list for a song, once per song.
//
// Shaped like videoLoadCaptions(), including the guard after the await: the
// list arrives asynchronously, so by the time it does the viewer may have
// moved on, and drawing it then would put one film's songs over another's.
async function videoLoadChapters(song) {
   if (player.chapterSong === song.id) { videoChaptersRender(); return; }

   player.chapterSong = song.id;
   player.chapters    = [];
   chapterDraft       = [];
   chapterSource      = 'none';
   chapterWritable    = false;
   chapterEditing     = false;
   chapterCurrent     = -1;
   videoChaptersRender();

   let sr;
   try { sr = await apiCall('getChapters', {id: song.id}); }
   catch { return; }
   if (player.queue[player.index]?.id !== song.id) return;

   const c = sr.chapters ?? {};
   player.chapters = (c.chapter ?? []).map(x => ({start: Number(x.start) || 0,
                                                  name:  x.name ?? ''}));
   chapterSource   = c.source ?? 'none';
   chapterWritable = !!c.writable;
   chapterDraft    = player.chapters.map(x => ({...x}));
   videoChaptersRender();
}

function videoClearChapters() {
   player.chapterSong = null;
   player.chapters    = [];
   chapterDraft       = [];
   chapterSource      = 'none';
   chapterWritable    = false;
   chapterEditing     = false;
   chapterCurrent     = -1;
   videoChaptersRender();
}

// Both toggles and the panel itself.  The button is offered whenever there is
// something to show *or* something the viewer could add, so a concert with no
// markers yet still has a way in; a film nobody may edit and that carries none
// shows nothing at all, matching how the subtitle picker disappears.
//
// Reading and editing are two modes, as they are for an album: outside edit
// mode a row is text you can jump from, and only the Edit button turns the
// rows into fields.  The list is read far more often than it is changed, and a
// panel of live inputs over a playing film invites a marker dragged somewhere
// by a stray click.
function videoChaptersRender() {
   const have  = chapterDraft.length > 0;
   const offer = have || chapterWritable;
   const panel = document.getElementById('video-chapters');
   const list  = document.getElementById('video-chapters-list');
   const btn   = document.getElementById('video-chapters-btn');
   const btn2  = document.getElementById('video-chapters-btn2');
   const now   = document.getElementById('video-chapter-now');
   const edit  = document.getElementById('video-chapters-edit');
   const dirty = chapterDirty();

   // Editing something nobody may write is not a state to be in -- a film can
   // stop being writable between loads, and the mode must not survive it.
   if (!chapterWritable) chapterEditing = false;
   // Cleared before it is read, or the toggle keeps its lit state over a film
   // that no longer offers a panel.
   if (!offer) chapterPanelOpen = false;

   btn.hidden  = !offer;
   btn2.hidden = !offer;
   btn.classList.toggle('on', chapterPanelOpen);
   panel.hidden = !chapterPanelOpen;
   now.hidden   = !have;

   edit.hidden = !chapterWritable;
   edit.classList.toggle('on', chapterEditing);
   edit.title  = chapterEditing ? 'Stop editing' : 'Edit chapters';

   document.getElementById('video-chapters-foot').hidden = !chapterEditing;
   document.getElementById('video-chapters-save').disabled   = !dirty;
   document.getElementById('video-chapters-cancel').disabled = !dirty;
   document.getElementById('video-chapters-msg').textContent =
      dirty ? 'Unsaved changes'
            : (chapterSource === 'container' ? 'From the video file' : '');

   list.replaceChildren();
   chapterDraft.forEach((c, i) => {
      const row = document.createElement('div');
      row.className = 'chapter-row' + (chapterEditing ? ' editing' : '');
      row.dataset.i = i;
      if (i === chapterCurrent) row.classList.add('current');

      const jump = document.createElement('button');
      jump.className   = 'mi';
      jump.textContent = 'play_arrow';
      jump.title       = 'Play from here';
      jump.addEventListener('click', () => videoChapterJump(i));
      row.appendChild(jump);

      if (!chapterEditing) {
         const time = document.createElement('span');
         time.className   = 'chapter-time';
         time.textContent = fmtChapterTime(c.start);
         const name = document.createElement('span');
         name.className   = 'chapter-name';
         // An empty name is reported as empty by the server rather than filled
         // in, so that a save cannot write a placeholder into a line somebody
         // deliberately left bare. Drawing one here costs nothing.
         name.textContent = c.name || `Chapter ${i + 1}`;
         // The whole row jumps, not just the button: reading, there is nothing
         // else a click on it could sensibly mean.
         row.addEventListener('click', () => videoChapterJump(i));
         row.append(time, name);
         list.appendChild(row);
         return;
         }

      const time = document.createElement('input');
      time.className = 'chapter-time';
      time.value     = fmtChapterTime(c.start);
      // On change rather than on input: the list re-sorts when a time moves,
      // and re-rendering on every keystroke would take the focus away
      // mid-edit.
      time.addEventListener('change', () => {
         const t = parseChapterTime(time.value);
         if (t === null) { time.classList.add('bad'); return; }
         time.classList.remove('bad');
         chapterDraft[i].start = t;
         chapterDraft.sort((a, b) => a.start - b.start);
         videoChaptersRender();
         });

      const name = document.createElement('input');
      name.className   = 'chapter-name';
      name.placeholder = `Chapter ${i + 1}`;
      name.value       = c.name;
      name.addEventListener('input', () => {
         chapterDraft[i].name = name.value;
         videoChaptersButtons();
         });

      const here = document.createElement('button');
      here.className   = 'mi';
      here.textContent = 'my_location';
      here.title       = 'Move to the current position';
      here.addEventListener('click', () => {
         chapterDraft[i].start = Math.max(0, playerPosition());
         chapterDraft.sort((a, b) => a.start - b.start);
         videoChaptersRender();
         });

      const del = document.createElement('button');
      del.className   = 'mi';
      del.textContent = 'delete';
      del.title       = 'Remove this marker';
      del.addEventListener('click', () => {
         chapterDraft.splice(i, 1);
         videoChaptersRender();
         });

      row.append(time, name, here, del);
      list.appendChild(row);
      });
}

// The subset of the render that a keystroke in a name field needs: rebuilding
// the rows there would move the caret to the end of the field on every letter.
function videoChaptersButtons() {
   const dirty = chapterDirty();
   document.getElementById('video-chapters-save').disabled   = !dirty;
   document.getElementById('video-chapters-cancel').disabled = !dirty;
   document.getElementById('video-chapters-msg').textContent =
      dirty ? 'Unsaved changes' : '';
}

function videoChaptersToggle() {
   const surf = document.getElementById('video-surface');
   // The panel is hidden while minimised, so opening it there would be a
   // button that appears to do nothing.
   if (surf.dataset.state === 'minimised') videoSurfaceSet('theatre');
   chapterPanelOpen = !chapterPanelOpen;
   videoChaptersRender();
}

// Leaving edit mode discards, as Cancel does — but never silently: the panel
// can be several minutes of marking up a concert, and the Edit button is
// directly beside the close button.
function videoChaptersEditToggle() {
   if (chapterEditing && chapterDirty()) {
      showConfirm('Discard the unsaved chapter changes?',
                  () => { videoChaptersCancel(); },
                  {title: 'Unsaved changes', yes: 'Discard', no: 'Keep editing'});
      return;
      }
   chapterEditing = !chapterEditing;
   videoChaptersRender();
}

function videoChapterAdd() {
   // Read the clock once: it moves between two calls, so finding the row again
   // by comparing against a second reading is a race with the film.
   const at = Math.max(0, playerPosition());
   const marker = {start: at, name: ''};
   chapterDraft.push(marker);
   chapterDraft.sort((a, b) => a.start - b.start);
   videoChaptersRender();
   // Focus the marker just added, wherever the sort put it. Identity, not
   // value: two markers may legitimately sit at the same second.
   const i = chapterDraft.indexOf(marker);
   document.querySelector(`.chapter-row[data-i="${i}"] .chapter-name`)?.focus();
}

function videoChapterJump(i) {
   const c = chapterDraft[i];
   if (c) playerSeekTo(c.start);
}

// "Previous" means the start of the chapter being played, unless we are
// already at it — the behaviour every physical transport has, and the reason
// a viewer can press it twice to go back one song.
function videoChapterPrev() {
   const pos = playerPosition();
   let i = -1;
   chapterDraft.forEach((c, n) => { if (c.start <= pos + 0.25) i = n; });
   if (i < 0) { playerSeekTo(0); return; }
   if (pos - chapterDraft[i].start > 3 || i === 0) playerSeekTo(chapterDraft[i].start);
   else playerSeekTo(chapterDraft[i - 1].start);
}

function videoChapterNext() {
   const pos  = playerPosition();
   const next = chapterDraft.find(c => c.start > pos + 0.25);
   if (next) playerSeekTo(next.start);
}

// Called from both clocks — the local timeupdate handler and the 100 ms cast
// interval — because neither knows about the other and the label would
// otherwise freeze the moment a cast starts.  Nothing here writes to a field:
// a marker being typed in must not be rewritten underneath the caret.
function videoChapterTick(abs) {
   if (!chapterDraft.length) return;
   let i = -1;
   for (let n = 0; n < chapterDraft.length; n++)
      if (chapterDraft[n].start <= abs + 0.25) i = n; else break;
   if (i === chapterCurrent) return;
   chapterCurrent = i;

   const now = document.getElementById('video-chapter-now');
   now.textContent = i >= 0
      ? (chapterDraft[i].name || `Chapter ${i + 1}`) : '';
   document.querySelectorAll('.chapter-row.current')
      .forEach(r => r.classList.remove('current'));
   document.querySelector(`.chapter-row[data-i="${i}"]`)?.classList.add('current');

   // The album listing's rows too, when the film being played is the one on
   // screen. They are numbered 1..n as getChapters numbers them, while the
   // panel's are 0-based -- the panel draws the draft, which is an array.
   //
   // This is why chapter rows carry no data-id: playerUpdateUI() marks the
   // playing track by that attribute and would otherwise put `.playing` on the
   // first chapter of a film rather than on the film's own row.
   const song = player.queue[player.index];
   document.querySelectorAll('.track-row.chapter-track.current')
      .forEach(r => r.classList.remove('current'));
   if (song && i >= 0)
      document.querySelector(
         `.track-row.chapter-track[data-chapter-of="${song.id}"]` +
         `[data-chapter="${i + 1}"]`)?.classList.add('current');
}

function videoChaptersCancel() {
   chapterDraft   = player.chapters.map(c => ({...c}));
   chapterEditing = false;
   videoChaptersRender();
}

async function videoChaptersSave() {
   const song = player.queue[player.index];
   if (!song) return;
   const save   = document.getElementById('video-chapters-save');
   const cancel = document.getElementById('video-chapters-cancel');
   const msg    = document.getElementById('video-chapters-msg');
   save.disabled = cancel.disabled = true;
   msg.textContent = 'Saving…';

   try {
      // A raw fetch because apiCall() takes an object of query parameters and
      // cannot express a request body.  The id stays in the query string; the
      // body is the chapter file itself.
      const {server} = creds.load();
      const p = new URLSearchParams({...authParams(),
         v: '1.16.1', c: 'gaindrive-web', f: 'json', id: song.id});
      const resp = await fetch(`${server}/rest/saveChapters.view?${p}`, {
         method:  'POST',
         headers: {'Content-Type': 'text/plain; charset=utf-8'},
         body:    chapterFileText(chapterDraft),
         });
      // Checked before .json(): a payload rejected by the server's size limit
      // comes back as a bare HTTP error with no Subsonic envelope in it, and
      // parsing that would report a syntax error rather than the real cause.
      if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
      const sr = (await resp.json())['subsonic-response'];
      if (sr.status !== 'ok') throw new Error(sr.error?.message ?? 'Unknown error');

      // Adopt the reply rather than the draft: the server sorts, sanitises and
      // may drop a line it could not read, so anything else leaves the panel
      // describing a file that is not what is on disk.
      const c = sr.chapters ?? {};
      player.chapters = (c.chapter ?? []).map(x => ({start: Number(x.start) || 0,
                                                     name:  x.name ?? ''}));
      chapterSource   = c.source ?? 'none';
      chapterWritable = !!c.writable;
      chapterDraft    = player.chapters.map(x => ({...x}));
      chapterCurrent  = -1;
      // Back to reading, as exitEditMode does for an album: the save is the
      // end of the edit, and leaving the fields up suggests it was not.
      chapterEditing  = false;
      videoChaptersRender();
      videoChapterTick(playerPosition());
      }
   catch (e) {
      console.error('[chapters] save failed', e);
      showError(`Could not save the chapters: ${e.message}`);
      }
   finally {
      save.disabled = cancel.disabled = false;
      }
}

// One song inside a video, drawn as a track row.
//
// It shares `.track-row` so the grid, the hover and the icon styling are the
// listing's rather than a second copy -- but it carries **no `data-id`**, and
// that absence is load-bearing: playerUpdateUI() marks the playing track with
// `.track-row[data-id="..."]` and takes the first match, so a chapter row
// carrying its video's id would steal the highlight from the row that owns it.
// Which chapter is playing is a different question, answered by
// videoChapterTick() through `.current`.
function makeChapterRow(song, chapter, songs, songIndex, ctx) {
   const row = document.createElement('div');
   row.className = 'track-row chapter-track';
   row.dataset.chapterOf = song.id;
   row.dataset.chapter   = chapter.index;

   const icon = document.createElement('span');
   icon.className = 'track-icon';

   const num = document.createElement('span');
   num.className = 'track-num';
   num.textContent = chapter.index;

   const titleWrap = document.createElement('span');
   titleWrap.className = 'track-title-wrap';
   const title = document.createElement('span');
   title.className = 'track-title';
   // The server reports an empty name as empty rather than inventing one, so
   // that a save cannot write a placeholder into a line left deliberately
   // bare. Drawing one here costs nothing.
   title.textContent = chapter.name || `Chapter ${chapter.index}`;
   titleWrap.appendChild(title);

   const dur = document.createElement('span');
   dur.className = 'track-dur';
   dur.textContent = chapter.duration ? fmtDuration(chapter.duration) : '';

   // No star and no playlist button: both address a song id, and a chapter is
   // not one. The video's own row still carries them.
   const spacer1 = document.createElement('span');
   const spacer2 = document.createElement('span');

   row.addEventListener('click', () => {
      if (row.classList.contains('editing')) return;
      player.albumCtx = ctx;
      playerLoad(songs, songIndex, chapter.start);
      });

   row.append(icon, num, titleWrap, spacer1, spacer2, dur);
   return row;
}

// `offset` starts playback partway in, which is what a chapter row asks for.
// It is forwarded rather than followed by a seek because playerPlay() already
// knows all three answers -- a cast LOAD carries it, a chunked stream puts it
// in timeOffset, and a Range-capable one gets `currentTime = offset` once the
// source is set.
function playerLoad(songs, startIndex, offset = 0) {
   document.querySelectorAll('.track-row.queued').forEach(r => r.classList.remove('queued'));
   player.queue    = [...songs];
   player.index    = startIndex;
   player.autoFrom = startIndex + 1;   // everything after current track is auto
   playerPlay(offset);
}

// Append song to the queue after trimming any auto-generated tail.
function playerEnqueue(song) {
   if (player.autoFrom < player.queue.length)
      player.queue.splice(player.autoFrom);
   player.queue.push(song);
   player.autoFrom = player.queue.length;   // new entry is manual; no auto tail
   sidebarQueueUpdate();
}

function playerPlay(offset = 0, forceMp3 = false) {
   const song = player.queue[player.index];
   if (!song) return;
   // Track whether this attempt is the mp3 fallback. The 'error' listener
   // below uses this to retry exactly once before giving up.
   player.streamFallbackTried = forceMp3;
   if (castDeviceId !== null) {
      // Reset so the IDLE status during Chromecast loading doesn't trigger a
      // spurious advance, and so interpolation starts fresh for the new track.
      // castStartOffset is the offset where the served stream begins in the
      // song — 0 for MP3 (native seek) or the seek point for FLAC/other
      // (server-side seek).  Initialise to 0; the next SSE push from
      // castEvents.view will overwrite it with the authoritative value.
      castWasPlaying      = false;
      castStartOffset     = 0;
      castBaseTime        = 0;
      castBaseAt          = 0;
      castPlayerState     = 'IDLE';
      castSongDuration    = 0;
      // After a LOAD the receiver emits a transient sequence (BUFFERING t=0,
      // briefly PLAYING with a tiny t, then PLAYING at the real seek time).
      // Track the position we asked it to seek to so onCastStatus can
      // discard reports that aren't yet near that position.
      castExpectedPosition = offset;
      // Synchronously, and for every load rather than only a video one: the
      // next item may be an audio track, or a film the receiver can show
      // itself, and neither reaches castApplyLocalVideo below.  A picture left
      // running would be a muted film downloading for nobody.
      castLocalVideoStop();
      // castAudioOnly and castReceiverVideo are deliberately *not* reset
      // here.  A device that cannot show video will not start being able to
      // between two tracks, and clearing them would flash the panel back to
      // "Playing on <amp>" for as long as the reply takes.  The reply
      // overwrites both either way.
      //
      // A cast URL names no format, so the receiver plays the source codec —
      // except for a soundtrack on a device with no screen, where the server
      // adds one.  Cleared because it describes the *local* element's stream
      // and there is no local stream while casting; the info dialog reads
      // castStream instead, which the reply below fills in.  It used to fall
      // back to the server's transcodedSuffix/transcodedBitRate here, and that
      // was wrong: those describe the account bitrate ceiling, which
      // stream.view exempts for a cast token, so the dialog reported a
      // conversion that was not happening.
      player.streamFormat = null;
      const params = {id: song.id};
      if (offset > 0) params.timeOffset = Math.floor(offset);
      // Carried on the LOAD rather than sent afterwards, because a track has
      // to be declared in the LOAD to exist at all — EDIT_TRACKS_INFO can turn
      // one on but cannot introduce it.  This is also what makes the choice
      // survive a seek, which for a cast is a fresh LOAD.
      if (song.isVideo && player.captionSong === song.id
          && player.captionIndex !== null)
         params.trackId = player.captionIndex + 1;
      // Use the dedicated castLoad endpoint rather than setting player.media.src.
      // Setting audio.src would cause the browser to send a Range request, which
      // httplib converts to 416 because the stream endpoint returns 204 (no body).
      //
      // The reply says whether the receiver was given the film or only its
      // soundtrack — a device with no video_out gets the latter.  That is the
      // server's decision, taken where both the song and the device are known,
      // and read back here rather than worked out again from the device list.
      const loadSong   = song;
      const loadOffset = offset;
      apiCall('castLoad', params)
         .then(r => {
            if (player.queue[player.index] !== loadSong) return;
            // Read for every load, audio included.  Guarding this on isVideo
            // was a bug rather than an economy: castAudioOnly then carried
            // over from the last film into an audio track, and with the
            // reply now also describing the stream the info dialog would
            // have shown one track's figures against another's.
            castStream    = r?.castLoad ?? null;
            castAudioOnly = !!castStream?.audioOnly;
            castReceiverVideo = !!castStream?.receiverShowsVideo;
            if (!loadSong.isVideo) return;
            // The receiver is not showing the film, so the picture need not
            // be lost — it can stay here, muted, following the receiver's
            // clock.  Started from the reply rather than above it because
            // until the server has answered we do not know the receiver
            // cannot show it, and fetching a film to find out would be the
            // whole cost of the feature paid on every cast.
            castApplyLocalVideo(loadSong, loadOffset);
            })
         .catch(err => {
            videoPreparing(false);
            console.warn('[cast] load failed', err);
            });
      // The picture is on the television, so the surface shows where it went
      // instead — and it has to be composed at all, because it is the only
      // place the subtitle picker lives.
      if (song.isVideo) {
         const surf = document.getElementById('video-surface');
         videoSurfaceSet(surf.dataset.state || 'theatre');
         videoSurfaceCaption(song);
         // castLoad replies at once, but a soundtrack for a screenless
         // receiver is transcoded in full before the LOAD is even sent — so
         // the wait is between the reply and the receiver making a sound.
         // onCastStatus clears this on the first status that is not IDLE,
         // which is the only thing that knows the film has actually started.
         // Same wait the remux tier has, same notice.
         videoPreparing(true);
         // Provisional, from what the *last* load decided, and corrected by
         // castApplyLocalVideo when the reply lands.  Guessing rather than
         // waiting because the reply is a round trip away and a surface that
         // flickers panel-then-picture on every track looks broken; a device
         // does not gain or lose a screen between two tracks, so the guess is
         // wrong only on the first load of a session.
         if (!castReceiverVideo && castLocalVideo.get()) videoCastPanel(false);
         else videoCastPanel(true, !castReceiverVideo);
         } else {
         videoSurfaceSet(null);
         }
      playerUpdateUI();
      return;
      }
   videoCastPanel(false);
   const streamParams = {id: song.id};
   // Naming an audio format for a video is what asks the server for its
   // soundtrack alone (see audio_only_request() in src/codecs.hh), so the
   // setting is expressed entirely as a format choice and everything below
   // then treats the track as ordinary transcoded audio.
   const audioOnly = song.isVideo && videoAudioOnly.get();
   // A video otherwise skips format negotiation entirely.  pickStreamFormat()
   // probes with an *audio* element and answers 'mp3' when it is unsure, which
   // for a video would fetch the soundtrack when the user wanted the picture.
   // The server already knows which tier a video takes; asking for nothing
   // lets it decide.
   //
   // nativeSeek is the server's answer to "will this stream carry a
   // Content-Length and answer Range requests".  It is true for both the
   // direct and remux tiers and false only for a re-encode, which is exactly
   // the distinction streamIsTranscoded already means.  Getting this wrong in
   // the pessimistic direction is not harmless: sending timeOffset for a
   // remuxable file demotes it from a cheap -c copy to a full re-encode.  It
   // describes the *video* stream, so it says nothing about an audio-only
   // request — which seeks the way every other transcode does.
   let fmt;
   if (audioOnly)         fmt = audioOnlyFormat();
   else if (song.isVideo) fmt = null;
   else                   fmt = forceMp3 ? 'mp3' : pickStreamFormat(song);
   if (fmt) streamParams.format = fmt;
   const chunked = (song.isVideo && !audioOnly)
      ? (song.nativeSeek === false)
      : !!fmt;
   // For transcoded streams the browser can't seek to un-buffered offsets
   // (chunked, no Range support), so ask the server to start ffmpeg at the
   // seek point instead — the served stream is already the slice we want.
   if (chunked && offset > 0) streamParams.timeOffset = Math.floor(offset);
   player.streamIsTranscoded = chunked;
   player.streamFormat       = fmt ?? null;
   player.localOffset        = (chunked && offset > 0) ? offset : 0;

   // An audio-only video is played by the audio element and draws no surface:
   // there is no picture in the stream to draw, and nothing to caption.
   playerSelectMedia(song.isVideo && !audioOnly);
   if (song.isVideo && !audioOnly) {
      // Keep whichever state the user last left the surface in, so skipping
      // to the next episode does not re-expand a surface they minimised.
      const surf = document.getElementById('video-surface');
      videoSurfaceSet(surf.dataset.state || 'theatre');
      videoSurfaceCaption(song);
      videoPreparing(true);
      videoLoadCaptions(song);
      videoLoadChapters(song);
      } else {
      videoSurfaceSet(null);
      }

   player.scrobbled = false;
   player.media.src = apiUrl('stream', streamParams);
   if (offset > 0 && !chunked) {
      // Range-capable stream — let the browser seek natively.  Keyed on
      // `chunked` rather than on `fmt`: a video never sets fmt, so testing it
      // here would re-apply the offset to a stream that already starts there.
      player.media.currentTime = offset;
      }
   player.media.play().catch(err => console.warn('[player] play failed', err));
   playerUpdateUI();
}

function playerUpdateUI() {
   const song = player.queue[player.index];
   if (!song) return;

   document.getElementById('player-title').textContent  = song.title;
   document.getElementById('player-artist').textContent = song.artist ?? '';
   document.getElementById('player-info-btn').disabled  = false;

   // 48 is #player-cover's box, set by the width/height attributes in
   // index.html — its CSS rule gives it no dimensions of its own.
   const cover = document.getElementById('player-cover');
   cover.dataset.albumId   = song.albumId ?? song.coverArt ?? '';
   cover.dataset.coverSize = coverPx(48);
   if (song.coverArt)
      cover.src = apiUrl('getCoverArt', {id: song.coverArt, size: coverPx(48)});
   else
      cover.removeAttribute('src');   // '' resolves to GET / and must never be assigned

   player.albumCtx = {
      albumId:    song.parent ?? '',
      albumTitle: song.album  ?? '',
      artistId:   null,
      artistName: song.artist ?? '',
      };

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
   sidebarQueueUpdate();
}

function sidebarQueueUpdate() {
   const section  = document.getElementById('sidebar-queue');
   const list     = document.getElementById('sidebar-queue-list');
   const upcoming = player.queue.slice(player.index + 1);
   if (upcoming.length === 0) {
      section.hidden = true;
      return;
      }
   section.hidden = false;
   list.innerHTML = '';
   upcoming.forEach((song, i) => {
      const qIdx = player.index + 1 + i;

      const li  = document.createElement('li');
      li.dataset.queueIndex = qIdx;

      const title = document.createElement('span');
      title.className   = 'sq-title';
      title.textContent = song.title;
      title.title       = song.artist ? `${song.artist} — ${song.title}` : song.title;

      const btn = document.createElement('button');
      btn.className = 'sq-remove mi';
      btn.title     = 'Remove from queue';
      btn.textContent = 'delete';

      li.appendChild(title);
      li.appendChild(btn);
      list.appendChild(li);
      });
   }

// Jump to a queued track (skipping over anything before it) or remove it.
document.getElementById('sidebar-queue-list').addEventListener('click', e => {
   const li = e.target.closest('li[data-queue-index]');
   if (!li) return;
   const qIdx = parseInt(li.dataset.queueIndex, 10);

   if (e.target.closest('.sq-remove')) {
      // Remove this entry from the queue.
      player.queue.splice(qIdx, 1);
      // Keep autoFrom consistent: if the removed slot was before the auto
      // boundary, the boundary shifts down by one.
      if (qIdx < player.autoFrom) player.autoFrom--;
      sidebarQueueUpdate();
      }
   else {
      // Jump to this track; everything before it is simply skipped.
      player.index = qIdx;
      playerPlay();
      }
   });

// Click cover art in the player bar to navigate to the album's track listing.
document.getElementById('player-cover').addEventListener('click', () => {
   const ctx = player.albumCtx;
   if (!ctx) return;
   history.pushState({view: 'tracks', albumId: ctx.albumId, albumTitle: ctx.albumTitle, artistId: ctx.artistId, artistName: ctx.artistName}, '');
   viewTracks(ctx.albumId, ctx.albumTitle, ctx.artistId, ctx.artistName);
   });

function scrobbleCurrentSong() {
   const song = player.queue[player.index];
   if (!song || player.scrobbled) return;
   player.scrobbled = true;
   apiCall('scrobble', {id: song.id, submission: true})
      .catch(err => console.warn('[scrobble] failed', err));
}

// Bound to both the audio and the video element, so whichever is active
// behaves identically.  The handler bodies address player.media rather than
// the element they are bound to, which is safe because only the active element
// has a src — the idle one fires nothing.
function bindMediaEvents(el) {

// Auto-advance to next track.
el.addEventListener('ended', () => {
   if (castDeviceId !== null) return;  // cast poll loop owns advance
   scrobbleCurrentSong();
   if (player.index < player.queue.length - 1) {
      player.index++;
      playerPlay();
      }
   });

// Keep seek bar and time display in sync while playing locally.
// In cast mode the poll loop owns the UI; ignore audio element events.
el.addEventListener('timeupdate', () => {
   if (castDeviceId !== null) return;
   const seek = document.getElementById('player-seek');
   const time = document.getElementById('player-time');
   // localOffset > 0 when the server is transcoding from a seek point — the
   // audio element's currentTime is relative to that slice, so add the
   // offset back to recover absolute song time.
   const cur  = player.media.currentTime + (player.localOffset || 0);
   // Prefer the duration from the song metadata over the audio element's
   // reported duration: a transcoded mp3 stream (Transfer-Encoding: chunked,
   // no XING header) makes Firefox extrapolate duration from bytes received,
   // so audio.duration grows as the track buffers.  song.duration is the
   // authoritative value from the source file's metadata.
   const song = player.queue[player.index];
   const dur  = song?.duration || player.media.duration || 0;
   if (song && !player.scrobbled && dur > 0 && cur / dur >= 0.5) scrobbleCurrentSong();
   if (!seek.dataset.seeking) {
      seek.max   = Math.floor(dur);
      seek.value = Math.floor(cur);
      }
   time.textContent = `${fmtDuration(Math.floor(cur))} / ${fmtDuration(Math.floor(dur))}`;
   videoChapterTick(cur);
   });

// Guarded like the rest, and it did not used to need to be: while casting the
// element was paused with no src and so fired nothing.  It fires plenty now —
// castSyncTick() pauses and plays the local picture to follow the receiver —
// and every one of those would fight onCastStatus for the glyph, which is
// supposed to report what the *receiver* is doing.
el.addEventListener('play',  () => {
   if (castDeviceId !== null) return;
   playerPlayGlyph('pause');
   });
el.addEventListener('pause', () => {
   if (castDeviceId !== null) return;
   playerPlayGlyph('play_arrow');
   });

// canPlayType() lies in some browser/codec pairings — Firefox claims it can
// play audio/mp4 ('maybe') but then fails on the AAC payload with
// NS_ERROR_DOM_MEDIA_METADATA_ERR.  When the optimistic direct stream fails
// to decode, retry once asking the server to transcode to mp3.  We don't
// know up-front which (browser, container, codec) triples are bad — letting
// the actual decoder be the source of truth keeps this format-list free.
el.addEventListener('error', () => {
   if (castDeviceId !== null) {
      // The sound is on the receiver and is unaffected; only the picture
      // failed.  Fall back to the panel rather than tearing down the session,
      // and do not try the mp3 retry below — this element is already silent.
      if (!castVideoLocal) return;
      console.warn('[cast] local picture failed, showing the panel instead:',
                   player.videoEl.error?.message);
      castLocalVideoStop();
      videoCastPanel(true, !castReceiverVideo);
      videoPreparing(false);
      return;
      }
   const err = player.media.error;
   const song = player.queue[player.index];
   if (!err || !song) return;
   // 3 = MEDIA_ERR_DECODE, 4 = MEDIA_ERR_SRC_NOT_SUPPORTED.
   if (err.code !== 3 && err.code !== 4) {
      console.warn('[player] audio error (code', err.code, ') at',
                   Math.floor(player.media.currentTime + (player.localOffset || 0)), 's,',
                   'song', song.id, ':', err.message);
      return;
      }
   if (player.streamFallbackTried) return;
   // The fallback is format=mp3, which for a video would request the
   // soundtrack alone — the picture would vanish and the player would look
   // like it had merely lost its cover art.  A video that will not decode is
   // a real failure and should say so.
   //
   // Unless the soundtrack alone is what was asked for, in which case this is
   // already an audio stream and mp3 is the ordinary fallback for one.
   if (song.isVideo && !videoAudioOnly.get()) {
      showError(`Cannot play “${song.title}”: the browser could not decode ` +
                `the video stream.`);
      return;
      }
   const offset = player.media.currentTime || 0;
   console.warn('[player] decode failed (code', err.code,
                '), retrying with format=mp3');
   // Defer one microtask: setting audio.src synchronously inside an
   // 'error' handler races with Firefox's error-state cleanup and the new
   // src is occasionally ignored.  A microtask gap lets the element settle
   // before the resource-selection algorithm runs again.
   queueMicrotask(() => playerPlay(offset, true));
   });

}

bindMediaEvents(player.audioEl);

// Wire control buttons and MediaSession handlers. Called once from showShell().
function setupPlayer() {
   player.videoEl = document.getElementById('video-el');
   bindMediaEvents(player.videoEl);
   setupVideoSurface();

   document.getElementById('player-playpause').addEventListener('click', () => {
      if (castDeviceId !== null) {
         const btn = document.getElementById('player-playpause');
         // Flip immediately so the UI responds without waiting for the next poll.
         if (btn.textContent === 'play_arrow') {
            playerPlayGlyph('pause');
            apiCall('castControl', {action: 'play'}).catch(() => {});
            } else {
            playerPlayGlyph('play_arrow');
            apiCall('castControl', {action: 'pause'}).catch(() => {});
            }
         return;
         }
      if (player.media.paused) player.media.play();
      else                     player.media.pause();
      });

   // Restart if more than 3 s in, otherwise go to previous track (standard UX).
   document.getElementById('player-prev').addEventListener('click', () => {
      const absCur = player.media.currentTime + (player.localOffset || 0);
      if (absCur > 3) {
         if (player.streamIsTranscoded && player.localOffset > 0) {
            // Re-fetch from offset 0 so "rewind to start" works even when the
            // current stream began at a seek point.
            playerPlay(0, true);
            } else {
            player.media.currentTime = 0;
            }
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
      playerSeekTo(Number(seek.value));
      delete seek.dataset.seeking;
      });

   document.getElementById('player-cast').addEventListener('click', openCastModal);
   document.getElementById('cover-lightbox').addEventListener('click', () => {
      const el = document.getElementById('cover-lightbox');
      el.classList.add('hidden');
      document.getElementById('cover-lightbox-img').src = '';
      });

   document.getElementById('cast-close-btn').addEventListener('click', () => {
      document.getElementById('cast-modal').classList.add('hidden');
      });
   document.getElementById('cast-stop-btn').addEventListener('click', stopCast);

   document.getElementById('player-info-btn').addEventListener('click', openInfoModal);
   document.getElementById('info-close-btn').addEventListener('click', () => {
      document.getElementById('info-modal').classList.add('hidden');
      });

   document.querySelector('.nav-title').addEventListener('click', () => {
      document.getElementById('about-version').textContent = serverVersion ?? '?';
      document.getElementById('about-modal').classList.remove('hidden');
      });
   document.getElementById('about-close-btn').addEventListener('click', () => {
      document.getElementById('about-modal').classList.add('hidden');
      });

   document.getElementById('playlist-close-btn').addEventListener('click', () => {
      document.getElementById('playlist-modal').classList.add('hidden');
      });
   document.getElementById('error-close-btn').addEventListener('click', () => {
      document.getElementById('error-modal').classList.add('hidden');
      });
   document.getElementById('confirm-no-btn').addEventListener('click', () => {
      document.getElementById('confirm-modal').classList.add('hidden');
      _confirmYes = null;
      });
   document.getElementById('confirm-yes-btn').addEventListener('click', () => {
      document.getElementById('confirm-modal').classList.add('hidden');
      const cb = _confirmYes;
      _confirmYes = null;
      if (cb) cb();
      });
   document.getElementById('playlist-new-btn').addEventListener('click', async () => {
      const name = document.getElementById('playlist-new-name').value.trim();
      if (!name) return;
      const sr = await apiCall('createPlaylist', {name});
      const newId = sr.playlist?.id;
      if (newId && playlistTargetId)
         await apiCall('updatePlaylist', {playlistId: newId, songIdToAdd: playlistTargetId});
      document.getElementById('playlist-modal').classList.add('hidden');
      });

   {
      const fileInput = document.getElementById('cover-art-file-input');
      const fileName  = document.getElementById('cover-art-file-name');
      const urlInput  = document.getElementById('cover-art-url');
      const urlPrev   = document.getElementById('cover-art-url-preview');

      document.getElementById('cover-art-file-btn')
         .addEventListener('click', () => fileInput.click());
      fileInput.addEventListener('change', () => {
         const f = fileInput.files[0];
         if (!f) return;
         fileName.textContent = f.name;
         const cb = _coverArtCb;
         _closeCoverArtDialog();
         fileInput.value = '';
         if (cb) cb({kind: 'file', file: f});
         });

      urlInput.addEventListener('input', () => {
         const v = urlInput.value.trim();
         if (v) { urlPrev.src = v; urlPrev.classList.remove('hidden'); }
         else   { urlPrev.removeAttribute('src'); urlPrev.classList.add('hidden'); }
         });
      urlPrev.addEventListener('error', () => urlPrev.classList.add('hidden'));

      document.getElementById('cover-art-url-btn').addEventListener('click', () => {
         const url = urlInput.value.trim();
         if (!url) return;
         const cb = _coverArtCb;
         _closeCoverArtDialog();
         if (cb) cb({kind: 'url', url});
         });
      document.getElementById('cover-art-cancel-btn')
         .addEventListener('click', _closeCoverArtDialog);
      }

   document.getElementById('promote-cancel-btn')
      .addEventListener('click', _closePromoteDialog);
   document.getElementById('promote-go-btn').addEventListener('click', () => {
      const cb   = _promoteCb;
      const root = document.getElementById('promote-root').value;
      const name = document.getElementById('promote-folder').value.trim();
      // Both are required by the server, and showPromoteDialog keeps the button
      // disabled until both are set — so this cannot fire without them.
      if (!root || !name) return;
      _closePromoteDialog();
      if (cb) cb(root, name);
      });

   if ('mediaSession' in navigator) {
      navigator.mediaSession.setActionHandler('play',          () => player.media.play());
      navigator.mediaSession.setActionHandler('pause',         () => player.media.pause());
      navigator.mediaSession.setActionHandler('previoustrack', () => {
         document.getElementById('player-prev').click();
         });
      navigator.mediaSession.setActionHandler('nexttrack',     () => {
         document.getElementById('player-next').click();
         });
      navigator.mediaSession.setActionHandler('seekto', details => {
         player.media.currentTime = details.seekTime;
         });
      }

   // Prompt before leaving the page while a track is playing, so an accidental
   // browser refresh or back-navigation doesn't silently kill playback.
   window.addEventListener('beforeunload', e => {
      if (castDeviceId !== null || !player.media.paused)
         e.returnValue = '';
      });
}

async function viewTracks(albumId, albumTitle, artistId, artistName,
                           autoPlayId = null, autoPlayOffset = 0) {
   console.log('[tracks] loading album', albumId, albumTitle);
   const pane = document.getElementById('pane-tracks');
   pane.innerHTML = '';

   let sr;
   try {
      sr = await apiCall('getAlbum', {id: albumId});
      }
   catch (e) {
      showError(e?.message ?? 'Could not reach the server. Please check your connection.');
      return;
      }
   const album = sr.album ?? {};
   const songs = album.song ?? [];
   console.log('[tracks] got', songs.length, 'tracks');

   // Make sure pane 1 (artist's albums) is in sync with what's in pane 2.
   // Entry paths like starred-album, starred-track, search-result, and the
   // player-cover thumbnail land here without first walking through
   // viewAlbums(), so pane 1 would otherwise stay empty or hold stale
   // content (e.g. playlist tracks). We use album.parent (artist id from
   // getAlbum) so this also works when the caller passed a null artistId.
   // Pane 1 is considered fresh only when its dataset marker matches AND it
   // still actually contains album rows — playlist track listings, search
   // result lists, etc. all leave the marker irrelevant.
   const albumsPane = document.getElementById('pane-albums');
   const pane1Fresh = albumsPane.dataset.artistId === String(album.parent)
                      && albumsPane.querySelector('.album-row');
   if (album.parent !== undefined && !pane1Fresh)
      await viewAlbums(album.parent, album.artist ?? artistName ?? '');

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
   const albumStar = makeAlbumStar(album);
   albumStar.classList.add('album-star-header');
   const editLink = document.createElement('span');
   editLink.className = 'edit-link';
   editLink.textContent = 'Edit';
   header.appendChild(back);
   header.appendChild(heading);
   header.appendChild(albumStar);
   header.appendChild(editLink);
   pane.appendChild(header);

   // Large cover art hero with carousel support for extra images.
   let heroImg = null;
   let placeholder = null;
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

      // 320 is .cover-hero-wrap's own max width; the image is 100% of it.
      heroImg = document.createElement('img');
      heroImg.className = 'album-hero';
      heroImg.dataset.albumId   = album.id;
      heroImg.dataset.coverSize = coverPx(HERO_BOX);
      heroImg.src = apiUrl('getCoverArt',
                           {id: album.coverArt, size: coverPx(HERO_BOX)});
      heroImg.alt = albumTitle;
      heroImg.addEventListener('click', () => {
         if (heroWrap.classList.contains('editing')) return;
         const params = {id: album.coverArt};
         if (carouselIdx > 0) params.index = carouselIdx;
         showLightbox(apiUrl('getCoverArt', params));
         });

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
         heroImg.src = apiUrl('getCoverArt',
            {id: album.coverArt, size: coverPx(HERO_BOX), index: idx});
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

   // The chapter index for this folder, keyed by video id. From the scan's
   // table rather than from getChapters per video: this is a browse path, and
   // reading a sidecar -- or worse, running ffprobe for a video without one --
   // every time somebody opens an album is the cost that table removes.
   //
   // Failure is a non-event: an album simply lists its videos the way it
   // always did. This is decoration over a listing that already works.
   const chaptersByVideo = new Map();
   try {
      const cr = await apiCall('getAlbumChapters', {id: albumId});
      for (const v of cr.albumChapters?.song ?? [])
         chaptersByVideo.set(v.id, v.chapter ?? []);
      }
   catch (e) { console.warn('[chapters] album index unavailable', e); }

   const frag = document.createDocumentFragment();
   const multiDisc = new Set(songs.map(s => s.discNumber ?? 1)).size > 1;
   // The same rule multiDisc follows, and for the same reason: a heading is
   // there to say *which* group a row belongs to, so a folder holding one
   // chaptered recording needs none. Counted over what will actually be drawn
   // rather than over the map, so a stray empty entry cannot conjure one.
   const multiChaptered =
      [...chaptersByVideo.values()].filter(c => c?.length).length > 1;
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
         // season is a gaindrive extension carrying the same number as
         // discNumber, present only when the grouping really is a season. Read
         // per group rather than per album, so a show with an unnumbered
         // Specials folder still heads that one "Disc".
         dh.textContent = `${song.season > 0 ? 'Series' : 'Disc'} ${disc}`;
         frag.appendChild(dh);
         }

      const row = document.createElement('div');
      row.className = 'track-row';
      row.dataset.id   = song.id;
      row.dataset.disc = song.discNumber ?? 1;
      row.dataset.year = song.year ?? '';
      row.dataset.dur  = song.duration ? fmtDuration(song.duration) : '';

      const icon = document.createElement('span');
      icon.className = 'track-icon';

      const num = document.createElement('span');
      num.className = 'track-num';
      num.textContent = useSeq ? (i + 1) : (song.track ?? '');

      // Title and, when the file disagrees with its folder, the track's own
      // artist stacked under it — the same shape the playlist listing uses.
      // The wrap is the row's grid cell, not the title: edit mode swaps this
      // element for an <input>, and replaceChild only reaches a direct child.
      const titleWrap = document.createElement('span');
      titleWrap.className = 'track-title-wrap';
      const title = document.createElement('span');
      title.className = 'track-title';
      title.textContent = song.title;
      titleWrap.appendChild(title);
      // The server decided what counts as a difference — it sends the folder's
      // spelling in `artist` when the tag is merely another way of writing the
      // same name — so this is an exact comparison and nothing more.
      row.dataset.trackArtist =
         (song.displayAlbumArtist && song.artist !== song.displayAlbumArtist)
            ? song.artist : '';
      if (row.dataset.trackArtist) {
         const artistEl = document.createElement('span');
         artistEl.className = 'track-artist';
         artistEl.textContent = row.dataset.trackArtist;
         titleWrap.appendChild(artistEl);
         }

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
      const [starBtn, listBtn] = makeTrackActions(song);
      row.appendChild(icon);
      row.appendChild(num);
      row.appendChild(titleWrap);
      row.appendChild(starBtn);
      row.appendChild(listBtn);
      row.appendChild(dur);
      frag.appendChild(row);

      // A chaptered recording stands in for itself: its markers become the
      // rows, under a heading naming it when the folder holds more than one.
      // Its own row stays in the DOM but hidden, because edit mode needs
      // something to edit -- chapter rows alone would leave a concert's title
      // and year uneditable.
      const chapters = chaptersByVideo.get(song.id);
      if (chapters?.length) {
         row.classList.add('has-chapters');
         if (multiChaptered) {
            const vh = document.createElement('div');
            vh.className = 'chapters-heading';
            vh.textContent = song.title;
            frag.appendChild(vh);
            }
         for (const c of chapters)
            frag.appendChild(makeChapterRow(song, c, songs, i,
               {albumId, albumTitle, artistId, artistName}));
         }
      }

   pane.appendChild(frag);
   paneNav.slideTo(2);

   if (autoPlayId !== null) {
      const idx = songs.findIndex(s => s.id === autoPlayId);
      if (idx !== -1) {
         player.albumCtx = {albumId, albumTitle, artistId, artistName};
         // The offset is how a chapter hit in search lands on its song rather
         // than at the start of a two-hour concert.
         playerLoad(songs, idx, autoPlayOffset);
         }
      }

   // ── Edit mode ──────────────────────────────────────────────────────────────
   // Toggled by the "Edit" link in the header.
   let pendingCover = null;  // {kind:'file', file} | {kind:'url', url} | null
   let nameInput    = null;  // album title input, live only while editing
   let artistInput  = null;  // artist name input, ditto

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

      // Album title and artist become inputs. Both are *directory names* on the
      // server — the scanner never reads them from tags — so saving them moves
      // files, which is why they are only editable here and not, say, inline.
      //
      // The artist comes from album.artist rather than the artistName argument:
      // callers reaching this view from search, starred or the player cover
      // pass null for it.
      nameInput = document.createElement('input');
      nameInput.type = 'text';
      nameInput.className = 'album-title-input';
      nameInput.value = nameInput.dataset.orig = heading.textContent;
      heading.replaceWith(nameInput);

      artistInput = document.createElement('input');
      artistInput.type = 'text';
      artistInput.className = 'album-artist-input';
      artistInput.placeholder = 'Artist';
      artistInput.value = artistInput.dataset.orig = album.artist ?? artistName ?? '';
      nameInput.after(artistInput);

      // Show a placeholder tile so the pencil has somewhere to sit when there
      // is no cover yet.
      if (!heroImg) {
         placeholder = document.createElement('div');
         placeholder.className = 'album-hero album-hero-placeholder mi';
         placeholder.dataset.albumId   = album.id;
         placeholder.dataset.coverSize = coverPx(HERO_BOX);
         placeholder.textContent = 'music_note';
         heroWrap.appendChild(placeholder);
         }

      const pencilBtn = document.createElement('button');
      pencilBtn.className = 'cover-edit-btn mi';
      pencilBtn.textContent = 'edit';
      pencilBtn.setAttribute('aria-label', 'Change cover art');
      pencilBtn.addEventListener('click', () => {
         showCoverArtDialog(picked => {
            pendingCover = picked;
            if (!heroImg) {
               heroImg = document.createElement('img');
               heroImg.className = 'album-hero';
               heroImg.dataset.albumId   = album.id;
               heroImg.dataset.coverSize = coverPx(HERO_BOX);
               heroImg.alt = albumTitle;
               heroWrap.insertBefore(heroImg, pencilBtn);
               placeholder?.remove();
               placeholder = null;
               }
            if (picked.kind === 'file') {
               const reader = new FileReader();
               reader.onload = e => { heroImg.src = e.target.result; };
               reader.readAsDataURL(picked.file);
               }
            else {
               heroImg.src = picked.url;
               }
            });
         });
      heroWrap.appendChild(pencilBtn);
      heroWrap.classList.add('editing');

      // The listing swaps back to its plain form: every video's own row, no
      // chapter rows and no video headings. One class on the pane rather than
      // per-element hiding, so exitEditMode is a single removal.
      pane.classList.add('editing-tracks');

      // Replace track num/title spans with inputs; swap dur span for year input.
      //
      // Chapter rows are excluded, and not merely for tidiness: they carry no
      // .track-year-input and no track number of their own, so the assignments
      // below would throw on the first one. Editing works on the songs the
      // folder holds, and a chapter is a position inside one -- which is why
      // the video's own row is only *hidden* while its chapters are shown, and
      // comes back here.
      pane.querySelectorAll('.track-row:not(.chapter-track)').forEach(row => {
         const numSpan   = row.querySelector('.track-num');
         // The wrap, not the title: the title is nested inside it so a track
         // artist can sit under it, and replaceChild below needs the row's own
         // child. Its text is read from the title span, which is the only part
         // being edited.
         const titleWrap = row.querySelector('.track-title-wrap');
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

         const discInput = document.createElement('input');
         discInput.type = 'number';
         discInput.min  = '1';
         discInput.className = 'track-disc-input';
         discInput.value = row.dataset.disc;
         discInput.dataset.orig = row.dataset.disc;

         const yearInput = document.createElement('input');
         yearInput.type = 'number';
         yearInput.min  = '0';
         yearInput.max  = '9999';
         yearInput.className = 'track-year-input';
         yearInput.value = row.dataset.year;
         yearInput.dataset.orig = row.dataset.year;

         row.replaceChild(numInput,   numSpan);
         row.replaceChild(titleInput, titleWrap);
         row.replaceChild(yearInput,  durSpan);
         row.insertBefore(discInput, yearInput);

         // Prevent row click (play) while editing.
         row.classList.add('editing');
         });

      // 'all' button: copies the first track's year to all others on the same disc.
      // 'all' for year: copies first year within the same disc section.
      function makeYearAllBtn(discHeading) {
         const btn = document.createElement('button');
         btn.className = 'year-all-btn';
         btn.textContent = 'all';
         btn.addEventListener('click', () => {
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

      // 'all' for CD: copies first disc value to every track in the album.
      function makeCdAllBtn() {
         const btn = document.createElement('button');
         btn.className = 'cd-all-btn';
         btn.textContent = 'all';
         btn.addEventListener('click', () => {
            let firstDisc = null;
            pane.querySelectorAll('.track-row').forEach(r => {
               const di = r.querySelector('.track-disc-input');
               if (!di) return;
               if (firstDisc === null) firstDisc = di.value;
               else di.value = firstDisc;
               });
            });
         return btn;
         }

      // Always create a header row: CD 'all' button always lives here.
      // Year 'all' button also lives here for single-disc albums; for multi-disc
      // it goes into each disc heading instead.
      const editHeader = document.createElement('div');
      editHeader.className = 'track-edit-header';
      editHeader.appendChild(makeCdAllBtn());

      const discHeadings = [...pane.querySelectorAll('.disc-heading')];
      if (discHeadings.length > 0) {
         // Multi-disc: year 'all' in each disc heading.
         discHeadings.forEach(dh => {
            dh.classList.add('editing');
            dh.appendChild(makeYearAllBtn(dh));
            });
         }
      else {
         // Single disc: year 'all' also in the header.
         editHeader.appendChild(makeYearAllBtn(null));
         }

      const firstTrackEl = pane.querySelector('.track-row');
      if (firstTrackEl) pane.insertBefore(editHeader, firstTrackEl);
      else pane.appendChild(editHeader);

      saveBtn.addEventListener('click', async () => {
         saveBtn.disabled = true;
         cancelBtn.disabled = true;

         const errors = [];

         // Save cover art first, if changed.
         if (pendingCover) {
            try {
               const {server} = creds.load();
               const p = new URLSearchParams({...authParams(),
                  v: '1.16.1', c: 'gaindrive-web', f: 'json', id: albumId});
               const fd = new FormData();
               if (pendingCover.kind === 'file') fd.append('file', pendingCover.file);
               else                              fd.append('url',  pendingCover.url);
               const resp = await fetch(`${server}/rest/setCoverArt.view?${p}`,
                  {method: 'POST', body: fd});
               if (!resp.ok) throw new Error(`HTTP ${resp.status}`);
               const data = await resp.json();
               const sr = data['subsonic-response'];
               if (sr.status !== 'ok')
                  throw new Error(sr.error?.message ?? 'Unknown error');
               dispatchCoverArtChanged(albumId);
               }
            catch (e) {
               console.error('[edit] cover upload failed', e);
               errors.push(`Cover art: ${e.message}`);
               }
            }

         // Save changed tracks.
         for (const row of pane.querySelectorAll('.track-row')) {
            const songId     = row.dataset.id;
            const numInput   = row.querySelector('.track-num-input');
            const titleInput = row.querySelector('.track-title-input');
            const discInput  = row.querySelector('.track-disc-input');
            const yearInput  = row.querySelector('.track-year-input');
            if (!numInput || !titleInput || !discInput || !yearInput) continue;

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
            if (discInput.value !== discInput.dataset.orig) {
               params.disc = discInput.value;
               changed = true;
               }
            if (yearInput.value !== yearInput.dataset.orig) {
               params.year = yearInput.value;
               changed = true;
               }
            if (changed) {
               try { await apiCall('updateSong', params); }
               catch (e) {
                  console.error('[edit] updateSong failed', e);
                  errors.push(`Track "${titleInput.value}": ${e.message}`);
                  }
               }
            }

         // The rename goes last, and that ordering is load-bearing: it moves the
         // directory, so every song id and the album id above it stop resolving.
         // The per-track updateSong calls have to have happened already.
         const newName   = nameInput.value.trim();
         const newArtist = artistInput.value.trim();
         const renaming  = errors.length === 0
            && (newName   !== nameInput.dataset.orig
                || newArtist !== artistInput.dataset.orig);
         let renamed = null;
         if (renaming) {
            if (!newName || !newArtist) {
               errors.push('Album and artist names cannot be empty.');
               }
            else {
               try {
                  // No musicFolderId, so moveAlbum keeps everything above the
                  // artist level and this is a rename in place. `folder` is
                  // the artist level under whatever root the album is already
                  // in — an artist here, since the edit screen is only drawn
                  // for an album that has one.
                  const sr = await apiCall('moveAlbum',
                     {id: albumId, album: newName, folder: newArtist});
                  renamed = sr.movedAlbum ?? null;
                  }
               catch (e) {
                  console.error('[edit] moveAlbum failed', e);
                  errors.push(`Rename: ${e.message}`);
                  }
               }
            }

         if (errors.length > 0) {
            showError('Some changes could not be saved:\n\n' + errors.join('\n'));
            saveBtn.disabled = false;
            cancelBtn.disabled = false;
            return;
            }

         if (renamed) {
            if (Number(renamed.tagFailures) > 0)
               showError(`Renamed, but the tags in ${renamed.tagFailures} file(s) `
                  + 'could not be rewritten.');
            // Every id in this pane has just changed, so there is nothing to
            // patch in place — re-render all three panes against the new ones.
            // Pane 0 too: re-filing may have created an artist or emptied one.
            await viewArtists();
            await viewAlbums(renamed.parent, renamed.artist,
                             libraryMode === 'categories');
            await viewTracks(renamed.id, renamed.album, renamed.parent, renamed.artist);
            return;
            }

         exitEditMode(true);
         });

      cancelBtn.addEventListener('click', () => {
         pendingCover = null;
         exitEditMode(false);
         });
      }

   function exitEditMode(keepValues) {
      // Restore Edit link.
      editLink.textContent = 'Edit';

      // Put the heading back. On a successful rename this function is not
      // reached at all — the view is re-rendered instead — so keepValues here
      // only ever restores a name that was never sent.
      if (nameInput) {
         heading.textContent = keepValues ? nameInput.value : nameInput.dataset.orig;
         nameInput.replaceWith(heading);
         nameInput = null;
         }
      artistInput?.remove();
      artistInput = null;

      // Remove pencil button and edit-mode placeholder from heroWrap.
      heroWrap.querySelector('.cover-edit-btn')?.remove();
      heroWrap.classList.remove('editing');
      placeholder?.remove();
      placeholder = null;

      // If cancelled, restore original cover state.
      if (!keepValues) {
         if (album.coverArt && heroImg) {
            heroImg.src = apiUrl('getCoverArt',
                                 {id: album.coverArt, size: coverPx(HERO_BOX)});
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
      pane.classList.remove('editing-tracks');
      pane.querySelectorAll('.track-row:not(.chapter-track)').forEach(row => {
         const numInput   = row.querySelector('.track-num-input');
         const titleInput = row.querySelector('.track-title-input');
         const discInput  = row.querySelector('.track-disc-input');
         const yearInput  = row.querySelector('.track-year-input');
         if (!numInput || !titleInput || !yearInput) return;

         const numSpan = document.createElement('span');
         numSpan.className = 'track-num';
         numSpan.textContent = keepValues ? numInput.value : numInput.dataset.orig;

         // Rebuilt rather than kept aside, so a saved title is what comes
         // back. The artist is redrawn from the row's dataset: it is not
         // editable here, and nothing in this pass can have changed it.
         const titleWrap = document.createElement('span');
         titleWrap.className = 'track-title-wrap';
         const titleSpan = document.createElement('span');
         titleSpan.className = 'track-title';
         titleSpan.textContent = keepValues ? titleInput.value : titleInput.dataset.orig;
         titleWrap.appendChild(titleSpan);
         if (row.dataset.trackArtist) {
            const artistEl = document.createElement('span');
            artistEl.className = 'track-artist';
            artistEl.textContent = row.dataset.trackArtist;
            titleWrap.appendChild(artistEl);
            }

         const durSpan = document.createElement('span');
         durSpan.className = 'track-dur';
         durSpan.textContent = row.dataset.dur;

         // Update stored disc/year if saved, so re-entering edit mode shows new values.
         if (keepValues && discInput) row.dataset.disc = discInput.value;
         if (keepValues) row.dataset.year = yearInput.value;

         discInput?.remove();
         row.replaceChild(numSpan,   numInput);
         row.replaceChild(titleWrap, titleInput);
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
      prev.className = 'liner-notes-btn mi';
      prev.textContent = 'chevron_left';
      prev.setAttribute('aria-label', 'Previous text file');

      const next = document.createElement('button');
      next.className = 'liner-notes-btn mi';
      next.textContent = 'chevron_right';
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
}

let _searchTimer = null;

function scheduleSearch() {
   clearTimeout(_searchTimer);
   _searchTimer = setTimeout(() => runSearch().catch(() => {}), 320);
}

async function runSearch() {
   const q = document.getElementById('search-input').value.trim();
   if (!q) return;
   const wantArtists = document.getElementById('sf-artists').checked;
   const wantAlbums  = document.getElementById('sf-albums').checked;
   const wantSongs   = document.getElementById('sf-songs').checked;

   let sr;
   try {
      sr = await apiCall('search3', {
         query:       q,
         artistCount: wantArtists ? 20 : 0,
         albumCount:  wantAlbums  ? 20 : 0,
         songCount:   wantSongs   ? 20 : 0,
         // Rides with the Songs filter: a chapter is a song inside a video,
         // so somebody who has turned songs off is not looking for one. The
         // server defaults this to 0, so an older client costs nothing.
         chapterCount: wantSongs  ? 20 : 0,
         });
      }
   catch {
      showError('Could not reach the server. Please check your connection.');
      return;
      }
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

   const srHeader = document.createElement('div');
   srHeader.className = 'view-header';
   const srTitle = document.createElement('h1');
   srTitle.className = 'view-title';
   srTitle.textContent = 'Search';
   srHeader.appendChild(srTitle);
   frag.appendChild(srHeader);

   if (artists.length === 0 && albums.length === 0 && songs.length === 0) {
      const msg = document.createElement('p');
      msg.className = 'search-no-results';
      msg.textContent = 'No results found.';
      frag.appendChild(msg);
      pane.replaceChildren(frag);
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

         // The portrait comes from our own server, not from the URL
         // getArtistInfo2 reports. That URL points at Wikimedia, so every
         // client of every install used to fetch a full-size original
         // straight from the internet — slow on a LAN, impossible offline,
         // and one getArtistInfo2 round trip per row on top. Same argument
         // that put the icon font in the binary.
         const img = artistPortrait(artist.id, coverPx(80),
                                    'search-artist-img');
         row.appendChild(img);

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

         const cover = makeAlbumCover(album);

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
         row.addEventListener('click', () => viewTracksFromSearch(album.id, album.title, album.parent, album.artist));
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

         row.addEventListener('click', () => viewTracksFromSearch(song.parent, song.album, null, song.artist, song.id));
         frag.appendChild(row);
         }
      }

   // Songs inside a video — a section of their own, because that is how the
   // server reports them. A chapter has no id anything can stream or star, so
   // it is not a song entry and must not be drawn as one; what it does have is
   // a film and a position in it, which is enough to play from.
   const chapters = res.chapter ?? [];
   if (chapters.length > 0) {
      const h = document.createElement('h2');
      h.className = 'index-heading';
      h.textContent = 'Chapters';
      frag.appendChild(h);

      for (const c of chapters) {
         const row = document.createElement('div');
         row.className = 'search-song-row';

         const info = document.createElement('div');
         info.className = 'search-song-info';

         const titleEl = document.createElement('span');
         titleEl.className = 'search-song-title';
         titleEl.textContent = c.name || `Chapter ${c.index}`;

         const sub = document.createElement('span');
         sub.className = 'search-song-sub';
         // Artist, album, then the track itself: a marker means nothing
         // without knowing which concert it is in, and the album is what the
         // folder is called while the track is what the file is called. They
         // coincide often enough -- a folder holding one recording named after
         // it -- that an exact duplicate is dropped rather than printed twice.
         const where = [c.artist, c.album];
         if (c.track && c.track !== c.album) where.push(c.track);
         sub.textContent = where.filter(Boolean).join(' · ');

         info.append(titleEl, sub);
         row.appendChild(info);

         const at = document.createElement('span');
         at.className = 'search-song-dur';
         at.textContent = fmtChapterTime(c.start);
         row.appendChild(at);

         row.addEventListener('click', () => viewTracksFromSearch(
            c.parent, c.album, null, c.artist, c.songId, c.start));
         frag.appendChild(row);
         }
      }

   pane.replaceChildren(frag);
}

// Wrapper around viewTracks for clicks from search results.
// On a 2-pane layout, viewTracks slides to depth 2, which pushes pane 0
// (search results) off-screen. Instead we move the rendered content into
// pane 1 and stay at depth 1, keeping search visible on the left.
async function viewTracksFromSearch(albumId, albumTitle, artistId, artistName,
                                     autoPlayId, autoPlayOffset = 0) {
   await viewTracks(albumId, albumTitle, artistId, artistName, autoPlayId,
                    autoPlayOffset);
   if (document.getElementById('search-bar').classList.contains('open')
         && paneNav._visiblePanes() === 2) {
      const p1 = document.getElementById('pane-albums');
      const p2 = document.getElementById('pane-tracks');
      p1.replaceChildren(...Array.from(p2.childNodes));
      paneNav.slideTo(1);
      }
   }

function setupSearch() {
   document.getElementById('search-btn').addEventListener('click', e => { e.preventDefault(); openSearchBar(); });
   document.getElementById('search-btn-mobile').addEventListener('click', e => {
      e.preventDefault();
      openSearchBar();
      });
   document.getElementById('search-close').addEventListener('click', closeSearchBar);
   document.getElementById('search-input').addEventListener('input', scheduleSearch);

   for (const id of ['sf-artists', 'sf-albums', 'sf-songs'])
      document.getElementById(id).addEventListener('change', scheduleSearch);
}

// ── Keyboard ────────────────────────────────────────────────────────────────

// One table drives both the key handler and the ? overlay, so the list a viewer
// is shown cannot drift from what the keys actually do.  Fields:
//
//  * `when` decides whether the shortcut is *applicable*.  It is asked before
//    the key fires and again when the overlay is drawn, so a shortcut can never
//    do something the interface is not currently offering, and the overlay
//    cannot claim it can.
//  * `run` performs it, delegating to the button it mirrors wherever one
//    exists.  That is the pattern the client already uses — #video-play clicks
//    #player-playpause, the Media Session keys click #player-prev/-next — and
//    it is what makes the gating free: #player-cast is hidden without castRole,
//    both chapter buttons are hidden on a film with no markers, and
//    #video-fullscreen is hidden while casting.  Testing the button is
//    therefore the whole of `when` in three of the four cases.
//  * `label` may be a function, so `q` can read the live title off
//    #video-close.  That button already flips between "Stop and close" and
//    "Close" depending on whether a cast is running, and a second copy of that
//    distinction here would be a second thing to keep in step.
//
// This replaced a rule that opened the search bar on *any* unclaimed printable
// key.  Every letter promoted to a shortcut was a letter search silently lost,
// which does not scale to a client meant to be driven from the keyboard, so
// search now has a key of its own and the alphabet is free.

// True when the element exists and is not hidden — the whole of most `when`
// predicates, since the shortcut is only offering what the button offers.
function keyShown(id) {
   const el = document.getElementById(id);
   return !!el && !el.hidden;
}

function videoOnScreen() {
   return !document.getElementById('video-surface').classList.contains('hidden');
}

// Fullscreen renders only #video-frame's subtree, so a modal parented on <body>
// is not drawn at all — the same constraint that put #video-chapters inside
// the frame and gave the chapter toggle a second home there.  A key that opens
// a dialog therefore has to leave fullscreen first, or it looks dead.
function keyLeaveFullscreen() {
   if (document.fullscreenElement)
      document.exitFullscreen?.().catch(() => {});
}

const SHORTCUTS = [
   {group: 'Playback', key: ' ', show: 'Space', label: 'Play or pause',
    when: () => true,
    run:  () => document.getElementById('player-playpause').click()},
   {group: 'Playback', key: 'ArrowLeft', show: '←',
    label: `Back ${SKIP_SECS} seconds`,
    when: () => true, run: () => playerSkip(-SKIP_SECS)},
   {group: 'Playback', key: 'ArrowRight', show: '→',
    label: `Forward ${SKIP_SECS} seconds`,
    when: () => true, run: () => playerSkip(SKIP_SECS)},

   {group: 'Video', key: 'f', show: 'F', label: 'Fullscreen',
    when: () => videoOnScreen() && keyShown('video-fullscreen'),
    run:  videoFullscreenToggle},
   {group: 'Video', key: 'l', show: 'L', label: 'Chapter list',
    when: () => videoOnScreen() && keyShown('video-chapters-btn2'),
    run:  videoChaptersToggle},
   {group: 'Video', key: 'q', show: 'Q',
    // Whatever the button says it does, which is not the same sentence while
    // casting: there it only puts the picture away and leaves the sound in the
    // other room alone.
    label: () => document.getElementById('video-close').title,
    when: videoOnScreen,
    run:  () => document.getElementById('video-close').click()},

   {group: 'Elsewhere', key: 'c', show: 'C', label: 'Cast to a device',
    when: () => keyShown('player-cast'),
    run:  () => { keyLeaveFullscreen();
                  document.getElementById('player-cast').click(); }},
   {group: 'Elsewhere', key: '/', show: '/', label: 'Search',
    when: () => true, run: openSearchBar},
   {group: 'Elsewhere', key: '?', show: '?', label: 'This list',
    when: () => true, run: keysToggle},
];

function keysToggle() {
   const modal = document.getElementById('keys-modal');
   if (!modal.classList.contains('hidden')) {
      modal.classList.add('hidden');
      return;
      }
   keyLeaveFullscreen();
   keysRender();
   modal.classList.remove('hidden');
}

// Drawn at open time, so it is a snapshot of what applies now.  Everything is
// listed and the inapplicable entries are dimmed rather than dropped: hiding
// them would make the client look like it had three shortcuts whenever no film
// was playing, and the dimmed row is also the answer to "why did F do nothing".
function keysRender() {
   const list = document.getElementById('keys-list');
   list.replaceChildren();
   let group = null;
   for (const sc of SHORTCUTS) {
      if (sc.group !== group) {
         group = sc.group;
         const h = document.createElement('div');
         h.className   = 'keys-group';
         h.textContent = group;
         list.appendChild(h);
         }
      const on = sc.when();
      const dt = document.createElement('dt');
      const kb = document.createElement('kbd');
      kb.textContent = sc.show;
      dt.appendChild(kb);
      const dd = document.createElement('dd');
      dd.textContent = (typeof sc.label === 'function') ? sc.label() : sc.label;
      if (!on) { dt.classList.add('keys-off'); dd.classList.add('keys-off'); }
      list.append(dt, dd);
      }
}

// The topmost open dialog, or null.  Escape is the only way out of one by
// keyboard, since none of them handles it themselves.  Document order is
// stacking order here — every .modal shares one z-index — so the last is the
// one drawn on top.
function keyOpenModal() {
   const open = document.querySelectorAll('.modal:not(.hidden)');
   return open.length ? open[open.length - 1] : null;
}

// Dismissing is not always just hiding: three of these arm a callback that must
// not survive to answer whatever asks next, and the lightbox holds a full-size
// image.  Two already have a close function, so this dispatches to them rather
// than restating what they do; a dialog with nothing to clean up is hidden
// directly.  Escape means No on a confirmation, as its No button does.
function keyDismissModal(modal) {
   switch (modal.id) {
      case 'cover-art-modal': _closeCoverArtDialog(); return;
      case 'promote-modal':   _closePromoteDialog();  return;
      case 'confirm-modal':   _confirmYes = null;     break;
      case 'cover-lightbox':
         document.getElementById('cover-lightbox-img').src = '';
         break;
      }
   modal.classList.add('hidden');
}

function setupKeys() {
   document.addEventListener('keydown', e => {
      // Before the typing guard, deliberately: openSearchBar() focuses the
      // search box, so the box has focus at exactly the moment Escape is
      // wanted.  A dialog outranks the search bar because it is drawn over it.
      if (e.key === 'Escape') {
         const modal = keyOpenModal();
         if (modal) {
            keyDismissModal(modal);
            e.preventDefault();
            }
         else if (document.getElementById('search-bar').classList.contains('open')) {
            closeSearchBar();
            e.preventDefault();
            }
         return;
         }

      if (e.ctrlKey || e.metaKey || e.altKey) return;

      // tagName is still the whole test: there is no contenteditable anywhere
      // in the client.  It covers the chapter time and name fields, the album,
      // track and playlist edit inputs, and #player-seek — which is a range
      // input, and so keeps its own arrow-key behaviour for free.
      const tag = document.activeElement?.tagName;
      if (tag === 'INPUT' || tag === 'TEXTAREA' || tag === 'SELECT') return;

      // Nothing but ? reaches past an open dialog.  Otherwise Q would close the
      // film behind the cast chooser and Space would start playback under a
      // confirmation prompt — a key changing something the viewer cannot see
      // is the one result a shortcut set must not produce.
      if (keyOpenModal() && e.key !== '?') return;

      // Lowercased so Shift+F works.  ? is Shift+/ on most layouts and arrives
      // as ? already, which is why it is spelled that way in the table.
      const key = e.key.length === 1 ? e.key.toLowerCase() : e.key;
      const sc  = SHORTCUTS.find(s => s.key === key);
      if (!sc || !sc.when()) return;
      // Applied to every match rather than per entry: / would otherwise open
      // Firefox's quick-find, and Space and the arrows would scroll the pane
      // under the film.
      e.preventDefault();
      sc.run();
      });

   document.getElementById('keys-close-btn').addEventListener('click', keysToggle);
}

// ── Shell ───────────────────────────────────────────────────────────────────

let wiredOnce = false;

async function showShell() {
   console.log('[shell] showing main shell');
   document.getElementById('login-screen').hidden = true;
   const shell = document.getElementById('app-shell');
   shell.hidden = false;
   console.log('[shell] app-shell hidden=', shell.hidden, 'display=', getComputedStyle(shell).display);

   // Once per page load, not once per login: logging out calls showLogin() and
   // logging back in returns here without a reload, so without the guard every
   // handler below is bound a second time.  That was survivable while they were
   // all one-way — a doubled openSearchBar() opens the bar — and stops being so
   // with keys that toggle, where the second call undoes the first and 'f' and
   // '?' simply appear dead.  Nothing in these depends on who logged in.
   if (!wiredOnce) {
      wiredOnce = true;
      setupPlayer();
      setupSearch();
      setupKeys();
      }

   // Fetch the logged-in user's roles so we can show/hide the cast button.
   try {
      const sr = await apiCall('getUser', {username: creds.load().user});
      currentUser = sr.user;
   } catch {}
   document.getElementById('player-cast').hidden = !currentUser?.castRole;

   // If a cast session is already active on the server (e.g. after a page
   // reload), restore local state so the icon and progress bar reflect it.
   if (currentUser?.castRole) {
      try {
         const sr = await apiCall('castSession');
         if (sr.castSession?.active) {
            const sess = sr.castSession;
            castDeviceId     = sess.deviceId;
            castDeviceName   = sess.deviceName ?? '';
            castStartOffset  = sess.startOffset;
            castBaseTime     = sess.currentTime;
            castBaseAt       = Date.now();
            castPlayerState  = sess.playerState;
            castSongDuration = sess.songDuration;
            castAudioOnly    = !!sess.audioOnly;
            castReceiverVideo = !!sess.receiverShowsVideo;
            // The same description castLoad's reply carries, for the load
            // that is already playing — this reload has no castLoad reply to
            // have read it from, which is why castSession repeats it.
            castStream       = sess;
            // The server numbers caption tracks from 1 and 0 means off; the
            // picker indexes from 0 and null means off.
            player.captionIndex = sess.trackId > 0 ? sess.trackId - 1 : null;
            const seek = document.getElementById('player-seek');
            if (sess.songDuration > 0) seek.max = sess.songDuration;
            castButtonState(true);
            // Populate player queue so title/thumbnail are visible in the bar.
            try {
               const songSr = await apiCall('getSong', {id: sess.songId});
               if (songSr.song) {
                  player.queue = [songSr.song];
                  player.index = 0;
                  player.albumCtx = {
                     albumId:    songSr.song.parent,
                     albumTitle: songSr.song.album ?? '',
                     artistId:   null,
                     artistName: songSr.song.artist ?? '',
                     };
                  playerUpdateUI();
                  // A film already on the television: draw the panel and
                  // rebuild the picker, so the reload lands on the same
                  // controls it left. captionIndex was set from the session
                  // above, so videoCaptionsMenu marks the right entry.
                  if (songSr.song.isVideo) {
                     player.captionSong = songSr.song.id;
                     videoSurfaceSet('theatre');
                     videoSurfaceCaption(songSr.song);
                     // Start the picture where the receiver already is, not
                     // at zero.  For a Range-capable stream the sync loop
                     // would seek there anyway; for a chunked one it could
                     // not, having no way to reach bytes it never asked for.
                     castApplyLocalVideo(songSr.song,
                                         castStartOffset + castBaseTime);
                     }
                  }
               } catch (_) {}
            startCastEvents();
            }
         } catch (_) {}
      }

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
            await viewAlbums(s.artistId, s.artistName, s.isCategory === true);
         } else if (s.view === 'tracks') {
         if (document.getElementById('pane-tracks').children.length > 0)
            paneNav.slideTo(2);
         else
            await viewTracks(s.albumId, s.albumTitle, s.artistId, s.artistName);
         } else if (s.view === 'playlist-tracks') {
         if (document.getElementById('pane-albums').children.length > 0)
            paneNav.slideTo(1);
         else
            await viewPlaylistTracks(s.playlistId, s.playlistName);
         } else if (s.view === 'playlists') {
         if (document.getElementById('pane-artists').children.length > 0)
            paneNav.slideTo(0);
         else
            await viewPlaylists();
         } else {
         if (document.getElementById('pane-artists').children.length > 0) {
            paneNav.slideTo(0);
            await returnedToArtists();
            }
         else
            await showView('artists');
         }
      });

   // Recalculate pane widths and strip offset on resize without animating.
   new ResizeObserver(() => paneNav.relayout()).observe(
      document.getElementById('pane-viewport'));

   document.getElementById('logout-btn').addEventListener('click', async e => {
      e.preventDefault();
      // Tear down any active cast session before dropping creds — otherwise
      // the Chromecast keeps playing and the SSE listener stays open server-side.
      if (castDeviceId !== null) await stopCast({resumeLocal: false});
      creds.clear();
      showLogin();
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
   const {server, user, salt, token} = creds.load();
   console.log('[boot] saved creds present:', !!(server && user && salt && token));
   if (server && user && salt && token) {
      try {
         await verifySaved();
         showShell();
         return;
      } catch (err) {
         console.warn('[boot] saved credentials failed, clearing', err);
         creds.clear();
         showLogin();
      }
   }
   // A password left by a version of this client that stored one. There is no
   // way to turn it into a token without the user typing it again, so the only
   // safe thing is to remove it and ask.
   if (localStorage.getItem('gd_password')) {
      console.log('[boot] clearing a password stored by an older client');
      creds.clear();
   }
   if (await detectSubsonicOrigin()) {
      console.log('[boot] self-hosted: hiding server field');
      const serverInput = document.getElementById('server');
      serverInput.value = window.location.origin;
      serverInput.hidden = true;
      document.querySelector('label[for="server"]').hidden = true;
   }
})();

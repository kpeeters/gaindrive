// Apply the saved theme before first paint to avoid a flash. Loaded as a
// blocking script in <head> — and as a file rather than inline, so the SPA's
// Content-Security-Policy can say script-src 'self' with no inline carve-out.
(function(){var t=localStorage.getItem('gd_theme');if(t)document.documentElement.classList.add(t);})();

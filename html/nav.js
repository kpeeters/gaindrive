document.addEventListener('click', function (e) {
  var header = document.querySelector('.nav');
  if (!header) return;
  var btn = e.target.closest('.nav-toggle');
  if (btn) {
    var open = header.classList.toggle('open');
    btn.setAttribute('aria-expanded', open ? 'true' : 'false');
    return;
  }
  if (header.classList.contains('open') && (!e.target.closest('.nav-inner') || e.target.closest('.nav-links a'))) {
    header.classList.remove('open');
    var t = header.querySelector('.nav-toggle');
    if (t) t.setAttribute('aria-expanded', 'false');
  }
});
document.addEventListener('keydown', function (e) {
  if (e.key !== 'Escape') return;
  var header = document.querySelector('.nav.open');
  if (!header) return;
  header.classList.remove('open');
  var t = header.querySelector('.nav-toggle');
  if (t) t.setAttribute('aria-expanded', 'false');
});

#!/usr/bin/env python3
"""Render doc/api.toml into html/api.html, a page of the GainDrive website.

    make api-html
    python3 doc/gen_api_html.py                    # same thing
    python3 doc/gen_api_html.py --list             # endpoint names, no output

doc/api.toml is the source of truth for the gaindrive API extensions; this
script only formats it.  The page's <head>, navigation bar and footer are
lifted verbatim from html/docs.html rather than kept as a ninth hand-written
copy, so a restyle of the site reaches this page too.

TRUST BOUNDARY: every string that comes out of api.toml is HTML-escaped before
anything else happens, and there is no escape hatch.  The only unescaped
markup in the output is this file's own literals and the three blocks sliced
out of html/docs.html.
"""

import argparse
import html
import re
import sys
from pathlib import Path

try:
    import tomllib
except ModuleNotFoundError:
    sys.exit("gen_api_html.py needs Python 3.11 or newer (for tomllib).")

ROOT   = Path(__file__).resolve().parent.parent
TOML   = ROOT / "doc" / "api.toml"
CHROME = ROOT / "html" / "docs.html"
OUT    = ROOT / "html" / "api.html"

# Recognised keys.  An unknown key is a typo that would otherwise be dropped in
# silence, taking a parameter or a whole paragraph with it.
META_KEYS     = {"extension", "title", "intro"}
SECTION_KEYS  = {"id", "title", "group", "blurb"}
ENDPOINT_KEYS = {"kind", "name", "applies_to", "section", "path", "method",
                 "version", "auth", "auth_note", "response", "content_type",
                 "summary", "notes", "see", "param", "field", "error"}
PARAM_KEYS    = {"name", "type", "required", "default", "repeatable", "values",
                 "doc", "notes"}
FIELD_KEYS    = {"name", "type", "on", "doc", "notes"}
ERROR_KEYS    = {"code", "http", "when"}

KINDS = {"endpoint", "additions"}

AUTH_LABEL = {
    "none":       "no auth",
    "user":       None,               # the default; no badge worth drawing
    "admin":      "admin only",
    "uploadRole": "uploadRole",
    "castRole":   "castRole",
}
RESP_LABEL = {
    "subsonic":      None,            # the default
    "subsonic-json": "JSON only",
    "plain-json":    "bare JSON",
    "raw":           "raw body",
    "sse":           "event stream",
}

# A paragraph that starts like this in `notes` becomes a callout box.
NOTE_RE = re.compile(r"^NOTE:\s+")
# Lines that want a feature the subset does not have.  Warned about, not fatal.
UNSUPPORTED_RE = re.compile(r"^\s*(#{1,6}\s|\||\d+\.\s|-\s)")


# --------------------------------------------------------------------------
# The markdown subset
# --------------------------------------------------------------------------

def _esc(s):
    return html.escape(str(s), quote=True)


def _href_ok(url):
    """Only these can be typed into a TOML file and reach an href."""
    return (url.startswith(("http://", "https://", "#"))
            or url.endswith(".html"))


def _emphasis(s):
    """Bold, em and links.  Runs on already-escaped, code-free text."""
    s = re.sub(r"\*\*(.+?)\*\*", r"<b>\1</b>", s)
    s = re.sub(r"(?<!\*)\*([^*\s][^*]*?)\*(?!\*)", r"<em>\1</em>", s)

    def link(m):
        text, url = m.group(1), m.group(2)
        if not _href_ok(url):
            print(f"gen_api_html: refusing link target {url!r}", file=sys.stderr)
            return text
        return f'<a href="{url}">{text}</a>'

    # &amp; is already escaped, which is what an href attribute wants.
    return re.sub(r"\[([^\]]+)\]\(([^)\s]+)\)", link, s)


def _inline(s):
    """Escape, then apply inline markup.

    Code spans are lifted out FIRST, so nothing inside one is ever read as
    markup: the prose this renders is full of `personal=*` and `[tmdbid=550]`,
    either of which the emphasis and link rules would mangle.

    They are replaced by placeholders rather than split on, because emphasis
    has to be able to span a code span -- **Seeking is also a `castLoad`** is
    one bold run, and splitting leaves its two markers in different pieces
    where no regex can pair them.  NUL cannot occur here; TOML forbids it in
    both string forms.
    """
    spans = []

    def stash(m):
        spans.append(m.group(1))
        return f"\x00{len(spans) - 1}\x00"

    text = re.sub(r"`([^`]*)`", stash, _esc(s))
    text = _emphasis(text)
    return re.sub(r"\x00(\d+)\x00",
                  lambda m: f"<code>{spans[int(m.group(1))]}</code>", text)


def _md(text, where):
    """The subset: paragraphs, star lists, NOTE callouts, inline markup."""
    blocks = []
    lines = text.strip().split("\n")
    para, items = [], []

    def flush_para():
        if not para:
            return
        joined = " ".join(para)
        if NOTE_RE.match(joined):
            body = _inline(NOTE_RE.sub("", joined))
            blocks.append(f'<div class="note"><p>{body}</p></div>')
        else:
            blocks.append(f"<p>{_inline(joined)}</p>")
        para.clear()

    def flush_items():
        if not items:
            return
        li = "".join(f"<li>{_inline(t)}</li>" for t in items)
        blocks.append(f'<ul class="doc-list">{li}</ul>')
        items.clear()

    for line in lines:
        if UNSUPPORTED_RE.match(line):
            print(f"gen_api_html: {where}: unsupported markdown: {line.strip()!r}",
                  file=sys.stderr)
        if not line.strip():
            flush_para()
            flush_items()
        elif line.lstrip().startswith("* "):
            flush_para()
            items.append(line.lstrip()[2:].strip())
        elif items and line.startswith("  "):
            items[-1] += " " + line.strip()      # continuation of a list item
        else:
            flush_items()
            para.append(line.strip())
    flush_para()
    flush_items()
    return "\n".join(blocks)


# --------------------------------------------------------------------------
# Loading and validation
# --------------------------------------------------------------------------

def _keys(table, allowed, where):
    unknown = set(table) - allowed
    if unknown:
        sys.exit(f"{TOML.name}: {where}: unknown key(s): "
                 + ", ".join(sorted(unknown)))


def _load(path):
    try:
        data = tomllib.loads(path.read_text(encoding="utf-8"))
    except tomllib.TOMLDecodeError as e:
        sys.exit(f"{path}: {e}")

    _keys(data, {"meta", "section", "endpoint"}, "top level")
    meta = data.get("meta", {})
    _keys(meta, META_KEYS, "[meta]")

    sections = data.get("section", [])
    seen_sec = set()
    for s in sections:
        _keys(s, SECTION_KEYS, f"[[section]] {s.get('id', '?')}")
        for k in ("id", "title", "group"):
            if k not in s:
                sys.exit(f"{path.name}: [[section]] missing {k}")
        if s["id"] in seen_sec:
            sys.exit(f"{path.name}: duplicate section id {s['id']}")
        seen_sec.add(s["id"])

    endpoints = data.get("endpoint", [])
    names = set()
    for e in endpoints:
        label = e.get("name") or "/".join(e.get("applies_to", ["?"]))
        _keys(e, ENDPOINT_KEYS, f"[[endpoint]] {label}")

        kind = e.get("kind")
        if kind not in KINDS:
            sys.exit(f"{path.name}: {label}: kind must be one of "
                     + ", ".join(sorted(KINDS)))
        if kind == "endpoint":
            if "name" not in e:
                sys.exit(f"{path.name}: {label}: an endpoint needs a name")
            if "applies_to" in e:
                sys.exit(f"{path.name}: {label}: applies_to belongs to additions")
            if e["name"] in names:
                sys.exit(f"{path.name}: duplicate endpoint {e['name']}")
            names.add(e["name"])
        else:
            if "applies_to" not in e:
                sys.exit(f"{path.name}: {label}: additions need applies_to")
            if "version" in e:
                sys.exit(f"{path.name}: {label}: additions carry no version")

        if e.get("section") not in seen_sec:
            sys.exit(f"{path.name}: {label}: unknown section "
                     f"{e.get('section')!r}")
        if e.get("auth", "user") not in AUTH_LABEL:
            sys.exit(f"{path.name}: {label}: unknown auth {e['auth']!r}")
        if e.get("response", "subsonic") not in RESP_LABEL:
            sys.exit(f"{path.name}: {label}: unknown response {e['response']!r}")

        for p in e.get("param", []):
            _keys(p, PARAM_KEYS, f"{label} param {p.get('name', '?')}")
        for f in e.get("field", []):
            _keys(f, FIELD_KEYS, f"{label} field {f.get('name', '?')}")
        for err in e.get("error", []):
            _keys(err, ERROR_KEYS, f"{label} error")
            if ("code" in err) == ("http" in err):
                sys.exit(f"{path.name}: {label}: an error needs exactly one "
                         "of code or http")

    for e in endpoints:
        for ref in e.get("see", []):
            if ref not in names:
                sys.exit(f"{path.name}: {e.get('name', '?')}: see names "
                         f"unknown endpoint {ref!r}")

    return meta, sections, endpoints


# --------------------------------------------------------------------------
# Site chrome, lifted from html/docs.html
# --------------------------------------------------------------------------

def _slice(text, start, end, path):
    i = text.find(start)
    j = text.find(end, i + 1)
    if i < 0 or j < 0:
        sys.exit(f"{path}: could not find {start!r} ... {end!r}. The API page "
                 "borrows its head, nav and footer from that file; if the site "
                 "markup changed, update gen_api_html.py to match.")
    return text[i:j + len(end)]


def _chrome(path, title):
    text = path.read_text(encoding="utf-8")
    head   = _slice(text, "<head>", "</head>", path)
    header = _slice(text, '<header class="nav">', "</header>", path)
    footer = _slice(text, '<footer class="footer">', "</footer>", path)
    head = re.sub(r"<title>.*?</title>", f"<title>{_esc(title)}</title>",
                  head, count=1, flags=re.S)
    return head, header, footer


# --------------------------------------------------------------------------
# Emitters
# --------------------------------------------------------------------------

def _anchor(ep):
    if ep.get("name"):
        return "ep-" + re.sub(r"[^A-Za-z0-9]+", "-", ep["name"])
    joined = "-".join(ep["applies_to"])
    return "ep-" + re.sub(r"[^A-Za-z0-9]+", "-", joined).strip("-").lower()


def _sidebar(sections):
    groups = []
    for s in sections:
        if not groups or groups[-1][0] != s["group"]:
            groups.append((s["group"], []))
        groups[-1][1].append(s)
    out = ['<aside class="docs-nav">']
    for name, secs in groups:
        out.append('  <div class="dn-grp">')
        out.append(f'    <div class="h">{_esc(name)}</div>')
        for s in secs:
            out.append(f'    <a href="#{_esc(s["id"])}">{_esc(s["title"])}</a>')
        out.append("  </div>")
    out.append("</aside>")
    return "\n".join(out)


def _table(head, rows):
    if not rows:
        return ""
    th = "".join(f"<th>{h}</th>" for h in head)
    body = "\n".join("<tr>" + "".join(f"<td>{c}</td>" for c in r) + "</tr>"
                     for r in rows)
    return (f'<table class="doc-table">\n<thead><tr>{th}</tr></thead>\n'
            f"<tbody>\n{body}\n</tbody>\n</table>")


def _param_rows(params):
    rows = []
    for p in params:
        name = f'<code>{_esc(p["name"])}</code>'
        if p.get("required"):
            name += ' <b class="api-req">*</b>'
        bits = []
        if p.get("default") is not None:
            bits.append(f'default <code>{_esc(p["default"])}</code>')
        if p.get("values"):
            vals = ", ".join(f"<code>{_esc(v)}</code>" for v in p["values"])
            bits.append(f"one of {vals}")
        if p.get("repeatable"):
            bits.append("may repeat")
        doc = _inline(p.get("doc", ""))
        if bits:
            doc += (" " if doc else "") + "; ".join(bits) + "."
        rows.append((name, _esc(p.get("type", "")), doc))
    return rows


def _field_rows(fields):
    rows = []
    for f in fields:
        name = f'<code>{_esc(f["name"])}</code>'
        if f.get("on"):
            name += f' <span class="api-on">on {_esc(f["on"])}</span>'
        rows.append((name, _esc(f.get("type", "")), _inline(f.get("doc", ""))))
    return rows


def _error_rows(errors):
    rows = []
    for e in errors:
        if "code" in e:
            kind, val = "Subsonic", e["code"]
        else:
            kind, val = "HTTP", e["http"]
        rows.append((f"<code>{_esc(val)}</code>", kind, _inline(e.get("when", ""))))
    return rows


def _sub_notes(items, what):
    """A per-parameter or per-field note, below its table."""
    out = []
    for it in items:
        if not it.get("notes"):
            continue
        body = _md(it["notes"], f"{what} {it['name']}")
        out.append(f'<div class="api-note"><p class="api-note-h">'
                   f'<code>{_esc(it["name"])}</code></p>\n{body}</div>')
    return "\n".join(out)


def _endpoint(ep, names):
    out = [f'<div class="api-ep">']
    anchor = _anchor(ep)

    if ep["kind"] == "endpoint":
        heading = _esc(ep["name"])
    else:
        heading = " / ".join(_esc(a) for a in ep["applies_to"])
    out.append(f'<h3 id="{anchor}">{heading}</h3>')

    meta = []
    if ep["kind"] == "endpoint":
        path = ep.get("path") or f"/rest/{ep['name']}.view"
        meta.append(f'<code>{_esc(ep.get("method", "GET"))} {_esc(path)}</code>')
        # Optional, defaulting to 1: everything is version 1 until the
        # first public release, and a badge on every entry saying so is
        # noise.  A future version 2 sets the key on its own endpoints and
        # those alone are badged.
        ver = ep.get("version", "1")
        if ver != "1":
            meta.append(f'<span class="api-badge">v{_esc(ver)}</span>')
    else:
        meta.append('<span class="api-badge api-badge-std">standard endpoint</span>')
    auth = AUTH_LABEL.get(ep.get("auth", "user"))
    if auth:
        meta.append(f'<span class="api-badge">{_esc(auth)}</span>')
    resp = RESP_LABEL.get(ep.get("response", "subsonic"))
    if resp:
        meta.append(f'<span class="api-badge">{_esc(resp)}</span>')
    out.append('<p class="api-meta">' + "\n".join(meta) + "</p>")

    if ep["kind"] == "additions":
        links = []
        for a in ep["applies_to"]:
            if a in names:
                links.append(f'<a href="#ep-{_esc(a)}"><code>{_esc(a)}</code></a>')
            else:
                links.append(f"<code>{_esc(a)}</code>")
        out.append("<p>Carried by " + ", ".join(links)
                   + ". Only the gaindrive additions are listed here.</p>")

    if ep.get("summary"):
        out.append(f'<p>{_inline(ep["summary"])}</p>')
    if ep.get("auth_note"):
        out.append(f'<p>Permission: {_inline(ep["auth_note"])}</p>')
    if ep.get("content_type"):
        out.append(f'<p>Content type: <code>{_esc(ep["content_type"])}</code></p>')

    params = ep.get("param", [])
    if params:
        out.append("<h4>Parameters</h4>")
        out.append(_table(("Parameter", "Type", "Description"),
                          _param_rows(params)))
        out.append(_sub_notes(params, "param"))

    fields = ep.get("field", [])
    if fields:
        out.append("<h4>Response</h4>")
        out.append(_table(("Field", "Type", "Description"), _field_rows(fields)))
        out.append(_sub_notes(fields, "field"))

    errors = ep.get("error", [])
    if errors:
        out.append("<h4>Errors</h4>")
        out.append(_table(("Code", "Kind", "When"), _error_rows(errors)))

    if ep.get("notes"):
        out.append(_md(ep["notes"], ep.get("name", anchor)))

    if ep.get("see"):
        links = ", ".join(f'<a href="#ep-{_esc(s)}"><code>{_esc(s)}</code></a>'
                          for s in ep["see"])
        out.append(f'<p class="api-see">See also {links}.</p>')

    out.append("</div>")
    return "\n".join(x for x in out if x)


def _section(sec, endpoints, names):
    out = [f'<div class="doc-sec" id="{_esc(sec["id"])}">',
           f'<h2>{_esc(sec["title"])}</h2>']
    if sec.get("blurb"):
        out.append(_md(sec["blurb"], f"section {sec['id']}"))
    for ep in endpoints:
        out.append(_endpoint(ep, names))
    out.append("</div>")
    return "\n".join(out)


def _page(meta, sections, endpoints, chrome_path):
    names = {e["name"] for e in endpoints if e.get("name")}
    title = meta.get("title", "API")
    head, header, footer = _chrome(chrome_path, title)

    body = []
    for sec in sections:
        mine = [e for e in endpoints if e.get("section") == sec["id"]]
        body.append(_section(sec, mine, names))

    return f"""<!DOCTYPE html>
<html lang="en">
{head}
<body>

{header}

<section class="docs-head">
  <div class="wrap">
    <span class="eyebrow">API</span>
    <h1>{_esc(title)}</h1>
  </div>
</section>

<section class="docs-body">
  <div class="wrap docs">

{_sidebar(sections)}

    <div class="docs-main api-main">

{_md(meta.get("intro", ""), "[meta].intro")}

{chr(10).join(body)}

    </div>
  </div>
</section>

{footer}

<script src="nav.js"></script>
</body>
</html>
"""


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--toml", type=Path, default=TOML)
    ap.add_argument("--chrome", type=Path, default=CHROME,
                    help="page whose head, nav and footer are reused")
    ap.add_argument("--out", type=Path, default=OUT)
    ap.add_argument("--list", action="store_true",
                    help="print the endpoints named, and write nothing")
    args = ap.parse_args()

    meta, sections, endpoints = _load(args.toml)

    if args.list:
        for e in endpoints:
            for n in ([e["name"]] if e.get("name") else e["applies_to"]):
                print(n)
        return

    args.out.write_text(_page(meta, sections, endpoints, args.chrome),
                        encoding="utf-8")
    print(f"Wrote {args.out} — {len(endpoints)} entries in "
          f"{len(sections)} sections.")


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Download ~15 freely licensed albums to populate a test
Subsonic/Navidrome library.

Sources: Internet Archive items that mirror Free Music Archive,
netlabels (Bad Panda, Dusted Wax Kingdom, No-Source, MixGalaxy,
Clinical Archives...), Musopen, Nine Inch Nails and CC0 classical
recordings. Optionally Jamendo (needs a free API client_id).

The catalogue deliberately mixes:
  * formats     : 24-bit FLAC, 16-bit FLAC (from zip), WAV,
                  MP3 (CBR/VBR), Ogg Vorbis
  * structures  : normal albums, a 36-track 4-part set,
                  Various-Artists compilations, a 1-track release,
                  a badly tagged folder
  * metadata    : non-ASCII tags (French), missing covers

Only the Python 3 standard library is needed.

Usage:
  python3 fetch_test_library.py --dest ./music --dry-run
  python3 fetch_test_library.py --dest ./music
  python3 fetch_test_library.py --dest ./music --lossy
  python3 fetch_test_library.py --dest ./music \
      --jamendo-client-id XXXX --jamendo-albums 3
"""

import argparse
import hashlib
import json
import os
import re
import shutil
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request
import zipfile
from concurrent.futures import ThreadPoolExecutor, as_completed

IA_META = "https://archive.org/metadata/{}"
IA_DL = "https://archive.org/download/{}/{}"
IA_PAGE = "https://archive.org/details/{}"
JAMENDO_API = "https://api.jamendo.com/v3.0/albums/tracks/"
USER_AGENT = "test-library-fetcher/1.0 (personal test server)"

# ---------------------------------------------------------------
# Catalogue. "want" is the preferred format:
#   flac      original FLAC files (fallback: mp3)
#   mp3       best MP3 per track, original preferred
#   ogg       Ogg Vorbis (archive.org derivative)
#   zip-XXX   download the zip whose name contains XXX
#             (e.g. "zip-flac") and extract audio from it
#
# Files are stored as <dest>/<Artist>/<Album>/. "artist" and
# "album" override the archive.org creator/title, which often
# carry catalogue numbers or repeat the artist name. Items
# without overrides fall back to the cleaned-up metadata.
# ---------------------------------------------------------------
VA = "Various Artists"

CATALOGUE = [
    # --- Classical, CC0 / public domain -----------------------
    dict(id="OpenGoldbergVariations", want="flac",
         artist="Kimiko Ishizaka",
         album="The Open Goldberg Variations",
         note="CC0, 24-bit FLAC (large)"),
    dict(id="pandacd-715-js-bach-the-art-of-the-fugue-"
            "kunst-der-fuge-bwv-1080", want="mp3",
         artist="Kimiko Ishizaka",
         album="The Art of the Fugue, BWV 1080",
         note="CC0"),
    dict(id="master-tracks-the-open-well-tempered-clavier",
         want="mp3", artist="Kimiko Ishizaka",
         album="The Open Well-Tempered Clavier",
         note="public domain"),
    dict(id="SymphonyNo.5", want="mp3",
         artist="Ludwig van Beethoven", album="Symphony No. 5",
         note="Musopen, public domain, 1 track"),
    # --- Nine Inch Nails, CC BY-NC-SA ------------------------
    dict(id="nineinchnails_ghosts_I_IV", want="flac",
         artist="Nine Inch Nails", album="Ghosts I-IV",
         note="36 tracks in four parts"),
    dict(id="The_Slip-2640", want="mp3",
         artist="Nine Inch Nails", album="The Slip",
         note="Vocals, proper tags"),
    # --- Netlabels --------------------------------------------
    dict(id="DWK123", want="mp3",
         artist="ProleteR", album="Curses From Past Times",
         note="electro-swing/hip-hop"),
    dict(id="DWK149", want="zip-flac",
         artist="Boogie Belgique", album="Blueberry Hill",
         note="FLAC from zip"),
    dict(id="badpanda018", want="mp3",
         artist="Riding Alone For Thousands Of Miles",
         album="Brick City Love Song", note="post-rock"),
    dict(id="kzz002", want="ogg",
         artist="Echo Lali",
         album="La Valise aux Mille Voyages",
         note="French titles, Ogg Vorbis"),
    dict(id="pcr089EmilDavydov-Sketches", want="zip-wav",
         artist="Emil Davydov", album="Sketches",
         note="WAV (usually untagged)"),
    dict(id="nullbomb", want="mp3",
         artist="Nullbomb", album="Creatures 2005-2006",
         note="Poorly tagged files, edge case"),
    # --- Various-Artists compilations -------------------------
    dict(id="NS050", want="mp3",
         artist=VA, album="Another Day, Another Way",
         note="No-Source netlabel compilation"),
    dict(id="MIXG032", want="mp3",
         artist=VA, album="Retrovision",
         note="MixGalaxy compilation"),
    dict(id="ca200_cjazz", want="mp3",
         artist=VA, album="Clinical Jazz",
         note="Clinical Archives compilation (~540 MB)"),
]

AUDIO_EXT = (".flac", ".mp3", ".ogg", ".wav", ".m4a", ".opus")
IMAGE_EXT = (".jpg", ".jpeg", ".png")

_print_lock = threading.Lock()


def log(*args):
    with _print_lock:
        print(*args, flush=True)


# ---------------------------------------------------------------
# HTTP helpers
# ---------------------------------------------------------------
def http_get(url, retries=4, timeout=60):
    """Return an open response, retrying on transient errors."""
    req = urllib.request.Request(
        url, headers={"User-Agent": USER_AGENT})
    delay = 2
    for attempt in range(retries):
        try:
            return urllib.request.urlopen(req, timeout=timeout)
        except Exception as exc:  # noqa: BLE001
            if attempt == retries - 1:
                raise
            log(f"    retry {attempt + 1} for {url}: {exc}")
            time.sleep(delay)
            delay *= 2
    raise RuntimeError("unreachable")


def http_json(url):
    with http_get(url) as resp:
        return json.load(resp)


def md5_of(path, chunk=1 << 20):
    h = hashlib.md5()
    with open(path, "rb") as fh:
        for block in iter(lambda: fh.read(chunk), b""):
            h.update(block)
    return h.hexdigest()


def download(url, dest, size=None, md5=None):
    """Download url to dest. Skips when an identical file exists.
    Writes to dest.part first so an interrupted run resumes
    cleanly (the partial file is simply re-fetched)."""
    if os.path.exists(dest):
        if size is None or os.path.getsize(dest) == int(size):
            return "skip"
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    part = dest + ".part"
    with http_get(url, timeout=120) as resp, open(part, "wb") as fh:
        shutil.copyfileobj(resp, fh, length=1 << 20)
    if md5 and md5_of(part) != md5:
        os.remove(part)
        raise IOError(f"MD5 mismatch for {url}")
    os.replace(part, dest)
    return "ok"


# ---------------------------------------------------------------
# Naming helpers
# ---------------------------------------------------------------
_BAD = re.compile(r'[<>:"/\\|?*\x00-\x1f]')


def safe(name, maxlen=120):
    name = str(name).replace(": ", " - ")
    name = _BAD.sub("_", name).strip(" .")
    return name[:maxlen].rstrip(" .") or "Unknown"


_CATNO = re.compile(r"^\s*\[[^\]]+\]\s*|\s*[\[(][A-Za-z]*\d+[\])]\s*$")
_VA = re.compile(r"^(various( artists)?|va)$", re.I)


def clean_album(title, artist):
    """'[MIXG032] Various Artists - Retrovision' -> 'Retrovision'"""
    t = _CATNO.sub("", title).strip()
    for prefix in (artist, "Various Artists", "Various"):
        if prefix and t.lower().startswith(prefix.lower()):
            rest = t[len(prefix):].lstrip()
            if rest[:1] in ("-", "–", "—", ":"):
                t = rest[1:].strip()
                break
    return t or title


def clean_artist(creator):
    creator = (creator or "").strip()
    if not creator or _VA.match(creator):
        return VA
    return creator


def album_dir(dest, artist, album):
    return os.path.join(dest, safe(artist), safe(album))


def first(value):
    if isinstance(value, list):
        return value[0] if value else ""
    return value or ""


# ---------------------------------------------------------------
# archive.org file selection
# ---------------------------------------------------------------
_SUFFIX = re.compile(r"(_vbr|_64kb|_128kb|_320kb)$", re.I)


def stem(name):
    base = os.path.splitext(os.path.basename(name))[0]
    return _SUFFIX.sub("", base).lower()


def mp3_score(f):
    """Higher is better. Originals beat derivatives; 64 kbps
    previews are the last resort."""
    fmt = f.get("format", "")
    score = 0
    if f.get("source") == "original":
        score += 100
    if "320" in fmt:
        score += 30
    elif "VBR" in fmt:
        score += 20
    elif "64Kbps" in fmt:
        score -= 50
    else:
        score += 10
    return score


def pick_audio(files, want):
    """Return (list_of_file_dicts, kind) for a metadata file list.
    kind is 'files' or 'zip'."""
    by_ext = {}
    for f in files:
        ext = os.path.splitext(f["name"])[1].lower()
        by_ext.setdefault(ext, []).append(f)

    if want.startswith("zip-"):
        key = want[4:].lower()
        zips = [f for f in by_ext.get(".zip", [])
                if key in f["name"].lower()]
        if zips:
            zips.sort(key=lambda f: -int(f.get("size", 0)))
            return [zips[0]], "zip"
        want = "flac" if key == "flac" else "mp3"

    if want == "flac":
        flacs = [f for f in by_ext.get(".flac", [])
                 if f.get("source") == "original"]
        if flacs:
            return sorted(flacs, key=lambda f: f["name"]), "files"
        want = "mp3"

    if want == "ogg":
        oggs = by_ext.get(".ogg", [])
        if oggs:
            return sorted(oggs, key=lambda f: f["name"]), "files"
        want = "mp3"

    # mp3: best candidate per track stem
    best = {}
    for f in by_ext.get(".mp3", []):
        s = stem(f["name"])
        if s not in best or mp3_score(f) > mp3_score(best[s]):
            best[s] = f
    chosen = list(best.values())
    if chosen:
        return sorted(chosen, key=lambda f: f["name"]), "files"

    # last resort: any original audio at all
    anything = [f for f in files
                if f["name"].lower().endswith(AUDIO_EXT)
                and f.get("source") == "original"]
    return sorted(anything, key=lambda f: f["name"]), "files"


def pick_cover(files):
    """Prefer an original image named like a cover, then the
    largest original JPEG/PNG, then the item tile."""
    imgs = [f for f in files
            if f["name"].lower().endswith(IMAGE_EXT)
            and f.get("source") == "original"
            and "spectrogram" not in f["name"].lower()]
    if not imgs:
        return None
    named = [f for f in imgs
             if re.search(r"cover|front|folder", f["name"], re.I)]
    pool = named or [f for f in imgs
                     if f["name"] != "__ia_thumb.jpg"] or imgs
    return max(pool, key=lambda f: int(f.get("size", 0)))


# ---------------------------------------------------------------
# Album fetchers
# ---------------------------------------------------------------
def extract_audio(zip_path, out_dir):
    n = 0
    with zipfile.ZipFile(zip_path) as zf:
        for info in zf.infolist():
            name = info.filename
            low = name.lower()
            if info.is_dir() or "__macosx" in low:
                continue
            if not low.endswith(AUDIO_EXT + IMAGE_EXT):
                continue
            target = os.path.join(out_dir,
                                  safe(os.path.basename(name)))
            with zf.open(info) as src, open(target, "wb") as dst:
                shutil.copyfileobj(src, dst, length=1 << 20)
            n += 1
    return n


def plan_ia(entry, lossy):
    ident = entry["id"]
    meta = http_json(IA_META.format(urllib.parse.quote(ident)))
    if not meta or "files" not in meta:
        raise LookupError(f"item '{ident}' not found")
    md = meta.get("metadata", {})
    want = entry["want"]
    if lossy and want not in ("mp3", "ogg"):
        want = "mp3"
    audio, kind = pick_audio(meta["files"], want)
    if not audio:
        raise LookupError(f"no usable audio in '{ident}'")
    artist = entry.get("artist") or clean_artist(
        first(md.get("creator")))
    album = entry.get("album") or clean_album(
        first(md.get("title")) or ident, artist)
    return dict(
        source="archive.org",
        ident=ident,
        title=album,
        creator=artist,
        license=first(md.get("licenseurl")) or "(not stated)",
        url=IA_PAGE.format(ident),
        audio=audio,
        kind=kind,
        cover=pick_cover(meta["files"]),
        note=entry.get("note", ""),
    )


def fetch_ia(plan, dest):
    folder = album_dir(dest, plan["creator"], plan["title"])
    os.makedirs(folder, exist_ok=True)
    ident = plan["ident"]

    def url_for(name):
        return IA_DL.format(urllib.parse.quote(ident),
                            urllib.parse.quote(name))

    if plan["kind"] == "zip":
        z = plan["audio"][0]
        marker = os.path.join(folder, ".extracted")
        if not os.path.exists(marker):
            with tempfile.TemporaryDirectory() as tmp:
                zpath = os.path.join(tmp, "album.zip")
                download(url_for(z["name"]), zpath,
                         z.get("size"), z.get("md5"))
                n = extract_audio(zpath, folder)
            open(marker, "w").close()
            log(f"  [{ident}] extracted {n} files from zip")
    else:
        total = len(plan["audio"])
        for i, f in enumerate(plan["audio"], 1):
            out = os.path.join(folder,
                               safe(os.path.basename(f["name"])))
            status = download(url_for(f["name"]), out,
                              f.get("size"), f.get("md5"))
            log(f"  [{ident}] {i}/{total} {status}: "
                f"{os.path.basename(out)}")

    c = plan["cover"]
    if c:
        ext = os.path.splitext(c["name"])[1].lower()
        cover_out = os.path.join(folder, "cover" + ext)
        try:
            download(url_for(c["name"]), cover_out, c.get("size"))
        except Exception as exc:  # noqa: BLE001
            log(f"  [{ident}] cover failed: {exc}")
    return folder


def plan_jamendo(client_id, n_albums, lossy):
    fmt = "mp32" if lossy else "flac"
    params = dict(client_id=client_id, format="json",
                  limit=n_albums, order="popularity_total",
                  audiodlformat=fmt, imagesize=600)
    url = JAMENDO_API + "?" + urllib.parse.urlencode(params)
    data = http_json(url)
    hdr = data.get("headers", {})
    if hdr.get("status") != "success":
        raise RuntimeError(f"Jamendo API error: {hdr}")
    plans = []
    for alb in data.get("results", []):
        tracks = [t for t in alb.get("tracks", [])
                  if t.get("audiodownload_allowed", True)
                  and t.get("audiodownload")]
        if not tracks:
            continue
        lic = sorted({t.get("license_ccurl", "")
                      for t in tracks} - {""})
        plans.append(dict(
            source="jamendo",
            ident=f"jamendo-{alb.get('id')}",
            title=alb.get("name", "Unknown album"),
            creator=alb.get("artist_name", "Unknown artist"),
            license=", ".join(lic) or "(not stated)",
            url=f"https://www.jamendo.com/album/{alb.get('id')}",
            tracks=tracks,
            image=alb.get("image"),
            ext=".mp3" if fmt.startswith("mp3") else ".flac",
            note="Jamendo API",
        ))
    return plans


def fetch_jamendo(plan, dest):
    folder = album_dir(dest, plan["creator"], plan["title"])
    os.makedirs(folder, exist_ok=True)
    for t in plan["tracks"]:
        pos = int(t.get("position") or 0)
        name = safe(f"{pos:02d} - {t.get('name', 'track')}")
        out = os.path.join(folder, name + plan["ext"])
        status = download(t["audiodownload"], out)
        log(f"  [{plan['ident']}] {status}: {os.path.basename(out)}")
    if plan.get("image"):
        try:
            download(plan["image"],
                     os.path.join(folder, "cover.jpg"))
        except Exception as exc:  # noqa: BLE001
            log(f"  [{plan['ident']}] cover failed: {exc}")
    return folder


# ---------------------------------------------------------------
# Main
# ---------------------------------------------------------------
def plan_size(plan):
    if plan["source"] == "archive.org":
        return sum(int(f.get("size", 0)) for f in plan["audio"])
    return 0  # Jamendo does not report sizes up front


def write_licences(dest, done):
    path = os.path.join(dest, "LICENSES.md")
    lines = ["# Licences for this test library", "",
             "All content is redistributable under the licence "
             "listed. NC = non-commercial use only; ND = no "
             "derivatives; BY = credit the artist.", ""]
    for p in sorted(done, key=lambda p: p["creator"].lower()):
        lines += [f"## {p['creator']} - {p['title']}",
                  f"- Licence: {p['license']}",
                  f"- Source: {p['url']}", ""]
    with open(path, "w", encoding="utf-8") as fh:
        fh.write("\n".join(lines))
    return path


def main():
    ap = argparse.ArgumentParser(
        description="Fetch freely licensed albums for a test "
                    "Subsonic server.")
    ap.add_argument("--dest", default="./music",
                    help="library root (default ./music)")
    ap.add_argument("--dry-run", action="store_true",
                    help="list albums, formats and sizes only")
    ap.add_argument("--lossy", action="store_true",
                    help="MP3/Ogg only (roughly half the size; "
                         "--dry-run shows exact figures)")
    ap.add_argument("--only", default="",
                    help="comma-separated archive.org ids to fetch")
    ap.add_argument("--skip", default="",
                    help="comma-separated archive.org ids to skip")
    ap.add_argument("--jobs", type=int, default=3,
                    help="albums downloaded in parallel (default 3)")
    ap.add_argument("--jamendo-client-id", default=os.environ.get(
                    "JAMENDO_CLIENT_ID", ""),
                    help="Jamendo API client_id (free at "
                         "devportal.jamendo.com)")
    ap.add_argument("--jamendo-albums", type=int, default=0,
                    help="number of popular Jamendo albums to add")
    args = ap.parse_args()

    only = {s.strip() for s in args.only.split(",") if s.strip()}
    skip = {s.strip() for s in args.skip.split(",") if s.strip()}
    entries = [e for e in CATALOGUE
               if (not only or e["id"] in only)
               and e["id"] not in skip]

    log(f"Resolving {len(entries)} archive.org items...")
    plans, failed = [], []
    for e in entries:
        try:
            plans.append(plan_ia(e, args.lossy))
        except Exception as exc:  # noqa: BLE001
            failed.append((e["id"], str(exc)))
            log(f"  ! {e['id']}: {exc}")

    if args.jamendo_albums > 0:
        if not args.jamendo_client_id:
            log("  ! --jamendo-albums needs --jamendo-client-id")
        else:
            try:
                plans += plan_jamendo(args.jamendo_client_id,
                                      args.jamendo_albums,
                                      args.lossy)
            except Exception as exc:  # noqa: BLE001
                failed.append(("jamendo", str(exc)))
                log(f"  ! jamendo: {exc}")

    total = 0
    log("")
    for p in plans:
        size = plan_size(p)
        total += size
        if p["source"] == "archive.org":
            exts = sorted({os.path.splitext(f["name"])[1]
                           for f in p["audio"]})
            what = f"{len(p['audio'])} x {'/'.join(exts)}"
        else:
            what = f"{len(p['tracks'])} x {p['ext']}"
        log(f"- {safe(p['creator'])}/{safe(p['title'])}")
        log(f"    {what}, {size / 1e6:,.0f} MB, {p['license']}")
    log(f"\n{len(plans)} albums, ~{total / 1e9:.2f} GB "
        f"(Jamendo sizes not included)")

    if args.dry_run:
        return 0 if plans else 1

    os.makedirs(args.dest, exist_ok=True)
    done = []
    with ThreadPoolExecutor(max_workers=max(1, args.jobs)) as ex:
        futs = {}
        for p in plans:
            fn = fetch_ia if p["source"] == "archive.org" \
                else fetch_jamendo
            futs[ex.submit(fn, p, args.dest)] = p
        for fut in as_completed(futs):
            p = futs[fut]
            try:
                fut.result()
                done.append(p)
                log(f"done: {p['creator']} - {p['title']}")
            except Exception as exc:  # noqa: BLE001
                failed.append((p["ident"], str(exc)))
                log(f"FAILED: {p['ident']}: {exc}")

    lic = write_licences(args.dest, done)
    log(f"\n{len(done)} albums in {args.dest}; licences in {lic}")
    if failed:
        log("Failures:")
        for ident, msg in failed:
            log(f"  {ident}: {msg}")
    return 0 if not failed else 2


if __name__ == "__main__":
    sys.exit(main())

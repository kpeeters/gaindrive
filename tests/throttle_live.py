# /// script
# requires-python = ">=3.9"
# dependencies = []
# ///
"""Brute-force simulation against a live gaindrive server.

Standalone on purpose: no repo imports, stdlib only, so it can be copied to
any machine and run as

    uv run throttle_live.py --url https://example.org/music --user admin
    uv run throttle_live.py --url example.org --port 4040 --user admin

It behaves as an attacker would: no credentials, random password guesses for
one username, several in flight at once, each with a forged X-Forwarded-For
in case the server believes it. Everything is judged from response timing,
since a blocked guess gets the same reply as a wrong one.

This ALWAYS ends in the hard block: the address it runs from is locked out
for ten minutes, and so is every user behind the proxy if the server does not
trust the proxy's forwarded address. --password is optional and only used
once, at the end, to confirm the block by watching a correct login fail.
"""

import argparse
import getpass
import hashlib
import json
import secrets
import statistics
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from concurrent.futures import ThreadPoolExecutor

# Mirrors the constants in src/authz.cc.
FREE_TRIES  = 10
BLOCK_AFTER = 50
BLOCK_MINS  = 10

# Anything under this extra latency is network noise, not the 200 ms step.
DELAY_MARGIN = 0.15


def ping(base, user, *, password=None, token_of=None):
    """Returns (status, error code). The real password, when checking the
    block, goes as a salted token so it never travels in clear."""
    p = {"u": user, "v": "1.16.1", "c": "throttle_live", "f": "json"}
    if token_of is not None:
        salt = secrets.token_hex(8)
        p["t"] = hashlib.md5((token_of + salt).encode()).hexdigest()
        p["s"] = salt
    else:
        p["p"] = password
    # A fresh forged address per guess: if the server took it as the client,
    # every guess would land in an empty bucket and never be slowed.
    fake = f"203.0.113.{secrets.randbelow(254) + 1}"
    req = urllib.request.Request(
        f"{base}/ping.view?{urllib.parse.urlencode(p)}",
        headers={"X-Forwarded-For": fake, "X-Real-IP": fake})
    try:
        with urllib.request.urlopen(req, timeout=30) as r:
            body = r.read()
    except urllib.error.HTTPError as e:
        return f"http {e.code}", None
    except OSError as e:
        return f"error {e}", None
    try:
        sr = json.loads(body)["subsonic-response"]
    except (ValueError, KeyError):
        # A proxy answering with the SPA or an error page lands here.
        return "not a subsonic response", None
    return sr.get("status"), sr.get("error", {}).get("code")


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--url", required=True,
                    help="server root, as the web client is opened at")
    ap.add_argument("--port", type=int,
                    help="overrides any port given in --url")
    ap.add_argument("--user", default="admin",
                    help="username to guess passwords for (default admin)")
    ap.add_argument("--password",
                    help="real password of --user, to confirm the block; "
                         "use --password - to be prompted")
    ap.add_argument("--parallel", type=int, default=8,
                    help="guesses in flight at once (default 8)")
    ap.add_argument("--duration", type=int, default=60,
                    help="seconds to keep guessing (default 60)")
    a = ap.parse_args()

    root = a.url.rstrip("/")
    if "://" not in root:
        root = "http://" + root
    if a.port is not None:
        u = urllib.parse.urlsplit(root)
        host = f"[{u.hostname}]" if ":" in u.hostname else u.hostname
        root = u._replace(netloc=f"{host}:{a.port}").geturl()
    base = root + "/rest"
    pw = getpass.getpass() if a.password == "-" else a.password

    # One guess first, so a wrong URL or a proxy error page stops the run
    # here rather than after a minute of meaningless timings.
    st, code = ping(base, a.user, password="guess-" + secrets.token_hex(6))
    if code != 40:
        print(f"Probe guess got status={st}, code={code}; expected a "
              f"Subsonic 'wrong password' (40). Check --url and --port.")
        return 1

    print(f"Brute-forcing '{a.user}' at {base} for {a.duration} s, "
          f"{a.parallel} in parallel.")
    print(f"*** This address will be locked out for {BLOCK_MINS} minutes "
          f"afterwards. ***\nStarting in 5 seconds, Ctrl+C to abort.")
    time.sleep(5)

    # (sent, latency, status, code), appended as replies arrive.
    log = []
    lock = threading.Lock()
    t0 = time.monotonic()
    wall0 = time.time()
    stop = t0 + a.duration

    def worker():
        while time.monotonic() < stop:
            sent = time.monotonic()
            st, code = ping(base, a.user,
                            password="guess-" + secrets.token_hex(6))
            with lock:
                log.append((sent - t0, time.monotonic() - sent, st, code))

    with ThreadPoolExecutor(a.parallel) as ex:
        for _ in range(a.parallel):
            ex.submit(worker)
        last = 0
        while time.monotonic() < stop:
            time.sleep(1)
            with lock:
                new = log[last:]
                last = len(log)
            lat = [x[1] for x in new]
            med = f"{statistics.median(lat) * 1000:6.0f} ms" if lat else "     -"
            print(f"  t={time.monotonic() - t0:4.0f}s  {len(new):4d} replies"
                  f"  median {med}", flush=True)

    log.sort()
    failed = 0

    def check(ok, msg):
        nonlocal failed
        print(("PASS  " if ok else "FAIL  ") + msg)
        if not ok:
            failed += 1

    print()
    statuses = {}
    for x in log:
        k = x[2] if x[3] is None else f"{x[2]} code {x[3]}"
        statuses[k] = statuses.get(k, 0) + 1
    print(f"{len(log)} guesses answered: "
          + ", ".join(f"{n} {k}" for k, n in statuses.items()))
    check(not any(x[2] == "ok" for x in log), "no guess was accepted")
    check(all(x[3] == 40 for x in log),
          "every reply is an ordinary 'wrong password' (code 40)")

    lat = [x[1] for x in log]
    baseline = statistics.median(lat[:min(FREE_TRIES, len(lat))])
    slow = [i for i, t in enumerate(lat) if t > baseline + DELAY_MARGIN]
    print(f"Baseline reply time {baseline * 1000:.0f} ms, "
          f"{len(slow)} replies slowed down.")
    check(bool(slow), "guesses get slowed down (forged X-Forwarded-For ignored)")

    # The block shows as fast replies again after the slow ones; everything
    # sent before it was actually checked against the password.
    lifts = time.time() + BLOCK_MINS * 60
    if slow:
        tail = lat[slow[-1] + 1:]
        blocked = bool(tail) and len(tail) >= 2 * a.parallel and \
            statistics.median(tail) < baseline + DELAY_MARGIN
        check(blocked, f"guessing ends in a block ({len(tail)} instant "
              f"refusals after the last slowed guess)")
        if blocked:
            checked = slow[-1] + 1
            at = log[checked][0]
            per_hour = checked / (at + BLOCK_MINS * 60) * 3600
            lifts = wall0 + at + BLOCK_MINS * 60
            print(f"Block after {checked} guesses and {at:.0f} s: at most "
                  f"about {per_hour:.0f} guesses/hour from one address.")

    if pw:
        st, code = ping(base, a.user, token_of=pw)
        check(st == "failed" and code == 40,
              f"the real password is refused while blocked "
              f"(status={st}, code={code})")
    until = time.strftime("%H:%M", time.localtime(lifts))
    print(f"\nBlock lifts around {until}. The server log should show "
          f"'auth failed for user {a.user}'\nwith this machine's address, "
          f"not the proxy's or 203.0.113.x.")
    print(f"\n{failed} check(s) failed" if failed else "\nAll checks passed")
    return failed


if __name__ == "__main__":
    sys.exit(main())

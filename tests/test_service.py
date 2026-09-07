#!/usr/bin/env python3
"""--install-service generation, without root, systemd or a running server.

Joins test_video_names.py and test_chapters_parse.py as a test that needs no
server: it drives --service-dry-run, which runs every precondition it has the
privilege for, writes nothing and runs no systemctl.

Edit BINARY if your build is elsewhere.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

BINARY = os.environ.get("GAINDRIVE", "./build/gaindrive")

failures = []


def check(ok, what):
    print(("  ok   " if ok else "  FAIL ") + what)
    if not ok:
        failures.append(what)


def run(args, cwd=None):
    return subprocess.run([BINARY] + args, capture_output=True, text=True, cwd=cwd)


def split_documents(stdout):
    """The dry run prints '# would write <path>  (<mode>)' before each document."""
    parts = re.split(r"^# would write (\S+).*$", stdout, flags=re.M)
    # parts = [preamble, path1, body1, path2, body2, ...]
    return {parts[i]: parts[i + 1] for i in range(1, len(parts) - 1, 2)}


def main():
    if not os.path.exists(BINARY):
        sys.exit(f"{BINARY} not found; build first, or set GAINDRIVE=<path>")

    tmp = tempfile.mkdtemp(prefix="gd-service-")
    try:
        music = os.path.join(tmp, "music")
        os.mkdir(music)
        db = os.path.join(tmp, "gd.db")
        cfg = os.path.join(tmp, "gaindrive.conf")
        me = subprocess.run(["id", "-un"], capture_output=True,
                            text=True).stdout.strip()

        # The database is a precondition, not an artefact of this test: the
        # installer refuses until the first account exists, which is the check
        # that catches a skipped first run.
        r = run(["--db", db, "--artist-root", f"t={music}",
                 "--add-user", "admin", "--password", "secret"])
        check(r.returncode == 0, "setup: --add-user created the database")

        base = ["--config", cfg, "--db", db, "--artist-root", f"t={music}",
                "--host", "0.0.0.0", "--install-service", "--service-dry-run",
                "--service-user", me]

        print("\n--- generation")
        r = run(base)
        check(r.returncode == 0, "dry run succeeds")
        docs = split_documents(r.stdout)
        check(len(docs) == 2, "two documents printed (config and unit)")
        if len(docs) != 2:
            return
        conf_text = docs[cfg]
        unit_text = [v for k, v in docs.items() if k != cfg][0]

        print("\n--- the generated config")
        try:
            c = json.loads(conf_text)
        except Exception as e:
            check(False, f"config is valid JSON ({e})")
            return
        check(True, "config is valid JSON")
        check(c["host"] == "0.0.0.0", "host is what the command line said")
        check(c["db_path"] == db, "db_path is the database that was set up")
        check(c["roots"][0]["path"] == music, "root path is absolute")
        check(c["roots"][0]["type"] == "artists", "root type survives")
        # nullopt means "use the built-in yt-dlp table"; an explicit [] means
        # "no fetching at all", so writing [] here would silently disable it.
        check("url_handlers" not in c,
              "url_handlers is absent when no table was configured")
        # Neither is a config key, and inventing one here would make it one.
        check("debug" not in c and "no_scan" not in c,
              "--debug and --no-scan are not written as config keys")
        check(list(c)[0] == "//generated", "provenance comment comes first")

        print("\n--- the generated unit")
        # Not a bare "%" test: SystemCallFilter=@system-service and the
        # placeholders must not be confused for one another.
        check(re.search(r"%[A-Za-z_]+%", unit_text) is None,
              "no placeholder was left unfilled")
        check(f"\nUser={me}\n" in unit_text, "User= is the account asked for")
        check("ConditionPathExists=" + cfg in unit_text,
              "ConditionPathExists names the config")
        check(f"RequiresMountsFor=" in unit_text and music in unit_text,
              "RequiresMountsFor names the library root")
        check("StateDirectory" not in unit_text,
              "no StateDirectory: the database is not under /var/lib")
        check(re.search(r"^ExecStart=\S+ --config " + re.escape(cfg) + "$",
                        unit_text, re.M) is not None,
              "ExecStart passes the non-default config path")
        check("cmake/embed_text.cmake" not in unit_text,
              "the template's own documentation is not in the output")

        if shutil.which("systemd-analyze"):
            up = os.path.join(tmp, "gaindrive.service")
            with open(up, "w") as f:
                f.write(re.sub(r"^ExecStart=.*$", "ExecStart=/bin/true",
                               unit_text, flags=re.M))
            v = subprocess.run(["systemd-analyze", "verify", up],
                               capture_output=True, text=True)
            check("gaindrive.service:" not in v.stderr,
                  "systemd-analyze verify accepts the unit")

        print("\n--- config never beats the command line")
        # The regression test for the precedence bug: host, db_path and
        # upload_dir were assigned from the config with no args.count() guard,
        # so --install-service would have written the old value straight back.
        with open(cfg, "w") as f:
            json.dump({"host": "127.0.0.1", "upload_dir": "/tmp/stale"}, f)
        r = run(base + ["--upload-dir", "/tmp/fresh"])
        docs = split_documents(r.stdout)
        c = json.loads(docs[cfg])
        check(c["host"] == "0.0.0.0", "--host beats an existing config's host")
        check(c["upload_dir"] == "/tmp/fresh",
              "--upload-dir beats an existing config's upload_dir")
        os.remove(cfg)

        print("\n--- refusals")
        r = run(["--config", cfg, "--db", os.path.join(tmp, "absent.db"),
                 "--artist-root", f"t={music}", "--install-service",
                 "--service-dry-run", "--service-user", me])
        check(r.returncode == 1 and "no gaindrive database" in r.stderr,
              "a missing database is refused, naming the first run")

        r = run(["--config", cfg, "--db", db, "--install-service",
                 "--service-dry-run", "--service-user", me])
        check(r.returncode == 1 and "no library root" in r.stderr,
              "installing with nothing to serve is refused")

        r = run(base + ["--add-user", "bob"])
        check(r.returncode == 1 and "cannot be combined" in r.stderr,
              "--add-user with --install-service is refused")

        r = run(base + ["--service-user", "root"])
        check(r.returncode == 1 and "runs as root" in r.stderr,
              "a service running as root is refused")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)

    print()
    if failures:
        print(f"{len(failures)} failure(s):")
        for f in failures:
            print("  " + f)
        sys.exit(1)
    print("all checks passed")


main()

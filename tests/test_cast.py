#!/usr/bin/env python3
"""Query listCastDevices and print any Chromecast devices found on the LAN.

Start the server first:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_cast.py
"""

import sys
import urllib.parse
import urllib.request
import xml.etree.ElementTree as ET

BASE   = "http://localhost:4040/rest"
USER   = "admin"
PASS   = "secret"
VER    = "1.16.1"
CLIENT = "test"


def get(endpoint, **params):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT}
    p.update(params)
    url = f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url, timeout=10) as r:
        return ET.fromstring(r.read())


print("Scanning for Chromecast devices (2 s)…")
root = get("listCastDevices")

ns = "http://subsonic.org/restapi"
status = root.get("status")
if status != "ok":
    err = root.find(f"{{{ns}}}error")
    print(f"Error: {err.get('message') if err is not None else 'unknown'}")
    sys.exit(1)

devices = root.find(f"{{{ns}}}castDevices")
if devices is None or len(devices) == 0:
    print("No Chromecast devices found.")
    sys.exit(0)

print(f"Found {len(devices)} device(s):\n")
for d in devices:
    print(f"  id      : {d.get('id')}")
    print(f"  name    : {d.get('name')}")
    print(f"  model   : {d.get('model')}")
    print(f"  address : {d.get('address')}:{d.get('port')}")
    # false here is what makes castLoad send a film's soundtrack instead of
    # the film. It comes from bit 0 of the device's `ca` record, so this is
    # the check that the record was read off the device the way we think.
    print(f"  videoOut: {d.get('videoOut')}")
    print()

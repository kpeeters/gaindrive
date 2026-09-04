#!/usr/bin/env python3
"""Query listCastDevices and print any Chromecast devices found on the LAN.

Given a device and a song as well, start a session and check that castLoad and
castSession describe it identically — they must, since castSession exists only
so a reloaded page can recover what castLoad already said, and a disagreement
would show as a client whose info panel changed on refresh.

Start the server first:
    ./build/gaindrive --db /tmp/gd_test.db --music-root /music

Then run:
    python3 tests/test_cast.py
    python3 tests/test_cast.py <deviceId> <songId>

Note the second form really casts: it leaves the song playing on the device.
"""

import json
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

if len(sys.argv) < 3:
    print("To check what a LOAD reports, pass a device id and a song id:")
    print(f"    python3 {sys.argv[0]} <deviceId> <songId>")
    sys.exit(0)

DEVICE, SONG = sys.argv[1], sys.argv[2]
CONTROLLER = "test-cast-py"


def get_json(endpoint, **params):
    p = {"u": USER, "p": PASS, "v": VER, "c": CLIENT, "f": "json"}
    p.update(params)
    url = f"{BASE}/{endpoint}.view?{urllib.parse.urlencode(p)}"
    with urllib.request.urlopen(url, timeout=30) as r:
        return json.loads(r.read())["subsonic-response"]


# castLoad's reply and castSession's snapshot describe the same load and are
# required to agree — the second exists only because a reloaded page has no
# first to have read. Nothing else checks that, and a client drawing one thing
# before a reload and another after would be reporting the reload rather than
# the stream. So compare them here rather than by eye in a browser.
FIELDS = ("audioOnly", "contentType", "sentSuffix", "sentBitRate", "tier")

get_json("startCast", id=DEVICE, castController=CONTROLLER)
# Both read as JSON. castSession is JSON-only whatever f= says, and comparing
# it against castLoad's XML would compare "true" with True.
load = get_json("castLoad", id=SONG, castController=CONTROLLER)["castLoad"]
sess = get_json("castSession", castController=CONTROLLER)["castSession"]

print(f"castLoad for song {SONG}:")
for f in FIELDS:
    print(f"  {f:12}: {load.get(f)!r}")

bad = [f for f in FIELDS if sess.get(f) != load.get(f)]
if bad:
    print(f"\ncastSession DISAGREES on: {', '.join(bad)}")
    for f in bad:
        print(f"  {f}: castLoad {load.get(f)!r} vs castSession {sess.get(f)!r}")
    sys.exit(1)
print("\ncastSession agrees with castLoad on all five fields.")

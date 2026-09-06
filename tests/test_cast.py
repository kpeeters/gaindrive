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
    # What a person decided about the device, which overrides videoOut.
    print(f"  videoPref: {d.get('videoPref')}")
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


# The per-device preference is server-side precisely so every client agrees
# about a device, which means the only way to see it is to write one and read
# it back. Restored afterwards, since this is a real device somebody has
# probably configured deliberately.
found  = [d for d in devices if d.get("id") == DEVICE]
if not found:
    print(f"Device {DEVICE} is not in the list.")
    sys.exit(1)
before = found[0].get("videoPref")
if before is None:
    print("This server does not report videoPref; skipping that check.\n")

for want in ("send", "sound", "auto") if before is not None else ():
    get_json("setCastDevicePref", deviceId=DEVICE, videoPref=want)
    got = None
    for d in get_json("listCastDevices")["castDevices"]:
        if d["id"] == DEVICE:
            got = d["videoPref"]
    if got != want:
        print(f"videoPref did not round-trip: set {want!r}, read back {got!r}")
        sys.exit(1)
if before is not None:
    print("videoPref round-trips through setCastDevicePref for all three "
          "values.")

    # An unrecognised value must be refused rather than stored, or it would
    # read back as neither and behave as auto with nothing saying so.
    bad = get_json("setCastDevicePref", deviceId=DEVICE, videoPref="maybe")
    if bad.get("status") != "failed":
        print("setCastDevicePref accepted an unknown videoPref.")
        sys.exit(1)

    get_json("setCastDevicePref", deviceId=DEVICE, videoPref=before)
    print(f"videoPref restored to {before!r}.\n")

# castLoad's reply and castSession's snapshot describe the same load and are
# required to agree — the second exists only because a reloaded page has no
# first to have read. Nothing else checks that, and a client drawing one thing
# before a reload and another after would be reporting the reload rather than
# the stream. So compare them here rather than by eye in a browser.
FIELDS = ("audioOnly", "receiverShowsVideo", "contentType", "sentSuffix",
          "sentBitRate", "tier")

get_json("startCast", id=DEVICE, castController=CONTROLLER)
# Both read as JSON. castSession is JSON-only whatever f= says, and comparing
# it against castLoad's XML would compare "true" with True.
load = get_json("castLoad", id=SONG, castController=CONTROLLER)["castLoad"]
sess = get_json("castSession", castController=CONTROLLER)["castSession"]

# `tier` and `sentSuffix` are the two worth reading by eye for a video: a
# soundtrack reporting remux was copied out of the container, one reporting
# encode was decoded and re-encoded, and the second takes several times as
# long to appear.
print(f"castLoad for song {SONG}:")
for f in FIELDS:
    print(f"  {f:12}: {load.get(f)!r}")

bad = [f for f in FIELDS if sess.get(f) != load.get(f)]
if bad:
    print(f"\ncastSession DISAGREES on: {', '.join(bad)}")
    for f in bad:
        print(f"  {f}: castLoad {load.get(f)!r} vs castSession {sess.get(f)!r}")
    sys.exit(1)
print("\ncastSession agrees with castLoad on every field.")

"""Where the API tests find the server.

Every test that talks to a running server imports BASE from here, so one
option points any of them elsewhere:

    python3 tests/test_browse.py --url http://192.168.1.10:4040

The URL is the server's root, the address the web client is opened at; a
proxy prefix such as https://example.org/music works too. The default is
http://localhost:4040. The option is taken out of sys.argv before a test
reads its own arguments, so it may go anywhere on the command line.
"""

import http.client
import sys
import urllib.parse


def _take_url(default):
    args = sys.argv
    for i in range(1, len(args)):
        if args[i] == "--url" and i + 1 < len(args):
            url = args[i + 1]
            del args[i:i + 2]
            return url
        if args[i].startswith("--url="):
            url = args[i][len("--url="):]
            del args[i]
            return url
    return default


ROOT = _take_url("http://localhost:4040").rstrip("/")
if "://" not in ROOT:
    ROOT = "http://" + ROOT
BASE = ROOT + "/rest"

# What a raw request line has to start with: empty unless the server sits
# under a proxy prefix, which urllib callers get from BASE for free.
PREFIX = urllib.parse.urlsplit(ROOT).path


def connection(timeout):
    """An http.client connection to the server, for the tests that have to
    write the request line or the body themselves."""
    u = urllib.parse.urlsplit(ROOT)
    cls = (http.client.HTTPSConnection if u.scheme == "https"
           else http.client.HTTPConnection)
    return cls(u.hostname, u.port, timeout=timeout)

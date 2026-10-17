#!/usr/bin/env python3
"""A cover with a hostile EXIF block must not take the server down.

Every cover the server scales passes through exif_orientation() in
src/imagescale.cc, including images fetched from TheAudioDB, the Cover Art
Archive and TMDB with nobody logged in. The IFD offset it reads is the file's
own, and an offset near 4 GiB used to wrap the 32-bit bounds check, so the
parser read far past the buffer and the process died of SIGSEGV. A malformed
tag must simply mean "no orientation".

Like test_untrusted.py, this needs **no running server** and no network:

    python3 tests/test_image_scale.py [path/to/gaindrive]
"""

import os
import struct
import subprocess
import sys
import tempfile

BINARY = sys.argv[1] if len(sys.argv) > 1 else "./build/gaindrive"

# An 8x8 grey baseline JPEG, written by stb_image_write.
BASE_JPEG = bytes.fromhex(
    "ffd8ffe000104a46494600010100000100010000ffdb008400100b0c0e0c0a100e0d0e12"
    "11101318281a181616183123251d283a333d3c3933383740485c4e404457453738506d51"
    "575f626768673e4d71797064785c656763011112121815182f1a1a2f6342384263636363"
    "636363636363636363636363636363636363636363636363636363636363636363636363"
    "63636363636363636363ffc00011080008000803012200021101031101ffc401a2000001"
    "0501010101010100000000000000000102030405060708090a0b10000201030302040305"
    "0504040000017d01020300041105122131410613516107227114328191a1082342b1c115"
    "52d1f02433627282090a161718191a25262728292a3435363738393a434445464748494a"
    "535455565758595a636465666768696a737475767778797a838485868788898a92939495"
    "969798999aa2a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5"
    "d6d7d8d9dae1e2e3e4e5e6e7e8e9eaf1f2f3f4f5f6f7f8f9fa0100030101010101010101"
    "010000000000000102030405060708090a0b110002010204040304070504040001027700"
    "0102031104052131061241510761711322328108144291a1b1c109233352f0156272d10a"
    "162434e125f11718191a262728292a35363738393a434445464748494a53545556575859"
    "5a636465666768696a737475767778797a82838485868788898a92939495969798999aa2"
    "a3a4a5a6a7a8a9aab2b3b4b5b6b7b8b9bac2c3c4c5c6c7c8c9cad2d3d4d5d6d7d8d9dae2"
    "e3e4e5e6e7e8e9eaf2f3f4f5f6f7f8f9faffda000c03010002110311003f0028a28a00ff"
    "d9")

# IFD offsets to try: an ordinary one, and the two whose "+ 2" used to wrap.
OFFSETS = [8, 0xFFFFFFFE, 0xFFFFFFFF]


def with_exif(ifd_offset):
    """BASE_JPEG with an Exif APP1 segment right after SOI."""
    tiff = b"II" + struct.pack("<HI", 42, ifd_offset) + bytes(8)
    body = b"Exif\x00\x00" + tiff
    app1 = b"\xff\xe1" + struct.pack(">H", len(body) + 2) + body
    return BASE_JPEG[:2] + app1 + BASE_JPEG[2:]


def run():
    binary = os.path.abspath(BINARY)
    failed = 0
    # --image-scale-test writes a 640 px copy into the working directory.
    with tempfile.TemporaryDirectory() as tmp:
        for off in OFFSETS:
            path = os.path.join(tmp, f"exif-{off:08x}.jpg")
            with open(path, "wb") as f:
                f.write(with_exif(off))
            p = subprocess.run([binary, "--image-scale-test", path], cwd=tmp,
                               capture_output=True, text=True)
            if p.returncode != 0:
                print(f"FAIL  IFD offset 0x{off:08x}: exited {p.returncode}")
                failed += 1
            elif "size=64: 8x8" not in p.stdout:
                print(f"FAIL  IFD offset 0x{off:08x}: not scaled\n{p.stdout}")
                failed += 1
            else:
                print(f"PASS  IFD offset 0x{off:08x}")

    print(f"\n{len(OFFSETS) - failed}/{len(OFFSETS)} cases passed")
    return failed


if __name__ == "__main__":
    sys.exit(min(run(), 1))

#!/usr/bin/env python3
"""Map the carrier's pixel/vertex shader bytecode hashes (debug_ps_census
"ps:" lines: FNV-1a 32 over the GetFunction() bytecode) back to the game's
shader containers.

Finds every D3D9 shader blob in the given files by its version token
(vs/ps 1.x-3.0), walks the token stream to the end token (0x0000FFFF) to get
the exact length, hashes it the same way, and prints

    hash  size  profile  file@offset  constants...

(constant names from the CTAB via shader_ctab.py). With hashes given via -H,
only those are printed.

Usage: shader_hash.py <file>... [-H hash[,hash...]]
"""

import os
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import shader_ctab  # noqa: E402

VERSION_TOKENS = {
    0xFFFE0101: "vs_1_1", 0xFFFE0200: "vs_2_0", 0xFFFE0300: "vs_3_0",
    0xFFFF0101: "ps_1_1", 0xFFFF0104: "ps_1_4", 0xFFFF0200: "ps_2_0",
    0xFFFF0300: "ps_3_0",
}
END_TOKEN = 0x0000FFFF


def fnv1a(data):
    h = 2166136261
    for b in data:
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    return h


def blob_length(buf, start):
    """Bytes from the version token to the end token inclusive, or None."""
    pos = start + 4
    n = len(buf)
    while pos + 4 <= n:
        tok = struct.unpack_from("<I", buf, pos)[0]
        if tok == END_TOKEN:
            return pos + 4 - start
        if (tok & 0xFFFF) == 0xFFFE:  # comment: length in bits 16..30
            pos += 4 + 4 * ((tok >> 16) & 0x7FFF)
            continue
        length = (tok >> 24) & 0x0F  # SM2+: instruction length in DWORDs
        pos += 4 + 4 * length
    return None


def scan(path):
    buf = open(path, "rb").read()
    ctabs = {off: (creator, consts) for off, _p, creator, consts in shader_ctab.scan(path)}
    out = []
    for off in range(0, len(buf) - 8, 4):
        tok = struct.unpack_from("<I", buf, off)[0]
        prof = VERSION_TOKENS.get(tok)
        if not prof:
            continue
        nxt = struct.unpack_from("<I", buf, off + 4)[0]
        if (nxt & 0xFFFF) != 0xFFFE:  # real shaders open with a comment (CTAB)
            continue
        size = blob_length(buf, off)
        if not size or size > 1 << 20:
            continue
        names = []
        # The CTAB is one of the leading comments (some shaders open with DBUG).
        pos = off + 4
        while pos + 4 <= len(buf):
            tok = struct.unpack_from("<I", buf, pos)[0]
            if (tok & 0xFFFF) != 0xFFFE:
                break
            ctab = ctabs.get(pos + 4)
            if ctab:
                names = [c["name"] for c in ctab[1]]
                break
            pos += 4 + 4 * ((tok >> 16) & 0x7FFF)
        out.append((fnv1a(buf[off:off + size]), size, prof, off, names))
    return out


def main(argv):
    want = None
    files = []
    i = 0
    while i < len(argv):
        if argv[i] == "-H" and i + 1 < len(argv):
            want = {int(h, 16) for h in argv[i + 1].split(",")}
            i += 2
            continue
        files.append(argv[i])
        i += 1
    if not files:
        print(__doc__)
        return 2
    seen = set()
    for path in files:
        for h, size, prof, off, names in scan(path):
            if want is not None and h not in want:
                continue
            key = (h, os.path.basename(path), off)
            if key in seen:
                continue
            seen.add(key)
            print("%08x %6u %-7s %s@0x%x %s" % (h, size, prof, os.path.basename(path), off,
                                                 " ".join(n for n in names if n)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

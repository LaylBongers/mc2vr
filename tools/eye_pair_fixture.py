#!/usr/bin/env python3
"""Synthetic eye-dump fixture generator (regression test for the
parallax analysis in analyze_dumps.py).

Generates mc2vr_eye_left/right_frame<N>.bmp pairs by mirroring the carrier's
exact dump pipeline (src/carrier/eye_replay.cpp): fp16 (half) values ->
half decode -> Reinhard + 1/2.2 gamma tonemap -> 24-bit BMP. The right image
is the left scene sampled at (x - shift), i.e. its content sits `shift` px
to the RIGHT of the left image's.

Usage: eye_pair_fixture.py [outdir] [width] [height] [shift] [frame]
Then: analyze_dumps.py /nonexistent.log <outdir>  (or pass the real log)
Expected: 'best shift +<shift>px, SAD 0.00 vs shift0 ... | PARALLAX PRESENT'
(the numpy path recovers it exactly; the no-numpy fallback is within 4px).
"""

import math
import os
import struct
import sys

TM_LEVELS = 8192  # keep in sync with eye_replay.cpp
TM_MAX = 16.0


def f16(x):
    """Quantize through IEEE half (the RT format)."""
    return struct.unpack("<e", struct.pack("<e", x))[0]


def tonemap(v):
    """Mirror of eye_replay.cpp tonemap(): Reinhard + 1/2.2 gamma, LUT over [0,16)."""
    if not (v > 0):
        return 0
    idx = min(TM_LEVELS - 1, int(v * ((TM_LEVELS - 1) / TM_MAX)))
    x = TM_MAX * idx / (TM_LEVELS - 1)
    m = x / (1.0 + x)
    return int(255.0 * math.pow(m, 1.0 / 2.2) + 0.5)


def scene(x, y, w, h):
    """Textured 'world' with feature-rich vertical bars (near objects)."""
    v = 0.02 + 0.5 * (x / w) + 0.3 * math.sin(x * 0.013 + y * 0.005) + 0.05 * (y / h)
    if x % 37 < 4:
        v += 0.9
    return max(v, 0.0)


def write_bmp(path, w, h, gray):
    row = (w * 3 + 3) // 4 * 4
    data = bytearray(row * h)
    for y in range(h):
        off = y * row
        for x in range(w):
            c = gray[y * w + x]
            i = off + x * 3
            data[i] = c  # B
            data[i + 1] = c  # G
            data[i + 2] = c  # R
    hdr = bytearray(54)
    hdr[0:2] = b"BM"
    struct.pack_into("<I", hdr, 2, 54 + row * h)
    struct.pack_into("<I", hdr, 10, 54)
    struct.pack_into("<i", hdr, 18, w)
    struct.pack_into("<i", hdr, 22, h)
    struct.pack_into("<H", hdr, 26,  1)
    struct.pack_into("<H", hdr, 28, 24)
    struct.pack_into("<I", hdr, 34, row * h)
    with open(path, "wb") as f:
        f.write(bytes(hdr) + bytes(data))


def main():
    outdir = sys.argv[1] if len(sys.argv) > 1 else "."
    w = int(sys.argv[2]) if len(sys.argv) > 2 else 320
    h = int(sys.argv[3]) if len(sys.argv) > 3 else 180
    shift = int(sys.argv[4]) if len(sys.argv) > 4 else 13
    frame = int(sys.argv[5]) if len(sys.argv) > 5 else 812
    os.makedirs(outdir, exist_ok=True)

    for name, sh in (("left", 0), ("right", shift)):
        gray = [tonemap(f16(scene(x - sh, y, w, h))) for y in range(h) for x in range(w)]
        path = os.path.join(outdir, f"mc2vr_eye_{name}_frame{frame}.bmp")
        write_bmp(path, w, h, gray)
        print(f"wrote {path}")
    print(f"expected: best shift {shift:+d}px, PARALLAX PRESENT")


if __name__ == "__main__":
    main()

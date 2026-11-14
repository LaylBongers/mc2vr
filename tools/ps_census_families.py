#!/usr/bin/env python3
"""Rank the pixel shaders a game run drew by family (docs/shaders.md).

Input: ps_census.csv, written next to the carrier by debug_ps_census=all (run
totals per shader: hash, GPU ms, Mpix and draws per sampled frame, both eye
passes). Run it with shadow_resolution=1024: at other sizes the 9-tap
receivers' bytecode is texel-scaled at creation, so their hashes no longer
match the container's.

Each hash is mapped to its original shader:
  - the game's: by bytecode FNV-1a over shader3.bin (tools/shader_hash.py),
    named from docs/reverse_engineering/data/shader3_names.csv;
  - ours: by the generated headers (build/win32/generated/shaders/*.h),
    marked "reimpl".
pix% is the share of all main-size pixels (overdraw included; full-screen
passes like tone mapping count too); recv% the share among the shadow
receivers. Families are the .sho file names without the light-variant suffixes (_pl, _sl,
_pl_sl, each optionally _li). For each family: its share of the run's pixels,
GPU time, draws, the variants seen, and whether it samples the sun shadow map
(globalDirLightShadowMap) and with how many taps.

Usage: ps_census_families.py <ps_census.csv> <shader3.bin> [--all]
  default: only shadow-receiving families; --all: every family.
"""

import csv
import glob
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from shader_iface import fnv1a, load_generated, words_at  # noqa: E402

NAMES = os.path.join(ROOT, "docs", "reverse_engineering", "data", "shader3_names.csv")
GENERATED = os.path.join(ROOT, "build", "win32", "generated", "shaders")
SUFFIX = re.compile(r"(_pl_sl|_pl|_sl)?(_li)?$", re.IGNORECASE)
TEX_OPS = {66}  # texld (incl. texldp via the control bits)


def shadow_taps(words, ctab_has_shadow):
    """Texture instructions sampling s15 when the CTAB names the shadow map."""
    if not ctab_has_shadow:
        return 0
    taps, i = 0, 1
    while i < len(words):
        t = words[i]
        op = t & 0xFFFF
        if t == 0x0000FFFF:
            break
        if op == 0xFFFE:
            i += 1 + ((t >> 16) & 0x7FFF)
            continue
        n = (t >> 24) & 0xF
        if op in TEX_OPS and n >= 3 and (words[i + 3] & 0x7FF) == 15:
            taps += 1
        i += 1 + n
    return taps


def family_of(file_base):
    return SUFFIX.sub("", file_base)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    show_all = "--all" in sys.argv
    if len(args) != 2:
        print(__doc__)
        return 2
    census, container = args
    buf = open(container, "rb").read()
    n = struct.unpack_from("<I", buf, 0)[0]
    names = {int(r["offset"], 16): r["file"].rsplit("_3.sho", 1)[0] for r in csv.DictReader(open(NAMES))
             if r["file"]}

    # hash -> (name, family, taps, reimpl)
    known = {}
    for k in range(n):
        _key, off, size, _typ = struct.unpack_from("<4I", buf, 4 + 16 * k)
        words = words_at(buf, off)
        h = fnv1a(struct.pack("<%dI" % len(words), *words))
        name = names.get(off, "?@0x%x" % off)
        known[h] = (name, family_of(name), shadow_taps(words, b"globalDirLightShadowMap" in buf[off:off + size]),
                    False)
    for path in glob.glob(os.path.join(GENERATED, "*.h")):
        sym = os.path.basename(path)[:-2]
        words = load_generated(path)
        h = fnv1a(struct.pack("<%dI" % len(words), *words))
        known[h] = (sym, family_of(sym), shadow_taps(words, True), True)

    rows = list(csv.DictReader(open(census)))
    total_pix = sum(float(r["mpix_per_frame"]) for r in rows if r["category"] == "main") or 1.0
    fams = {}
    unknown = []
    for r in rows:
        h = int(r["hash"], 16)
        if h not in known:
            unknown.append(r)
            continue
        name, fam, taps, reimpl = known[h]
        f = fams.setdefault(fam, {"pix": 0.0, "gpu": 0.0, "draws": 0.0, "variants": set(), "taps": set(),
                                  "reimpl": set()})
        if r["category"] == "main":
            f["pix"] += float(r["mpix_per_frame"])
        f["gpu"] += float(r["gpu_ms_per_frame"])
        f["draws"] += float(r["draws_per_frame"])
        f["variants"].add(name)
        f["taps"].add(taps)
        f["reimpl"].add(reimpl)

    order = sorted(fams.items(), key=lambda kv: -kv[1]["pix"])
    recv_pix = sum(f["pix"] for _, f in order if max(f["taps"]) > 0) or 1.0
    print("%-34s %6s %6s %7s %7s %5s  %-9s %s" % ("family", "pix%", "recv%", "GPU ms", "draws", "taps", "status",
                                                 "variants seen"))
    shown_pix = 0.0
    for fam, f in order:
        taps = max(f["taps"])
        if not show_all and taps == 0:
            continue
        shown_pix += f["pix"]
        status = "reimpl" if all(f["reimpl"]) else ("mixed" if any(f["reimpl"]) else "original")
        recv = "%5.1f%%" % (100 * f["pix"] / recv_pix) if taps > 0 else "     -"
        print("%-34s %5.1f%% %s %7.3f %7.1f %5s  %-9s %s" % (fam, 100 * f["pix"] / total_pix, recv, f["gpu"], f["draws"],
                                                       "/".join(str(t) for t in sorted(f["taps"])), status,
                                                       " ".join(sorted(f["variants"]))))
    print("-- shown: %.1f%% of main-pass pixels; %d hashes not found (e.g. post/2D shaders created at runtime)"
          % (100 * shown_pix / total_pix, len(unknown)))
    return 0


if __name__ == "__main__":
    sys.exit(main())

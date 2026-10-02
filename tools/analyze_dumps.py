#!/usr/bin/env python3
# Parse mc2vr_carrier.log M3 ViewDump blocks and analyze ViewEntry field usage.
# Usage: analyze_dumps.py [path/to/mc2vr_carrier.log]
#
# Produces:
#   - per-dump header summary (idx, type, entry, obj, t3, first/refresh)
#   - load-vs-steady diff for any idx dumped twice
#   - nonzero-dword field map per dump (with float interpretation)
#   - varying-across-dumps region analysis (constant vs per-view fields)
#
# Written during M3 field-map derivation; reuse for S1 dump analysis
# (docs/stereo_design.md). Entry size and dump line format must stay in sync
# with src/carrier/render_dump.cpp (dump_bytes: 32 bytes/line, "M3 entry
# +0xOFF: hex", "M3 obj +0xOFF: hex").

import re
import struct
import sys

VIEW_STRIDE = 0x810
OBJ_LEN = 0x40


def parse(path):
    dumps, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(r"M3 ViewDump( \(refresh\))? idx=(\d+) type=(\d+) entry=(\w+) "
                      r"obj=(\w+) t3=(\w+) \((\w[\w-]*)\)", line)
        if m:
            cur = {"refresh": bool(m.group(1)), "idx": int(m.group(2)),
                   "type": int(m.group(3)), "obj": int(m.group(5), 16),
                   "t3": int(m.group(6), 16), "state": m.group(7),
                   "data": bytearray(VIEW_STRIDE), "objdata": bytearray(OBJ_LEN)}
            dumps.append(cur)
            continue
        m = re.search(r"M3 entry \+0x([0-9a-f]+): ([0-9a-f]+)", line)
        if m and cur:
            off = int(m.group(1), 16)
            cur["data"][off:off + 32] = bytes.fromhex(m.group(2))
            continue
        m = re.search(r"M3 obj \+0x([0-9a-f]+): ([0-9a-f]+)", line)
        if m and cur:
            off = int(m.group(1), 16)
            cur["objdata"][off:off + 32] = bytes.fromhex(m.group(2))
    return dumps


def words(d, key="data"):
    return [struct.unpack_from("<I", d[key], o)[0] for o in range(0, len(d[key]), 4)]


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else (
        "/home/laylb/Games/mercenaries-2/drive_c/Program Files (x86)/EA Games/"
        "Mercenaries 2 World in Flames/mc2vr/mc2vr_carrier.log")
    dumps = parse(path)
    print(f"parsed {len(dumps)} dumps: "
          f"{[(d['idx'], d['state']) for d in dumps]}")
    if not dumps:
        return

    by_idx = {}
    for d in dumps:
        by_idx.setdefault(d["idx"], []).append(d)

    # Load vs steady diff for twice-dumped indices
    for idx, ds in by_idx.items():
        if len(ds) >= 2:
            a, b = ds[0], ds[-1]
            print(f"\n=== idx {idx}: {a['state']} -> {b['state']} diff ===")
            wa, wb = words(a), words(b)
            shown = 0
            for i in range(VIEW_STRIDE // 4):
                if wa[i] != wb[i]:
                    off = i * 4
                    fa = struct.unpack_from("<f", a["data"], off)[0]
                    fb = struct.unpack_from("<f", b["data"], off)[0]
                    print(f"  +0x{off:03x}: {wa[i]:#010x} -> {wb[i]:#010x} "
                          f"(f {fa:.6g} -> {fb:.6g})")
                    shown += 1
                    if shown >= 40:
                        print("  ... (truncated)")
                        break
            if shown == 0:
                print("  (identical)")

    # Per-dump nonzero map
    for d in dumps:
        w = words(d)
        nz = [(i * 4, w[i]) for i in range(VIEW_STRIDE // 4) if w[i] != 0]
        print(f"\n=== idx {d['idx']} ({d['state']}, t3={d['t3']}, obj {d['obj']:#x}): "
              f"{len(nz)} nonzero dwords ===")
        for off, v in nz:
            f = struct.unpack_from("<f", d["data"], off)[0]
            fl = f" (f={f:.6g})" if -1e9 < f < 1e9 and f == f else ""
            print(f"  +0x{off:03x}: {v:#010x}{fl}")


if __name__ == "__main__":
    main()

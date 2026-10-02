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
#
# S1 support (src/carrier/s1_probe.cpp log tokens):
#   - "S1 elem @docConsPos +0xOFF: hex" / "@naiveCons +0x00: pairs" ring
#     element dumps -> layout check (+0x1c/+0x24/+0x2c = 30/810/680 + live ptrs)
#   - "S1 xform: ... tag=T m=hex" -> 4x4 float decode per SetTransform detail
#   - key evidence lines echoed (S1 RESIDENCY / S1 patch / S1 vlist / S1 bracket
#     bursts / frame-ctx validation / 10s S1 window reports)

import re
import struct
import sys

VIEW_STRIDE = 0x810
OBJ_LEN = 0x40
ELEM_SIZE = 96


def parse(path):
    dumps, cur = [], None
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.search(
            r"M3 ViewDump( \(refresh\))? idx=(\d+) type=(\d+) entry=(\w+) "
            r"obj=(\w+) t3=(\w+) \((\w[\w-]*)\)",
            line,
        )
        if m:
            cur = {
                "refresh": bool(m.group(1)),
                "idx": int(m.group(2)),
                "type": int(m.group(3)),
                "obj": int(m.group(5), 16),
                "t3": int(m.group(6), 16),
                "state": m.group(7),
                "data": bytearray(VIEW_STRIDE),
                "objdata": bytearray(OBJ_LEN),
            }
            dumps.append(cur)
            continue
        m = re.search(r"M3 entry \+0x([0-9a-f]+): ([0-9a-f]+)", line)
        if m and cur:
            off = int(m.group(1), 16)
            cur["data"][off : off + 32] = bytes.fromhex(m.group(2))
            continue
        m = re.search(r"M3 obj \+0x([0-9a-f]+): ([0-9a-f]+)", line)
        if m and cur:
            off = int(m.group(1), 16)
            cur["objdata"][off : off + 32] = bytes.fromhex(m.group(2))
    return dumps


def words(d, key="data"):
    return [struct.unpack_from("<I", d[key], o)[0] for o in range(0, len(d[key]), 4)]


# ---- S1 analysis -----------------------------------------------------------

S1_ECHO = re.compile(
    r"S1 (RESIDENCY|patch:|vlist:|bracket: frame|xform: calls|vp: calls|"
    r"vlist: frames|elem: ca=|: frame-ctx|: RenderFrame entry|: WARNING|"
    r"elem @docConsPos \(3)"
)
S1_ELEM_LINE = re.compile(r"S1 elem @docConsPos \+0x([0-9a-f]+): ([0-9a-f]+)")
S1_ELEM_NAIVE = re.compile(
    r"S1 elem @(naiveCons|naiveProd|docConsPos) \+0x[0-9a-f]+: "
    r"((?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} "
    r"(?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} "
    r"(?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8})$"
)
S1_ELEM_HDR = re.compile(r"S1 elem: ca=.*")
S1_XFORM = re.compile(r"S1 xform: frame=(\d+) state=(\w+) tag=(\S+) m=([0-9a-f]{128})")


def parse_s1(path):
    """Collect S1 evidence from the log."""
    elems = {}  # tag -> {off: bytes}
    naive = {}  # tag -> bytes(32)
    xforms = []
    echoes = []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = S1_ELEM_LINE.search(line)
        if m:
            elems.setdefault("docConsPos", {})[int(m.group(1), 16)] = bytes.fromhex(
                m.group(2)
            )
            continue
        m = S1_ELEM_NAIVE.search(line)
        if m:
            naive[m.group(1)] = bytes.fromhex(m.group(2).replace(" ", ""))
            continue
        if S1_ELEM_HDR.search(line):
            echoes.append(line)
            continue
        m = S1_XFORM.search(line)
        if m:
            xforms.append(
                (int(m.group(1)), m.group(2), m.group(3), bytes.fromhex(m.group(4)))
            )
            continue
        m = S1_ECHO.search(line)
        if m:
            echoes.append(line)
    return elems, naive, xforms, echoes


def decode_elem(tag, data, off_base=0):
    """Verify the expected 96-byte element layout and print it."""
    buf = bytearray(96)
    n = 0
    for off, b in data.items():
        rel = off - off_base
        if 0 <= rel < 96:
            buf[rel : rel + len(b)] = b
            n += len(b)
    if n < 96:
        return False  # element lines not captured at this offset — skip
    dwords = [struct.unpack_from("<I", buf, o)[0] for o in range(0, 96, 4)]
    ok = dwords[7] == 0x30 and dwords[9] == 0x810 and dwords[11] == 0x680
    print(
        f"\n=== S1 element @ {tag}: layout "
        f"{'VERIFIED (30/810/680 pairs)' if ok else 'NOT as expected'} ==="
    )
    print(f"  +0x00 hdr: {dwords[0]:#010x} {dwords[1]:#010x}")
    print(f"  +0x0c cursors: {' '.join(f'{dwords[i]:#x}' for i in range(3, 7))}")
    print(f"  +0x1c pair0 {{30, ptr {dwords[8]:#010x}}}  (camera staging slot)")
    print(f"  +0x24 pair1 {{810, ptr {dwords[10]:#010x}}} (ViewEntry*)")
    print(f"  +0x2c pair2 {{680, ptr {dwords[12]:#010x}}} (frame-ctx block)")
    print(f"  +0x34 tail: {' '.join(f'{dwords[i]:#x}' for i in range(13, 24))}")
    return ok


def report_s1(path):
    elems, naive, xforms, echoes = parse_s1(path)
    print("\n################ S1 evidence ################")
    for line in echoes[:60]:
        print("  " + line)
    if len(echoes) > 60:
        print(f"  ... ({len(echoes) - 60} more echoed lines)")
    if elems:
        for off_base in (0, 96, 192):
            decode_elem(
                f"docConsPos elem@+0x{off_base:x}", elems["docConsPos"], off_base
            )
    for tag, b in naive.items():
        dwords = [struct.unpack_from("<I", b, o)[0] for o in range(0, 32, 4)]
        ok = (
            len(b) >= 52
            and dwords[7] == 0x30
            and dwords[9] == 0x810
            and dwords[11] == 0x680
        )
        print(
            f"\n=== S1 element @ {tag} (first 32B): "
            f"{'LOOKS VALID' if ok else 'not an element header'} ==="
        )
        print("  " + " ".join(f"{v:08x}" for v in dwords))
    for frame, state, tag, raw in xforms:
        floats = struct.unpack("<16f", raw)
        print(f"\n=== S1 xform frame={frame} state={state} tag={tag} ===")
        for r in range(4):
            print(
                "  [" + " ".join(f"{floats[r * 4 + c]:12.4g}" for c in range(4)) + "]"
            )


# ---- M3 analysis ------------------------------------------------------------


def main():
    path = (
        sys.argv[1]
        if len(sys.argv) > 1
        else (
            "/home/laylb/Games/mercenaries-2/drive_c/Program Files (x86)/EA Games/"
            "Mercenaries 2 World in Flames/mc2vr/mc2vr_carrier.log"
        )
    )
    report_s1(path)
    dumps = parse(path)
    print(f"parsed {len(dumps)} dumps: {[(d['idx'], d['state']) for d in dumps]}")
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
                    print(
                        f"  +0x{off:03x}: {wa[i]:#010x} -> {wb[i]:#010x} "
                        f"(f {fa:.6g} -> {fb:.6g})"
                    )
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
        print(
            f"\n=== idx {d['idx']} ({d['state']}, t3={d['t3']}, obj {d['obj']:#x}): "
            f"{len(nz)} nonzero dwords ==="
        )
        for off, v in nz:
            f = struct.unpack_from("<f", d["data"], off)[0]
            fl = f" (f={f:.6g})" if -1e9 < f < 1e9 and f == f else ""
            print(f"  +0x{off:03x}: {v:#010x}{fl}")


if __name__ == "__main__":
    main()

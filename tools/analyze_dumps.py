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


# ---- S1/S1b analysis ---------------------------------------------------------

S1_ECHO = re.compile(
    r"S1 (RESIDENCY|patch|raw:|vsmat:|vs:|vsreg:|vlist:|bracket: frame|"
    r"bracket: frames|xform: calls|xform\(SetTransform|vp: calls|"
    r"vlist: frames|elem: |elemscan|vsmatch:|vsclock:|vsclean:|vspatched|crec: dumping|"
    r": frame-ctx|"
    r": RenderFrame entry|: WARNING)"
)
# S1b: "S1 elem @consPos+<elem> +0x<off>: hex" (3 x 32B lines per element).
S1_ELEM_CONSPOS = re.compile(r"S1 elem @consPos\+(\d+) \+0x([0-9a-f]+): ([0-9a-f]+)")
# S1b: VS-constant matrix classification detail.
S1_VSMAT = re.compile(r"S1 vsmat: frame=(\d+) reg=c(\d+) tag=(\S+) m=([0-9a-f]{128})")
# S1 (first run) formats, kept so old logs still parse.
S1_ELEM_LINE = re.compile(r"S1 elem @docConsPos \+0x([0-9a-f]+): ([0-9a-f]+)")
S1_ELEM_NAIVE = re.compile(
    r"S1 elem @(naiveCons|naiveProd) \+0x[0-9a-f]+: "
    r"((?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} "
    r"(?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8} "
    r"(?:[0-9a-f]{2}){8} (?:[0-9a-f]{2}){8})$"
)
S1_XFORM = re.compile(r"S1 xform: frame=(\d+) state=(\w+) tag=(\S+) m=([0-9a-f]{128})")


def _walk_dword_pairs(buf):
    """Yield (offset, size, ptr) for {u32 size, ptr} descriptor pairs found in a
    96-byte queue element (S0 layout: pairs start at +0x1c; S1 runtime capture
    showed other producers use the same {size, ptr} scheme with other sizes)."""
    out = []
    for i in range(1, 22):
        size = struct.unpack_from("<I", buf, i * 4)[0]
        ptr = struct.unpack_from("<I", buf, (i + 1) * 4)[0]
        if 0x4 <= size <= 0x400000 and 0x00100000 <= ptr < 0x01a48000:
            out.append((i * 4, size, ptr))
    return out


def decode_elem96(tag, buf):
    """Decode and verify one 96-byte ring element; returns True if it is a
    SubmitWorldPackets world-view element ({0x30,0x810,0x680} pairs)."""
    if len(buf) < 96:
        print(f"\n=== S1 element @ {tag}: incomplete capture ({len(buf)}B) ===")
        return False
    dwords = [struct.unpack_from("<I", buf, o)[0] for o in range(0, 96, 4)]
    is_world = dwords[7] == 0x30 and dwords[9] == 0x810 and dwords[11] == 0x680
    print(f"\n=== S1 element @ {tag}: "
          f"{'WORLD-VIEW ELEMENT VERIFIED (30/810/680)' if is_world else 'other producer'} ===")
    print(f"  +0x00 hdr: {dwords[0]:#010x} {dwords[1]:#010x}")
    print(f"  +0x08 cursors: {' '.join(f'{dwords[i]:#x}' for i in range(2, 7))}")
    pairs = _walk_dword_pairs(buf)
    if pairs:
        for off, size, ptr in pairs:
            label = {0x30: "camera staging", 0x810: "ViewEntry*", 0x680: "frame-ctx"}.get(size)
            print(f"  +0x{off:02x} pair {{{size:#x}, {ptr:#010x}}}"
                  f"{f'  ({label})' if label else ''}")
    else:
        print(f"  +0x1c..: {' '.join(f'{dwords[i]:#x}' for i in range(7, 24))}")
    return is_world


def print_mat16(frame, where, tag, raw):
    floats = struct.unpack("<16f", raw)
    print(f"\n=== {where} frame={frame} {tag} ===")
    for r in range(4):
        print("  [" + " ".join(f"{floats[r * 4 + c]:12.4g}" for c in range(4)) + "]")


def parse_s1(path):
    """Collect S1/S1b evidence from the log."""
    elems = {}   # elem index -> {off: bytes}   (S1b @consPos)
    old_elems = {}  # off -> bytes              (S1 @docConsPos)
    naive = {}   # tag -> bytes(32)             (S1 @naiveCons/naiveProd)
    xforms, vsmats, echoes = [], [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.rstrip("\n")
        m = S1_ELEM_CONSPOS.search(line)
        if m:
            elems.setdefault(int(m.group(1)), {})[int(m.group(2), 16)] = \
                bytes.fromhex(m.group(3))
            continue
        m = S1_ELEM_LINE.search(line)
        if m:
            old_elems.setdefault("docConsPos", {})[int(m.group(1), 16)] = \
                bytes.fromhex(m.group(2))
            continue
        m = S1_ELEM_NAIVE.search(line)
        if m:
            naive[m.group(1)] = bytes.fromhex(m.group(2).replace(" ", ""))
            continue
        m = S1_VSMAT.search(line)
        if m:
            vsmats.append((int(m.group(1)), int(m.group(2)), m.group(3),
                           bytes.fromhex(m.group(4))))
            continue
        m = S1_XFORM.search(line)
        if m:
            xforms.append((int(m.group(1)), m.group(2), m.group(3),
                           bytes.fromhex(m.group(4))))
            continue
        if S1_ECHO.search(line):
            echoes.append(line)
    return elems, old_elems, naive, xforms, vsmats, echoes


# S1c: exfil blocks (GPU matrices vs walked-view entry matrices) and the
# scan-based world-element dumps.
S1_EXENTRY = re.compile(r"S1 exentry: i(\d+) m(\d)=([0-9a-f]{128})")
S1_EXMAT = re.compile(r"S1 exmat: reg=c(\d+) m=([0-9a-f]{128})")
S1_EXSUB = re.compile(r"S1 exsub(\d) \+0x([0-9a-f]{3,}): ([0-9a-f]+)")
S1_EXCAM = re.compile(r"S1 excam i(\d+) \+0x([0-9a-f]{3,}): ([0-9a-f]+)")
S1_EXGLOB = re.compile(r"S1 exglob \+0x([0-9a-f]{3,}): ([0-9a-f]+)")
S1_SCAN_ELEM_HDR = re.compile(r"S1 elem @scan(\d+): world element \(ViewEntry=(\w+)\)")
S1_SCAN_ELEM = re.compile(r"S1 elem @scan \+0x([0-9a-f]+): ([0-9a-f]+)")


def parse_exfils(path):
    """Per exfil frame: {idx: {0: m0bytes, 1: m1bytes}} + [(reg, matrix)]."""
    exfils, cur_entries, cur_mats, cur_subs = [], {}, [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = S1_EXENTRY.search(line)
        if m:
            cur_entries.setdefault(int(m.group(1)), {})[int(m.group(2))] = \
                bytes.fromhex(m.group(3))
            continue
        m = S1_EXMAT.search(line)
        if m:
            cur_mats.append((int(m.group(1)), bytes.fromhex(m.group(2))))
            continue
        m = S1_EXSUB.search(line)
        if m:
            s, off, b = int(m.group(1)), int(m.group(2), 16), bytes.fromhex(m.group(3))
            cur_subs.setdefault(s, bytearray())[off : off + len(b)] = b
            continue
        m = S1_EXCAM.search(line)
        if m:
            # camData objects: keyed as pseudo-subs 10+view for the search
            key = 10 + int(m.group(1))
            off, b = int(m.group(2), 16), bytes.fromhex(m.group(3))
            cur_subs.setdefault(key, bytearray())[off : off + len(b)] = b
            continue
        m = S1_EXGLOB.search(line)
        if m:
            off, b = int(m.group(1), 16), bytes.fromhex(m.group(2))
            cur_subs.setdefault(9, bytearray())[off : off + len(b)] = b
            continue
        if "S1 exfil: frame=" in line:
            if cur_entries or cur_mats or cur_subs:
                exfils.append((cur_entries, cur_mats, cur_subs))
            cur_entries, cur_mats, cur_subs = {}, [], {}
    if cur_entries or cur_mats or cur_subs:
        exfils.append((cur_entries, cur_mats, cur_subs))
    return exfils


def _mat_dist(a, b):
    """Mean absolute elementwise distance between two 4x4 float matrices."""
    fa = struct.unpack("<16f", a)
    fb = struct.unpack("<16f", b)
    return sum(abs(x - y) for x, y in zip(fa, fb)) / 16.0


def _transpose(b):
    f = struct.unpack("<16f", b)
    return struct.pack("<16f", *[f[r * 4 + c] for c in range(4) for r in range(4)])


def report_exfil(path):
    """Offline relationship search: for each exfiled GPU matrix, the closest
    walked-view entry matrix (m0/m1, plus transposed) by mean |diff|. A small
    distance on a non-exact match reveals the derivation (e.g. projection
    product, conjugation, or a rebuilt view matrix)."""
    exfils = parse_exfils(path)
    if not exfils:
        return
    print(f"\n################ exfil relationship search ({len(exfils)} frames) ############")
    for entries, mats, subs in exfils:
        if not mats and not subs:
            continue
        print(f"\n--- exfil frame: {len(entries)} views, {len(mats)} GPU matrices, "
              f"{len(subs)} sub dumps ---")
        # Candidate set: every snapshotted matrix (all nine for full views)
        # plus transposes, and sliding 16-float windows of the subobject
        # copies (the VM consumer receives them inside the 0x680 element —
        # a derived main-camera source would match a sub window).
        def _nonzero(b):
            return b is not None and any(b)

        candidates = []
        for idx, d in sorted(entries.items()):
            for k, mat in sorted(d.items()):
                if not _nonzero(mat):
                    continue  # all-zero template matrices match everything
                candidates.append((f"i{idx}.m{k}", mat))
                candidates.append((f"i{idx}.m{k}T", _transpose(mat)))
        def _subname(s):
            if s == 9:
                return "globcam"
            if s >= 10:
                return f"camData(i{s - 10})"
            return f"sub{s}"

        for s, buf in sorted(subs.items()):
            for off in range(0, max(1, len(buf) - 63), 4):
                win = bytes(buf[off : off + 64])
                if len(win) < 64 or not _nonzero(win):
                    continue
                candidates.append((f"{_subname(s)}+0x{off:03x}", win))
        # Cross-search: which sub windows equal an entry matrix?
        for s, buf in sorted(subs.items()):
            for idx, d in sorted(entries.items()):
                for k, mat in d.items():
                    if not _nonzero(mat):
                        continue
                    for off in range(0, max(1, len(buf) - 63), 4):
                        win = bytes(buf[off : off + 64])
                        if len(win) == 64 and win == mat:
                            print(f"  {s}+0x{off:03x} == i{idx}.m{k} EXACT")
        for reg, mat in mats:
            best = (1e30, None)
            for name, cand in candidates:
                dist = _mat_dist(mat, cand)
                if dist < best[0]:
                    best = (dist, name)
            f = struct.unpack("<16f", mat)
            tag = " ==" if best[0] == 0 else ""
            print(f"  c{reg}: best={best[1]} dist={best[0]:.5g}{tag} "
                  f"t=[{', '.join(f'{v:.3g}' for v in f[12:15])}]")


def report_scan_elems(path):
    """Decode the scan-based world-element dumps (S1c: full-ring scan)."""
    cur, idx, found = None, None, 0
    for line in open(path, encoding="utf-8", errors="replace"):
        m = S1_SCAN_ELEM_HDR.search(line)
        if m:
            cur, idx = bytearray(96), int(m.group(1))
            continue
        m = S1_SCAN_ELEM.search(line)
        if m and cur is not None:
            off = int(m.group(1), 16)
            b = bytes.fromhex(m.group(2))
            if off < 96:
                cur[off : off + len(b)] = b
            if off + len(b) >= 96:
                if decode_elem96(f"scan{idx}", cur):
                    found += 1
                cur = None
    if found:
        print(f"\n  scan-based world elements verified: {found}")


# S1d: camera-ring record dumps (window C layout analysis).
S1_CREC = re.compile(r"S1 crec rec(\d+) \+0x([0-9a-f]+): ([0-9a-f]+)")


def report_crecs(path):
    """Decode the camera-ring record dumps: print dwords and flag any value in
    the view-table range (ViewEntry pointers) — identifies the real layout of
    the {pos, serial, rot16, ViewEntry*, lodByte} records."""
    recs = {}
    for line in open(path, encoding="utf-8", errors="replace"):
        m = S1_CREC.search(line)
        if m:
            r, off, b = int(m.group(1)), int(m.group(2), 16), bytes.fromhex(m.group(3))
            recs.setdefault(r, bytearray(40))[off : off + len(b)] = b
    if not recs:
        return
    print(f"\n#### camera-ring records (window C layout analysis, {len(recs)} dumped) ####")
    VIEW_TABLE = 0x012865e0
    VIEW_STRIDE = 0x810
    for r in sorted(recs):
        dwords = [struct.unpack_from("<I", recs[r], o)[0] for o in range(0, 40, 4)]
        parts = []
        for i, v in enumerate(dwords):
            note = ""
            if VIEW_TABLE <= v < VIEW_TABLE + 512 * VIEW_STRIDE:
                note = f" <-ViewEntry i{(v - VIEW_TABLE) // VIEW_STRIDE}"
            parts.append(f"+0x{i*4:02x}:{v:08x}{note}")
        print(f"  rec{r}: " + "  ".join(parts))


# S1f: low-register VS cache dumps per exfil frame — offline dynamic-register
# analysis: registers whose content changes between exfil frames are the
# per-frame camera candidates; static ones are global constants.
S1_EXCACHE = re.compile(r"S1 excache: c(\d+)=([0-9a-f]+)")


def report_excache(path):
    frames = []  # list of {reg: bytes16}, one per exfil frame
    cur = None
    for line in open(path, encoding="utf-8", errors="replace"):
        if "S1 exfil: frame=" in line:
            cur = {}
            frames.append(cur)
            continue
        m = S1_EXCACHE.search(line)
        if m and cur is not None:
            cur[int(m.group(1))] = bytes.fromhex(m.group(2))
    frames = [f for f in frames if f]
    if len(frames) < 2:
        return
    print(f"\n#### VS low-register dynamics across {len(frames)} exfil frames ####")
    regs = sorted({r for f in frames for r in f})
    for r in regs:
        vals = [f.get(r) for f in frames]
        n = sum(1 for v in vals if v is not None)
        uniq = len(set(v for v in vals if v is not None))
        if uniq == 0:
            continue
        t = struct.unpack("<4f", next(v for v in vals if v is not None))
        status = ("STATIC" if uniq == 1 else f"dynamic x{uniq}")
        print(f"  c{r}: {status} (seen in {n}/{len(frames)} frames) "
              f"first=[{', '.join(f'{v:.4g}' for v in t)}]")


# S1h: burst correlation — consecutive frames of the GPU camera registers
# vs every live view's position. Identifies which view (if any) tracks the
# GPU camera; also checks the r23-r25 translation vs -r21.
S1_BURST = re.compile(
    r"S1 burst: frame=(\d+)((?: r(\d+)=([0-9a-f,]+))+) \| pos:((?: i(\d+):\(([^)]+)\))+)")


def report_burst(path):
    frames = []
    for line in open(path, encoding="utf-8", errors="replace"):
        m = S1_BURST.search(line)
        if not m:
            continue
        regs = {}
        for rm in re.finditer(r"r(\d+)=([0-9a-f,]+)", m.group(2)):
            regs[int(rm.group(1))] = [
                struct.unpack("<f", struct.pack("<I", int(h, 16)))[0]
                for h in rm.group(2).split(",")]
        views = {}
        for vm in re.finditer(r"i(\d+):\(([^)]+)\)", m.group(5)):
            views[int(vm.group(1))] = tuple(float(x) for x in vm.group(2).split(","))
        frames.append({"frame": int(m.group(1)), "regs": regs, "views": views})
    if not frames:
        return
    print(f"\n#### burst correlation ({len(frames)} consecutive frames) ####")
    cam = [f["regs"].get(21) for f in frames]
    cam = [c for c in cam if c]
    if cam:
        print(f"  r21 (camera pos?) first/last: "
              f"{tuple(round(v, 2) for v in cam[0][:3])} -> "
              f"{tuple(round(v, 2) for v in cam[-1][:3])}")
    # Which logged view position tracks r21 (or the negated r23-25 translation)?
    all_views = sorted({v for f in frames for v in f["views"]})
    best = (1e30, None)
    for v in all_views:
        dists = []
        for f in frames:
            c = f["regs"].get(21)
            p = f["views"].get(v)
            if not c or not p:
                continue
            dists.append(sum(abs(c[i] - p[i]) for i in range(3)) / 3.0)
        if len(dists) >= 5:
            avg = sum(dists) / len(dists)
            print(f"  view i{v}: mean|r21 - pos7c4| = {avg:.2g} over {len(dists)} frames")
            if avg < best[0]:
                best = (avg, v)
    if best[1] is not None:
        print(f"  BEST-TRACKING VIEW: i{best[1]} (mean dist {best[0]:.2g})"
              + ("  <- TRACKS the GPU camera" if best[0] < 50 else
                 "  (too far to be the same camera)"))
    # r23-r25 translation vs negated r21
    t = []
    for f in frames[:3]:
        r21 = f["regs"].get(21)
        rows = [f["regs"].get(r) for r in (23, 24, 25)]
        if r21 and all(rows):
            t.append(tuple(rows[i][3] for i in range(3)))
    if t:
        print(f"  r23/24/25 [.w] = {tuple(round(x, 1) for x in t[0])} "
              f"vs -r21.xyz = {tuple(round(-v, 1) for v in cam[0][:3]) if cam else '?'}")


def report_s1(path):
    elems, old_elems, naive, xforms, vsmats, echoes = parse_s1(path)
    print("\n################ S1 evidence ################")
    for line in echoes[:80]:
        print("  " + line)
    if len(echoes) > 80:
        print(f"  ... ({len(echoes) - 80} more echoed lines)")

    world_found = 0
    for idx in sorted(elems):
        data = elems[idx]
        buf = bytearray(96)
        n = 0
        for off, b in data.items():
            if 0 <= off < 96:
                buf[off : off + len(b)] = b
                n += len(b)
        if n < 96:
            continue  # element lines not captured — skip
        if decode_elem96(f"consPos+{idx}", buf):
            world_found += 1
    if elems and not world_found:
        print("\n  NOTE: no world-view element ({0x30,0x810,0x680}) in this "
              "run's element dumps")

    for off_base in (0, 96, 192):
        if "docConsPos" in old_elems:
            data = old_elems["docConsPos"]
            buf = bytearray(96)
            n = 0
            for off, b in data.items():
                rel = off - off_base
                if 0 <= rel < 96:
                    buf[rel : rel + len(b)] = b
                    n += len(b)
            if n >= 96:
                decode_elem96(f"docConsPos elem@+0x{off_base:x}", buf)

    for tag, b in naive.items():
        dwords = [struct.unpack_from("<I", b, o)[0] for o in range(0, 32, 4)]
        ok = len(b) >= 52 and dwords[7] == 0x30 and dwords[9] == 0x810 \
            and dwords[11] == 0x680
        print(f"\n=== S1 element @ {tag} (first 32B): "
              f"{'LOOKS VALID' if ok else 'not an element header'} ===")
        print("  " + " ".join(f"{v:08x}" for v in dwords))

    for frame, reg, tag, raw in vsmats:
        print_mat16(frame, f"S1 vsmat reg=c{reg}", f"tag={tag}", raw)
    for frame, state, tag, raw in xforms:
        print_mat16(frame, "S1 xform", f"state={state} tag={tag}", raw)
    report_scan_elems(path)
    report_crecs(path)
    report_excache(path)
    report_burst(path)
    report_exfil(path)


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

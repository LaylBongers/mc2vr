#!/usr/bin/env python3
"""Scan MC2 shader containers for D3D9 shader bytecode and dump the CTAB
constant tables (constant name -> register index) — the S2a/#3a register-role
map input (docs/next_steps.md #3).

Containers (game data dir + Precache/): shaderVT*.bin / shaderR2VB*.bin /
shader3*.bin are {entry} directories of raw bytecode; Precache/*.precache are
"CERP"-keyed caches. We don't parse either directory — we scan the whole file
for "CTAB" FOURCCs (constant tables in D3D9 comment sections) and parse each
one relative to the FOURCC.

CTAB layout in this build (empirical, shaderVT.bin @0x108):
  base = fourcc+4: {Size=0x1c, Creator, Version, u1, u2, ConstantInfo, ...}
  Constant infos: {Name, RegisterSet:16, RegisterIndex:16, RegisterCount:16,
  Reserved:16, TypeInfo, DefaultValue} — the array position is found by
  validating entries (printable name, set in 0..3, idx<256, 1<=count<=64),
  then walking until an entry stops validating. TypeInfo =
  {Class:16, Type:16, Rows:16, Cols:16, ...} parsed when sane.

Usage: shader_ctab.py <file>... [-g REGEX]
  default: one line per constant per shader. -g: only shaders whose creator
  or any constant name matches REGEX (case-insensitive). -s: one-line summary
  per shader (names only).
"""

import re
import struct
import sys

CONST_INFO = struct.Struct(
    "<IHHHHII"
)  # Name, RegisterSet, RegisterIndex, RegisterCount, Reserved, TypeInfo, DefaultValue = 20 bytes
TYPE_INFO = struct.Struct("<HHHH")
SET_NAMES = {0: "b", 1: "i", 2: "f", 3: "s"}
CLASS_NAMES = {0: "scalar", 1: "vector", 2: "matrix", 3: "object", 4: "struct"}
TYPE_NAMES = {
    0: "void",
    1: "bool",
    2: "int",
    3: "float",
    5: "texture",
    9: "sampler",
    13: "samplerCUBE",
}
VERSIONS = {
    b"\x00\x01\xfe\xff": "vs_1_1",
    b"\x00\x02\xfe\xff": "vs_2_0",
    b"\x00\x03\xfe\xff": "vs_3_0",
    b"\x00\x01\xff\xff": "ps_1_x",
    b"\x00\x02\xff\xff": "ps_2_0",
    b"\x00\x03\xff\xff": "ps_3_0",
}


def cstring(buf, off):
    if not 0 <= off < len(buf):
        return None
    end = buf.find(b"\0", off)
    if end < 0 or end - off > 64:
        return None
    s = buf[off:end]
    if not s or not all(0x20 <= b <= 0x7E for b in s):
        return None
    return s.decode("latin1")


def valid_info(buf, base, pos):
    if pos + CONST_INFO.size > len(buf):
        return None
    name_off, rset, ridx, rcnt, _res, ti_off, _dv = CONST_INFO.unpack_from(buf, pos)
    if rset not in (0, 1, 2, 3) or ridx > 255 or not 1 <= rcnt <= 64:
        return None
    name = cstring(buf, base + name_off) if 0 < name_off < 0x100000 else None
    if name is None:
        return None
    cls = typ = rows = cols = 0
    if 0 < ti_off < 0x100000 and base + ti_off + TYPE_INFO.size <= len(buf):
        cls, typ, rows, cols = TYPE_INFO.unpack_from(buf, base + ti_off)
        if cls > 4 or typ > 15 or rows > 4 or cols > 4:
            cls = typ = rows = cols = 0
    return {
        "name": name,
        "set": rset,
        "idx": ridx,
        "count": rcnt,
        "class": cls,
        "type": typ,
        "rows": rows,
        "cols": cols,
    }


def parse_ctab(buf, fourcc):
    base = fourcc + 4
    if base + 0x1C > len(buf):
        return None
    size, creator, _ver = struct.unpack_from("<III", buf, base)
    if size not in (0x1C, 0x2C):
        return None
    # Find the info array: try dword offsets in the header region; accept the
    # first position with >=2 consecutive valid entries.
    for start in range(base + 0x1C, base + 0x30, 4):
        c0 = valid_info(buf, base, start)
        if c0 and valid_info(buf, base, start + CONST_INFO.size):
            consts, pos = [], start
            while True:
                c = valid_info(buf, base, pos)
                if c is None:
                    break
                consts.append(c)
                pos += CONST_INFO.size
                if len(consts) > 512:
                    break
            if len(consts) >= 2:
                return cstring(buf, base + creator), consts
    return None


def profile_of(buf, fourcc):
    for back in range(8, 1024, 4):
        pos = fourcc - back
        if pos < 0:
            return None
        prof = VERSIONS.get(bytes(buf[pos : pos + 4]))
        if prof:
            return prof
    return None


def scan(path):
    buf = open(path, "rb").read()
    out = []
    for m in re.finditer(re.escape(b"CTAB"), buf):
        parsed = parse_ctab(buf, m.start())
        if parsed:
            out.append((m.start(), profile_of(buf, m.start()), parsed[0], parsed[1]))
    return out


def reg_key(c):
    if c["set"] == 3:
        return f"s{c['idx']}"
    hi = c["idx"] + c["count"] - 1
    return f"c{c['idx']}" + (f"-c{hi}" if hi > c["idx"] else "")


def reg_sort(k):
    return (k[0], int(re.search(r"\d+", k).group()))


def main():
    args = sys.argv[1:]
    filt = None
    summary = False
    if "-g" in args:
        i = args.index("-g")
        filt = re.compile(args[i + 1], re.I)
        del args[i : i + 2]
    if "-s" in args:
        summary = True
        args.remove("-s")
    for path in args:
        hits = scan(path)
        shown = 0
        for off, prof, creator, consts in hits:
            if (
                filt
                and not filt.search(creator or "")
                and not any(filt.search(c["name"]) for c in consts)
            ):
                continue
            if shown == 0:
                print(f"===== {path}: {len(hits)} shader CTABs =====")
            shown += 1
            if summary:
                names = ",".join(sorted({c["name"] for c in consts}))
                print(f"@0x{off:x} {prof} '{creator}': {names}")
                continue
            print(f"\n-- shader @0x{off:x}  profile={prof}  creator='{creator}'")
            byreg = {}
            for c in consts:
                byreg.setdefault(reg_key(c), []).append(c)
            for key in sorted(byreg, key=reg_sort):
                c0 = byreg[key][0]
                extra = f"  [+{len(byreg[key]) - 1}]" if len(byreg[key]) > 1 else ""
                dims = f"{c0['rows']}x{c0['cols']}" if c0["rows"] and c0["cols"] else ""
                print(
                    f"   {key:9s} {c0['name']}"
                    f" ({CLASS_NAMES.get(c0['class'], c0['class'])}"
                    f" {TYPE_NAMES.get(c0['type'], c0['type'])} {dims}){extra}"
                )
        if shown == 0:
            print(f"===== {path}: {len(hits)} CTABs, none match =====")


if __name__ == "__main__":
    main()

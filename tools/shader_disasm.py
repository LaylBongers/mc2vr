#!/usr/bin/env python3
"""Minimal D3D9 vs_3_0/ps_3_0 shader disassembler — the viewContextData layout oracle.

Why this exists (docs/stereo_design.md): the viewContextData block is
written by SecuROM-VM'd code (opaque), so the *engine* binary cannot tell us
which of the 4-5 constant registers is the camera-position anchor and which
are the VP rows. The SHADERS are static plaintext data and are the other
consumer: their dp4/m4x4/mov instructions define the meaning of every
register. This tool decodes the token stream enough to map that usage.

Token layout (empirically calibrated on shader3.bin @0x9fb0, 2026-10-03 —
this d3dx build byte-swaps COMMENT tokens only; instructions are standard LE):
  - version token: 0xFFFE0300 (vs_3_0) etc.
  - comment token: size<<16 | 0xFFFE (swapped vs standard) — skip 1+size dwords
  - end token: 0x0000FFFF
  - instruction: opcode = bits 15-0 (D3DSIO_*), nparams = bits 27-24
    (EXCLUDES the opcode token; def = 1 dst + 4 raw floats, dcl = usage+operand)
  - operand: regtype = (t >> 28) & 7 (0=r,1=v,2=c,3=a,4=rast,6=o), regnum =
    bits 0-7; src swizzle = bits 16-23 (identity 0xE4); dst writemask =
    bits 8-11; src modifier = bits 24-27 (1 = negate)

Usage: shader_disasm.py <container> <hex-offset>   (offset = the "shader
@0x..." value from tools/shader_ctab.py; the version token is located at or
before it). Registers are annotated with CTAB constant names when available.
"""

import struct
import sys

VERSIONS = {
    0xFFFE0300: "vs_3_0",
    0xFFFE0200: "vs_2_0",
    0xFFFE0100: "vs_1_1",
    0xFFFF0300: "ps_3_0",
    0xFFFF0200: "ps_2_0",
    0xFFFF0100: "ps_1_x",
}
REGTYPE = {0: "r", 1: "v", 2: "c", 3: "aL", 4: "rast", 5: "attr", 6: "o", 7: "cI"}
OPS = {
    1: "mov",
    2: "add",
    3: "sub",
    4: "mad",
    5: "mul",
    6: "rcp",
    7: "rsq",
    8: "dp3",
    9: "dp4",
    10: "min",
    11: "max",
    12: "slt",
    13: "sge",
    14: "exp",
    15: "log",
    16: "lit",
    17: "dst",
    19: "frc",
    20: "m4x4",
    21: "m3x3",
    22: "m3x2",
    23: "m4x3",
    24: "call",
    25: "callnz",
    26: "loop",
    27: "ret",
    28: "endloop",
    29: "label",
    31: "dcl",
    32: "pow",
    34: "sgn",
    35: "abs",
    37: "rep",
    38: "endrep",
    39: "if",
    40: "ifc",
    41: "else",
    42: "endif",
    43: "break",
    44: "breakc",
    45: "mova",
    46: "defb",
    47: "defi",
    81: "def",
}
DCL_USAGE = {
    0: "position",
    1: "blendweight",
    2: "normal",
    3: "psize",
    4: "color",
    5: "texcoord",
    6: "tangent",
    7: "binormal",
    8: "tessfactor",
    9: "depth",
    10: "sample",
}


def operand(t, names):
    rtype = (t >> 28) & 7
    num = t & 0xFF
    base = f"{REGTYPE.get(rtype, 'x')}{num}"
    if rtype == 2 and names and num in names:
        base += f"({names[num]})"
    if t & 0x0000FF00:  # dst writemask bits 8-11
        wm = (t >> 8) & 0xF
        return base + "." + "".join("xyzw"[i] for i in range(4) if wm & (1 << i))
    sw = (t >> 16) & 0xFF
    mod = "-" if ((t >> 24) & 0xF) & 1 else ""
    if sw != 0xE4:
        sel = "".join("xyzw"[(sw >> (2 * i)) & 3] for i in range(4))
        return mod + base + "." + sel
    return mod + base


def find_code_start(buf, hint):
    pos = None
    for back in range(0, 8192, 4):
        p = hint - back
        if p < 0:
            break
        if struct.unpack_from("<I", buf, p)[0] in VERSIONS:
            pos = p
            break
    if pos is None:
        return None, None
    p = pos + 4
    while p + 4 <= len(buf):
        t = struct.unpack_from("<I", buf, p)[0]
        if (t & 0xFFFF) == 0xFFFE:  # swapped comment token
            p += 4 + 4 * (t >> 16)
        else:
            break
    return pos, p


def disasm(buf, code, names=None):
    out, p = [], code
    while p + 4 <= len(buf):
        t = struct.unpack_from("<I", buf, p)[0]
        if t == 0x0000FFFF:
            out.append("END")
            break
        op, nparams = t & 0xFFFF, (t >> 24) & 0xF
        name = OPS.get(op, f"op{op}")
        if name == "dcl":
            usage = struct.unpack_from("<I", buf, p + 4)[0]
            o = struct.unpack_from("<I", buf, p + 8)[0]
            out.append(
                f"dcl_{DCL_USAGE.get(usage & 0xF, usage & 0xF)}"
                f"{usage >> 16} {operand(o, names)}"
            )
            p += 12
            continue
        if name in ("def", "defb", "defi"):
            raws = [
                struct.unpack_from("<I", buf, p + 4 + 4 * i)[0]
                for i in range(nparams - 1)
            ]
            floats = [
                f"{struct.unpack('<f', struct.pack('<I', r))[0]:g}"
                if name == "def"
                else f"0x{r:08x}"
                for r in raws
            ]
            out.append(
                f"{name} {operand(struct.unpack_from('<I', buf, p + 4)[0], names)}, "
                + ", ".join(floats)
            )
            p += 4 + 4 * nparams
            continue
        ops = []
        for i in range(1, nparams + 1):
            ops.append(operand(struct.unpack_from("<I", buf, p + 4 * i)[0], names))
        out.append(f"{name:<8s} " + ", ".join(ops))
        p += 4 + 4 * nparams
    return out


def main():
    path, off = sys.argv[1], int(sys.argv[2], 0)
    buf = open(path, "rb").read()
    ver, code = find_code_start(buf, off)
    if ver is None:
        print("no version token found near offset")
        return
    print(
        f"version @0x{ver:x} = {VERSIONS.get(struct.unpack_from('<I', buf, ver)[0])},"
        f" code @0x{code:x}"
    )
    for line in disasm(buf, code):
        print(line)


if __name__ == "__main__":
    main()

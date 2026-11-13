#!/usr/bin/env python3
"""D3D9 shader-model 2/3 bytecode disassembler (vs/ps 2_0..3_0), for reading
the game's shaders: which constant registers mean what (e.g. the
viewContextData layout, docs/reverse_engineering/view_context_records.md —
written by SecuROM-VM'd code, so the shaders are the readable consumer).

Decoding follows the D3D9 token format (d3d9types.h):
  - version token 0xFFFE03xx (vs) / 0xFFFF03xx (ps); end token 0x0000FFFF;
    comment token (size << 16) | 0xFFFE, followed by `size` dwords.
  - instruction token: opcode bits 0-15 (D3DSIO_*), control bits 16-23
    (comparison for ifc/breakc/setp, texld project/bias), length bits 24-27
    (operand dwords after the token, SM2+), predicated bit 28, coissue bit 30.
  - operands: the first is the destination (except for flow control).
    register type = bits 28-30 | bits 11-12 << 3; number = bits 0-10;
    relative addressing bit 13 (SM3: an extra address token follows).
    dst: write mask bits 16-19, result modifier bits 20-23 (_sat, _pp,
    _centroid). src: swizzle bits 16-23, source modifier bits 24-27.
  - dcl: a usage token (usage bits 0-4, index bits 16-19; for samplers the
    texture type bits 27-30) before the dst operand. def/defi/defb: dst then
    4 (1 for defb) raw values.
The CTAB comment (D3DXSHADER_CONSTANTTABLE) annotates registers with names.

Usage: shader_disasm.py <container> <hex-offset>   (offset at or after the
version token, e.g. from tools/shader_hash.py / shader_ctab.py).
"""

import struct
import sys

VERSIONS = {0xFFFE: "vs", 0xFFFF: "ps"}

# D3DSHADER_PARAM_REGISTER_TYPE
REG_VS = {0: "r", 1: "v", 2: "c", 3: "a", 4: "oPos", 5: "oD", 6: "o", 7: "cI", 8: "oC", 9: "oDepth",
          10: "s", 11: "c", 12: "c", 13: "c", 14: "b", 15: "aL", 16: "tempfloat16", 17: "misc",
          18: "label", 19: "p"}
REG_PS = {**REG_VS, 3: "t"}
MISC = {0: "vPos", 1: "vFace"}
RASTOUT = {0: "oPos", 1: "oFog", 2: "oPts"}

OPS = {
    0: "nop", 1: "mov", 2: "add", 3: "sub", 4: "mad", 5: "mul", 6: "rcp", 7: "rsq", 8: "dp3", 9: "dp4",
    10: "min", 11: "max", 12: "slt", 13: "sge", 14: "exp", 15: "log", 16: "lit", 17: "dst", 18: "lrp",
    19: "frc", 20: "m4x4", 21: "m4x3", 22: "m3x4", 23: "m3x3", 24: "m3x2", 25: "call", 26: "callnz",
    27: "loop", 28: "ret", 29: "endloop", 30: "label", 31: "dcl", 32: "pow", 33: "crs", 34: "sgn",
    35: "abs", 36: "nrm", 37: "sincos", 38: "rep", 39: "endrep", 40: "if", 41: "ifc", 42: "else",
    43: "endif", 44: "break", 45: "breakc", 46: "mova", 47: "defb", 48: "defi",
    64: "texcoord", 65: "texkill", 66: "texld", 67: "texbem", 68: "texbeml", 69: "texreg2ar",
    70: "texreg2gb", 71: "texm3x2pad", 72: "texm3x2tex", 73: "texm3x3pad", 74: "texm3x3tex",
    76: "texm3x3spec", 77: "texm3x3vspec", 78: "expp", 79: "logp", 80: "cnd", 81: "def",
    82: "texreg2rgb", 83: "texdp3tex", 84: "texm3x2depth", 85: "texdp3", 86: "texm3x3", 87: "texdepth",
    88: "cmp", 89: "bem", 90: "dp2add", 91: "dsx", 92: "dsy", 93: "texldd", 94: "setp", 95: "texldl",
    96: "breakp",
}
# Instructions whose operands are all sources (no destination).
NO_DST = {"if", "ifc", "break", "breakc", "breakp", "callnz", "call", "loop", "rep", "label", "texkill",
          "else", "endif", "endloop", "endrep", "ret", "nop"}
INDENT_IN = {"if", "ifc", "loop", "rep", "else"}
INDENT_OUT = {"endif", "endloop", "endrep", "else"}
COMPARE = {1: "_gt", 2: "_eq", 3: "_ge", 4: "_lt", 5: "_ne", 6: "_le"}
USAGE = {0: "position", 1: "blendweight", 2: "blendindices", 3: "normal", 4: "psize", 5: "texcoord",
         6: "tangent", 7: "binormal", 8: "tessfactor", 9: "positiont", 10: "color", 11: "fog",
         12: "depth", 13: "sample"}
SAMPLER = {1: "1d", 2: "2d", 3: "cube", 4: "volume"}
# D3DSPSM_*: NONE, NEG, BIAS, BIASNEG, SIGN, SIGNNEG, COMP, X2, X2NEG, DZ, DW, ABS, ABSNEG, NOT.
SRC_MOD = {0: "{}", 1: "-{}", 2: "{}_bias", 3: "-{}_bias", 4: "{}_bx2", 5: "-{}_bx2", 6: "1-{}",
           7: "{}_x2", 8: "-{}_x2", 9: "{}_dz", 10: "{}_dw", 11: "abs({})", 12: "-abs({})", 13: "!{}"}


def u32(buf, p):
    return struct.unpack_from("<I", buf, p)[0]


def reg_name(t, is_ps, names):
    rtype = ((t >> 28) & 7) | (((t >> 11) & 3) << 3)
    num = t & 0x7FF
    kind = (REG_PS if is_ps else REG_VS).get(rtype, f"?{rtype}")
    if rtype == 17:
        return MISC.get(num, f"misc{num}")
    if rtype == 4:
        return RASTOUT.get(num, f"rast{num}")
    if rtype == 9:
        return "oDepth"
    base = f"{kind}{num}"
    key = {2: "c", 11: "c", 12: "c", 13: "c", 14: "b", 7: "i", 10: "s"}.get(rtype)
    if key and names:
        hit = names.get((key, num))
        if hit:
            base += f"<{hit}>"
    return base


def dst(t, is_ps, names, rel=""):
    s = reg_name(t, is_ps, names) + rel
    wm = (t >> 16) & 0xF
    if wm != 0xF:
        s += "." + "".join("xyzw"[i] for i in range(4) if wm & (1 << i))
    return s


def mods(t):
    m = (t >> 20) & 0xF
    return ("_sat" if m & 1 else "") + ("_pp" if m & 2 else "") + ("_centroid" if m & 4 else "")


def src(t, is_ps, names, rel=""):
    s = reg_name(t, is_ps, names) + rel
    sw = (t >> 16) & 0xFF
    if sw != 0xE4:
        sel = [("xyzw"[(sw >> (2 * i)) & 3]) for i in range(4)]
        if len(set(sel)) == 1:
            sel = sel[:1]
        s += "." + "".join(sel)
    return SRC_MOD.get((t >> 24) & 0xF, "{}?mod").format(s)


def parse_ctab(buf, p, size):
    """Standard D3DXSHADER_CONSTANTTABLE in a comment at p (data after the token)."""
    if size < 8 or buf[p:p + 4] != b"CTAB":
        return None
    base = p + 4
    _sz, _creator, _ver, count, info = struct.unpack_from("<IIIII", buf, base)
    names = {}
    for i in range(min(count, 512)):
        q = base + info + 20 * i
        if q + 20 > p + 4 * size:
            break
        name_off, rset, ridx, rcnt = struct.unpack_from("<IHHH", buf, q)
        end = buf.find(b"\0", base + name_off)
        name = buf[base + name_off:end].decode("latin1", "replace")
        key = {0: "b", 1: "i", 2: "c", 3: "s"}.get(rset, "?")
        for k in range(max(rcnt, 1)):
            names[(key, ridx + k)] = name if rcnt <= 1 else f"{name}[{k}]"
    return names


def find_version(buf, hint):
    for back in range(0, 1 << 16, 4):
        p = hint - back
        if p < 0:
            break
        t = u32(buf, p)
        if (t >> 16) in VERSIONS and (t & 0xFF00) in (0x0200, 0x0300):
            return p
    return None


def disasm(buf, ver_pos):
    vt = u32(buf, ver_pos)
    is_ps = (vt >> 16) == 0xFFFF
    sm = (vt >> 8) & 0xFF
    out = [f"{VERSIONS[vt >> 16]}_{sm}_{vt & 0xFF}"]
    names = {}
    p, depth = ver_pos + 4, 0
    while p + 4 <= len(buf):
        t = u32(buf, p)
        op = t & 0xFFFF
        if op == 0xFFFF:
            out.append("end")
            break
        if op == 0xFFFE:
            size = t >> 16
            names = parse_ctab(buf, p + 4, size) or names
            p += 4 + 4 * size
            continue
        length = (t >> 24) & 0xF
        ctrl = (t >> 16) & 0xFF
        name = OPS.get(op, f"op{op}")
        toks = [u32(buf, p + 4 + 4 * i) for i in range(length)]
        p += 4 + 4 * length
        if name in INDENT_OUT:
            depth = max(depth - 1, 0)
        pad = "  " * depth
        if name in INDENT_IN:
            depth += 1
        if name == "dcl":
            usage, d = toks[0], toks[1]
            rtype = ((d >> 28) & 7) | (((d >> 11) & 3) << 3)
            if rtype == 10:  # sampler
                tag = "_" + SAMPLER.get((usage >> 27) & 0xF, "?")
            elif is_ps and sm < 3 or rtype == 17:
                tag = ""
            else:
                tag = f"_{USAGE.get(usage & 0x1F, usage & 0x1F)}{(usage >> 16) & 0xF or ''}"
            out.append(f"{pad}dcl{tag}{mods(d)} {dst(d, is_ps, names)}")
            continue
        if name in ("def", "defi", "defb"):
            vals = toks[1:]
            if name == "def":
                vs = [f"{struct.unpack('<f', struct.pack('<I', v))[0]:g}" for v in vals]
            else:
                vs = [str(struct.unpack("<i", struct.pack("<I", v))[0]) for v in vals]
            out.append(f"{pad}{name} {dst(toks[0], is_ps, names)}, " + ", ".join(vs))
            continue
        # Operands, consuming SM3 relative-address tokens.
        operands, i = [], 0
        while i < len(toks):
            tok = toks[i]
            rel = ""
            if tok & (1 << 13) and sm >= 2 and i + 1 < len(toks):
                rel = f"[{src(toks[i + 1], is_ps, names)}]"
                i += 1
            operands.append((tok, rel))
            i += 1
        pred = ""
        if t & (1 << 28) and operands:
            # The predicate register follows the destination.
            k = 1 if name not in NO_DST else 0
            if k < len(operands):
                ptok, _ = operands.pop(k)
                pred = f"({src(ptok, is_ps, names)}) "
        suffix = ""
        if name in ("ifc", "breakc", "setp"):
            suffix = COMPARE.get(ctrl & 7, "")
        elif name == "texld":
            suffix = {1: "p", 2: "b"}.get(ctrl & 3, "")
        parts = []
        for j, (tok, rel) in enumerate(operands):
            if j == 0 and (name not in NO_DST or name == "texkill"):  # texkill's operand is dst-format
                parts.append(dst(tok, is_ps, names, rel))
                suffix += mods(tok)
            else:
                parts.append(src(tok, is_ps, names, rel))
        coissue = "+" if t & (1 << 30) else ""
        out.append(f"{pad}{pred}{coissue}{name}{suffix}" + (" " + ", ".join(parts) if parts else ""))
    return out


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        sys.exit(2)
    path, off = sys.argv[1], int(sys.argv[2], 0)
    buf = open(path, "rb").read()
    ver = find_version(buf, off)
    if ver is None:
        print("no version token found near offset")
        sys.exit(1)
    print(f"// version @0x{ver:x}")
    for line in disasm(buf, ver):
        print(line)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Numerical equivalence check of reimplemented pixel shaders against the
originals (docs/shaders.md). shader_iface.py checks the interface; this
checks the math.

For each registry entry (src/carrier/hooks/shader_reimpl.cpp), emulates the
ORIGINAL (container at the registry's offset) and OURS (generated header) as
ps_3_0 on N random trials and compares the outputs (oC0..oC3, oDepth):
|a-b| <= tol * max(1, |a|, |b|), both NaN equal, same-signed infinities equal.

Per trial, identical for both shaders:
  - inputs by semantic (dcl usage/index, not v#): uniform [-1,1], each float4
    folded to [0,1] with probability 1/2 (colors always [0,1]); vPos random
    pixel coordinates, vFace +-1;
  - c0..c223 likewise; on half the trials pointLights/spotLights (by CTAB
    name) get plausible values (PLAUSIBLE), else unphysical ranges make the
    lights inf and the final clamps hide them; i0..i15 small ints, b0..b15
    random bools (so all light-count branches get covered); Waves (PgWaterFP*)
    footprint scales enlarged on 3/4 of the trials so the wave branches run;
    def/defi/defb override them;
  - dsx/dsy (screen-space derivatives): a fixed affine function of the
    operand, the same for both shaders;
  - samplers are mocks looked up by CTAB name: the sun shadow map
    (globalDirLightShadowMap) returns one constant depth for every texel
    (per trial: +1e9 lit, -1e9 shadowed, or uniform [-2,2]), so a 1-tap and a
    9-tap lookup must agree; any other sampler returns per channel
    0.5 + 0.5 sin(dot(coord, k) + phase), k and phase seeded by the name
    (coord: the projected uv for texldp, xy for 2D, the normalized direction
    for cubes).
    Consequence: the shadow-map uv (cascade u/v rows, tap offsets, the
    9-tap's bilinear weights) is NOT checked; the cascade choice and the
    compared depth are (via the random-depth trials).
Trials run as SIMD lanes (numpy, float64), with per-lane masks for
if/else/loops/break/predication, as a GPU would.

Usage: shader_equiv.py <container shader3.bin> [registry.cpp] [generated dir]
           [--trials N] [--seed S] [--tol T] [--only <hash|symbol>]
           [--self]             compare each original with itself (self-test)
           [--perturb neg|def[:K]]  alter OURS in memory to show a difference
                                is caught: neg = negate src0 of the last
                                instruction writing oC0; def[:K] = add 0.01 to
                                the K-th (default first) def constant
Exit 1 if any entry differs, 2 on usage/emulation errors.
"""

import os
import re
import struct
import sys
import time

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from shader_iface import ROOT, fnv1a, load_generated, parse_ctab, reg_type, words_at  # noqa: E402
from shader_disasm import OPS  # noqa: E402

SHADOW_SAMPLER = "globalDirLightShadowMap"
NUM_C, NUM_I, NUM_B = 224, 16, 16

# Register types (D3DSHADER_PARAM_REGISTER_TYPE).
R_TEMP, R_INPUT, R_CONST, R_ADDR, R_CONSTINT, R_COLOROUT, R_DEPTHOUT = 0, 1, 2, 3, 7, 8, 9
R_SAMPLER, R_CONSTBOOL, R_LOOP, R_MISC, R_PRED = 10, 14, 15, 17, 19
USAGE_COLOR = 10
# D3DSPSM_*: 0 none, 1 neg, 2 bias, 3 biasneg, 4 sign(bx2), 5 signneg, 6 comp,
# 7 x2, 8 x2neg, 9 dz, 10 dw, 11 abs, 12 absneg, 13 not.
SRC_MOD_NAMES = {2: "bias", 3: "-bias", 4: "bx2", 5: "-bx2", 6: "comp", 7: "x2", 8: "-x2", 9: "dz", 10: "dw"}
NO_DST = {"if", "ifc", "else", "endif", "rep", "endrep", "loop", "endloop", "break", "breakc", "breakp",
          "call", "callnz", "label", "ret", "nop"}


class EmuError(Exception):
    pass


class Instr:
    __slots__ = ("op", "name", "ctrl", "pred", "dst", "srcs", "predsrc", "raw", "index")


def decode(words):
    """Token stream -> (instructions, defs, dcls, ctab). Operands are
    (token, relative-address token or None)."""
    if words[0] != 0xFFFF0300:
        raise EmuError("not ps_3_0 (version token %08x)" % words[0])
    prog, defs, dcls, ctab = [], [], [], None
    i = 1
    while i < len(words):
        t = words[i]
        op = t & 0xFFFF
        if t == 0x0000FFFF:
            break
        if op == 0xFFFE:
            n = (t >> 16) & 0x7FFF
            if ctab is None:
                ctab = parse_ctab(words, i)
            i += 1 + n
            continue
        n = (t >> 24) & 0xF
        args = words[i + 1:i + 1 + n]
        name = OPS.get(op)
        if name is None:
            raise EmuError("unknown opcode %d at token %d" % (op, i))
        if name == "dcl":
            dcls.append((args[0], args[1]))
        elif name in ("def", "defi", "defb"):
            defs.append((name, args[0], list(args[1:])))
        else:
            ops, k = [], 0
            while k < len(args):
                tok, rel = args[k], None
                if tok & (1 << 13):
                    rel = args[k + 1]
                    k += 1
                ops.append((tok, rel))
                k += 1
            ins = Instr()
            ins.op, ins.name, ins.ctrl, ins.raw = op, name, (t >> 16) & 0xFF, t
            ins.pred = bool(t & (1 << 28))
            has_dst = name not in NO_DST
            ins.predsrc = None
            if ins.pred:
                ins.predsrc = ops.pop(1 if has_dst else 0)
            ins.dst = ops[0] if has_dst else None
            ins.srcs = ops[1:] if has_dst else ops
            ins.index = len(prog)
            prog.append(ins)
        i += 1 + n
    return prog, defs, dcls, ctab or {}


def match_blocks(prog):
    """if/else/endif and loop/endloop pairing: index -> partner index."""
    match, stack = {}, []
    for ins in prog:
        if ins.name in ("if", "ifc", "rep", "loop"):
            stack.append(ins.index)
        elif ins.name == "else":
            match[stack[-1]] = ins.index
            stack[-1] = ins.index
        elif ins.name in ("endif", "endrep", "endloop"):
            match[stack.pop()] = ins.index
    if stack:
        raise EmuError("unbalanced flow control")
    return match


def sampler_names(ctab):
    return {reg + k: (name if count <= 1 else "%s[%d]" % (name, k))
            for name, (rset, reg, count) in ctab.items() if rset == "s" for k in range(max(count, 1))}


def mock_params(name):
    rng = np.random.default_rng(fnv1a(name.encode()))
    k = rng.uniform(1.0, 5.0, (4, 3)) * rng.choice([-1.0, 1.0], (4, 3))
    return k, rng.uniform(0, 2 * np.pi, 4)


class Trials:
    """Per-trial inputs, shared by both shaders."""

    def __init__(self, n, rng, ctab=None):
        self.n = n
        self.rng = rng
        self.c = self.mixed((n, NUM_C, 4))
        self.plausible = rng.random(n) < 0.5
        for name, (rset, reg, count) in (ctab or {}).items():
            if rset == "c" and name in PLAUSIBLE:
                PLAUSIBLE[name](self, reg, count)
        self.i = rng.integers(-4, 9, (n, NUM_I, 4))
        self.b = rng.random((n, NUM_B)) < 0.5
        mode = rng.integers(0, 4, n)
        self.shadow = np.where(mode == 0, 1e9, np.where(mode == 1, -1e9, rng.uniform(-2, 2, n)))
        self.vpos = np.stack([rng.uniform(0, 2048, n), rng.uniform(0, 2048, n), np.zeros(n), np.zeros(n)], 1)
        self.vface = rng.choice([-1.0, 1.0], n)
        self.inputs = {}

    def waves(self, reg, count):
        """Waves (PgWaterFP*): two sets of rows [0..5], [6..11]; [3]/[9] scale
        the pixel footprint the wave fade is computed from. The fade is
        non-zero only when |Waves[3 or 9] * input| > 1, which [-1,1] values
        never reach: on 3/4 of the trials scale them up so the wave branches
        run."""
        big = self.rng.random(self.n) < 0.75
        for k in (3, 9):
            if k < count:
                self.c[big, reg + k] *= 8

    def spot_lights(self, reg, count):
        """spotLights (lights.hlsli): position (w cone min), direction (w cone
        max), colour, (near, exponent, far, cone scale). Fully random values
        mostly give 0^-e = inf ranges that the final clamp then hides, so on
        the plausible half of the trials: near < far, exponent > 0, min < max."""
        n, rng, lanes = self.n, self.rng, self.plausible
        for k in range(count // 4):
            r = reg + 4 * k
            if r + 3 >= NUM_C:
                break
            lo, hi = np.sort(rng.uniform(-1, 1, (2, n)), 0)
            near = rng.uniform(0, 1, n)
            s3 = np.stack([near, rng.uniform(0.25, 4, n), near + rng.uniform(0.5, 3, n),
                           rng.uniform(0.5, 4, n)], 1)
            d = rng.normal(size=(n, 3))
            d /= np.linalg.norm(d, axis=1, keepdims=True)
            self.c[lanes, r, 3] = lo[lanes]
            self.c[lanes, r + 1, :3] = d[lanes]
            self.c[lanes, r + 1, 3] = hi[lanes]
            self.c[lanes, r + 2] = np.abs(self.c[lanes, r + 2])
            self.c[lanes, r + 3] = s3[lanes]

    def point_lights(self, reg, count):
        """pointLights: position, colour, attenuation saturate(dist*x + z):
        on the plausible half, a positive colour and a falloff (x < 0 < z)."""
        n, rng, lanes = self.n, self.rng, self.plausible
        for k in range(count // 3):
            r = reg + 3 * k
            if r + 2 >= NUM_C:
                break
            self.c[lanes, r + 1] = np.abs(self.c[lanes, r + 1])
            self.c[lanes, r + 2, 0] = -rng.uniform(0.2, 2, n)[lanes]
            self.c[lanes, r + 2, 2] = rng.uniform(0.5, 2, n)[lanes]

    def mixed(self, shape):
        """Uniform [-1,1]; each register (float4) of each trial is folded to
        [0,1] with probability 1/2, so all-positive colours, materials and
        lights (the common case) and sign mixes both occur."""
        v = self.rng.uniform(-1, 1, shape)
        fold = self.rng.random(shape[:-1] + (1,)) < 0.5
        return np.where(fold, np.abs(v), v)

    def semantic(self, usage, index):
        key = (usage, index)
        if key not in self.inputs:
            v = self.mixed((self.n, 4))
            self.inputs[key] = np.abs(v) if usage == USAGE_COLOR else v
        return self.inputs[key]

    def summary(self, lane):
        bs = [k for k in range(NUM_B) if self.b[lane, k]]
        sd = self.shadow[lane]
        return "bools set %s, %s constants, shadow depth %s" % (
            " ".join("b%d" % k for k in bs) or "none", "plausible" if self.plausible[lane] else "random",
            {1e9: "+1e9 (lit)", -1e9: "-1e9 (shadowed)"}.get(sd, "%.4g" % sd))


# Constant arrays (CTAB name) that get plausible values on half the trials.
PLAUSIBLE = {"spotLights": Trials.spot_lights, "pointLights": Trials.point_lights, "Waves": Trials.waves}


def f32(u):
    return struct.unpack("<f", struct.pack("<I", u))[0]


def i32(u):
    return struct.unpack("<i", struct.pack("<I", u))[0]


SWZ = [[(sw >> (2 * k)) & 3 for k in range(4)] for sw in range(256)]
COMPARE = {1: np.greater, 2: np.equal, 3: np.greater_equal, 4: np.less, 5: np.not_equal, 6: np.less_equal}


class Machine:
    def __init__(self, words, trials, label):
        self.label = label
        self.prog, defs, dcls, self.ctab = decode(words)
        self.match = match_blocks(self.prog)
        self.t = trials
        n = trials.n
        self.n = n
        self.c = trials.c.copy()
        self.i = trials.i.copy()
        self.b = trials.b.copy()
        for kind, dtok, vals in defs:
            r = dtok & 0x7FF
            if kind == "def":
                self.c[:, r] = [f32(v) for v in vals]
            elif kind == "defi":
                self.i[:, r] = [i32(v) for v in vals]
            else:
                self.b[:, r] = bool(vals[0])
        self.v = {}
        self.samplers = {}
        snames = sampler_names(self.ctab)
        for usage, dtok in dcls:
            rt, r = reg_type(dtok), dtok & 0x7FF
            if rt == R_INPUT:
                self.v[r] = trials.semantic(usage & 0x1F, (usage >> 16) & 0xF)
            elif rt == R_SAMPLER:
                name = snames.get(r)
                if name is None:
                    raise EmuError("%s: sampler s%d has no CTAB name" % (label, r))
                self.samplers[r] = (name, (usage >> 27) & 0xF, mock_params(name))
            elif rt == R_MISC:
                pass
            else:
                raise EmuError("%s: dcl of register type %d" % (label, rt))
        self.r = {}
        self.out = {}        # (type, num) -> (n,4)
        self.written = {}    # (type, num) -> 4-bit component mask
        self.p = np.zeros((n, 4), bool)
        self.aL = np.zeros(n, np.int64)
        self.killed = np.zeros(n, bool)
        self.active = np.ones(n, bool)

    # ---- operands -------------------------------------------------------
    def fail(self, ins, what):
        raise EmuError("%s: instruction %d (%s): %s" % (self.label, ins.index, ins.name, what))

    def rel_offset(self, ins, rel):
        rt = reg_type(rel)
        if rt == R_LOOP:
            return self.aL
        if rt == R_ADDR:
            self.fail(ins, "a0 relative addressing (not in ps_3_0)")
        self.fail(ins, "relative address register type %d" % rt)

    def fetch(self, ins, tok, rel):
        """Raw (n,4) value of a source register, before swizzle/modifier."""
        rt, r = reg_type(tok), tok & 0x7FF
        n = self.n
        if rt == R_TEMP:
            return self.r.get(r, np.zeros((n, 4)))
        if rt == R_CONST:
            if rel is None:
                return self.c[:, r]
            idx = np.clip(r + self.rel_offset(ins, rel), 0, NUM_C - 1)
            return self.c[np.arange(n), idx]
        if rt == R_INPUT:
            if rel is not None:
                self.fail(ins, "relative input addressing")
            if r not in self.v:
                self.fail(ins, "read of undeclared v%d" % r)
            return self.v[r]
        if rt == R_CONSTINT:
            return self.i[:, r].astype(np.float64)
        if rt == R_CONSTBOOL:
            return np.repeat(self.b[:, r:r + 1], 4, 1)
        if rt == R_PRED:
            return self.p
        if rt == R_LOOP:
            return np.repeat(self.aL[:, None].astype(np.float64), 4, 1)
        if rt == R_MISC:
            if r == 0:
                return self.t.vpos
            if r == 1:
                return np.repeat(self.t.vface[:, None], 4, 1)
        self.fail(ins, "unsupported source register type %d" % rt)

    def src(self, ins, k):
        tok, rel = ins.srcs[k]
        val = self.fetch(ins, tok, rel)
        sw = (tok >> 16) & 0xFF
        if sw != 0xE4:
            val = val[:, SWZ[sw]]
        mod = (tok >> 24) & 0xF
        if mod == 0:
            return val
        if mod == 1:
            return -val
        if mod == 11:
            return np.abs(val)
        if mod == 12:
            return -np.abs(val)
        if mod == 13:
            return ~val
        self.fail(ins, "source modifier %s" % SRC_MOD_NAMES.get(mod, mod))

    def scalar(self, ins, k):
        return self.src(ins, k)[:, 0]

    def pred_mask(self, ins):
        tok, _ = ins.predsrc
        v = self.p[:, SWZ[(tok >> 16) & 0xFF]]
        mod = (tok >> 24) & 0xF
        if mod == 13:
            v = ~v
        elif mod:
            self.fail(ins, "predicate modifier %d" % mod)
        return v

    def write(self, ins, val):
        tok, rel = ins.dst
        if rel is not None:
            self.fail(ins, "relative destination addressing")
        rt, r = reg_type(tok), tok & 0x7FF
        wm = (tok >> 16) & 0xF
        if (tok >> 20) & 1:  # _sat (NaN -> 0)
            val = np.where(np.isnan(val), 0.0, np.clip(val, 0.0, 1.0))
        if val.ndim == 1:
            val = np.repeat(val[:, None], 4, 1)
        comps = np.array([(wm >> k) & 1 for k in range(4)], bool)
        m = self.active[:, None] & comps[None, :]
        if ins.pred:
            m = m & self.pred_mask(ins)
        if rt == R_TEMP:
            old = self.r.get(r, np.zeros((self.n, 4)))
            self.r[r] = np.where(m, val, old)
        elif rt in (R_COLOROUT, R_DEPTHOUT):
            key = (rt, r)
            if rt == R_DEPTHOUT:
                comps = np.array([True, False, False, False])
                m = m & comps[None, :]
            old = self.out.get(key, np.zeros((self.n, 4)))
            self.out[key] = np.where(m, val, old)
            self.written[key] = self.written.get(key, 0) | int(sum(int(c) << k for k, c in enumerate(comps)))
        elif rt == R_PRED:
            self.p = np.where(m, val.astype(bool), self.p)
        else:
            self.fail(ins, "unsupported destination register type %d" % rt)

    # ---- samplers -------------------------------------------------------
    def sample(self, ins, coord, stok):
        if reg_type(stok) != R_SAMPLER:
            self.fail(ins, "texld without a sampler operand")
        s = stok & 0x7FF
        if s not in self.samplers:
            self.fail(ins, "undeclared sampler s%d" % s)
        name, stype, (k, phase) = self.samplers[s]
        if name == SHADOW_SAMPLER:
            out = np.repeat(self.t.shadow[:, None], 4, 1)
        else:
            if stype == 2:
                x = coord[:, :2]
                kk = k[:, :2]
            elif stype == 3:
                x = coord[:, :3] / np.linalg.norm(coord[:, :3], axis=1, keepdims=True)
                kk = k
            elif stype in (1, 4):
                x = coord[:, :3] if stype == 4 else coord[:, :1]
                kk = k[:, :x.shape[1]]
            else:
                self.fail(ins, "sampler type %d" % stype)
            out = 0.5 + 0.5 * np.sin(x @ kk.T + phase[None, :])
        sw = (stok >> 16) & 0xFF
        return out[:, SWZ[sw]] if sw != 0xE4 else out

    # ---- execution ------------------------------------------------------
    def run(self):
        prog, match = self.prog, self.match
        stack = []
        pc = 0
        steps = 0
        with np.errstate(all="ignore"):
            while pc < len(prog):
                steps += 1
                if steps > 2_000_000:
                    raise EmuError("%s: runaway execution" % self.label)
                ins = prog[pc]
                name = ins.name
                nxt = pc + 1
                if name == "if":
                    tok = ins.srcs[0][0]
                    rt = reg_type(tok)
                    if rt not in (R_CONSTBOOL, R_PRED):
                        self.fail(ins, "if on register type %d" % rt)
                    cond = self.src(ins, 0)[:, 0].astype(bool)
                    stack.append(["if", self.active, cond])
                    self.active = self.active & cond
                    if not self.active.any():
                        nxt = match[pc]
                elif name == "ifc":
                    cond = COMPARE[ins.ctrl & 7](self.scalar(ins, 0), self.scalar(ins, 1))
                    stack.append(["if", self.active, cond])
                    self.active = self.active & cond
                    if not self.active.any():
                        nxt = match[pc]
                elif name == "else":
                    f = stack[-1]
                    self.active = f[1] & ~f[2]
                    if not self.active.any():
                        nxt = match[pc]
                elif name == "endif":
                    self.active = stack.pop()[1]
                elif name in ("rep", "loop"):
                    itok = ins.srcs[0 if name == "rep" else 1][0]
                    if reg_type(itok) != R_CONSTINT:
                        self.fail(ins, "loop counter register type %d" % reg_type(itok))
                    ireg = self.i[:, itok & 0x7FF]
                    count = np.clip(ireg[:, 0], 0, 255)
                    frame = ["loop", pc, self.active, 0, count, ireg[:, 2], self.aL.copy(),
                             np.zeros(self.n, bool)]
                    if name == "loop":
                        self.aL = np.where(self.active, ireg[:, 1], self.aL)
                    self.active = self.active & (count > 0)
                    if self.active.any():
                        stack.append(frame)
                    else:
                        self.active = frame[2]
                        self.aL = frame[6]
                        nxt = match[pc] + 1
                elif name in ("endrep", "endloop"):
                    f = stack[-1]
                    f[3] += 1
                    if name == "endloop":
                        self.aL = self.aL + f[5]
                    self.active = f[2] & ~f[7] & (f[3] < f[4])
                    if self.active.any():
                        nxt = f[1] + 1
                    else:
                        stack.pop()
                        self.active = f[2]
                        self.aL = f[6]
                elif name in ("break", "breakc", "breakp"):
                    if name == "break":
                        hit = self.active
                    elif name == "breakc":
                        hit = self.active & COMPARE[ins.ctrl & 7](self.scalar(ins, 0), self.scalar(ins, 1))
                    else:
                        tok = ins.srcs[0][0]
                        v = self.p[:, SWZ[(tok >> 16) & 0xFF][0]]
                        hit = self.active & (~v if (tok >> 24) & 0xF == 13 else v)
                    for f in reversed(stack):
                        if f[0] == "loop":
                            f[7] = f[7] | hit
                            break
                        f[1] = f[1] & ~hit
                    else:
                        self.fail(ins, "break outside a loop")
                    self.active = self.active & ~hit
                elif name == "nop":
                    pass
                elif name == "texkill":
                    tok = ins.dst[0]
                    r = self.fetch(ins, tok, ins.dst[1])
                    wm = (tok >> 16) & 0xF
                    comps = [k for k in range(4) if (wm >> k) & 1]
                    self.killed |= self.active & (r[:, comps] < 0).any(1)
                else:
                    self.alu(ins)
                pc = nxt
        return self.out, self.written, self.killed

    def alu(self, ins):
        name = ins.name
        s = lambda k: self.src(ins, k)  # noqa: E731
        if name == "mov":
            v = s(0)
        elif name == "add":
            v = s(0) + s(1)
        elif name == "sub":
            v = s(0) - s(1)
        elif name == "mul":
            v = s(0) * s(1)
        elif name == "mad":
            v = s(0) * s(1) + s(2)
        elif name == "dp3":
            v = (s(0)[:, :3] * s(1)[:, :3]).sum(1)
        elif name == "dp4":
            v = (s(0) * s(1)).sum(1)
        elif name == "dp2add":
            v = (s(0)[:, :2] * s(1)[:, :2]).sum(1) + self.scalar(ins, 2)
        elif name == "rcp":
            v = 1.0 / self.scalar(ins, 0)          # 1/0 = +-inf
        elif name == "rsq":
            v = 1.0 / np.sqrt(np.abs(self.scalar(ins, 0)))
        elif name == "exp":
            v = np.exp2(self.scalar(ins, 0))
        elif name == "log":
            v = np.log2(np.abs(self.scalar(ins, 0)))
        elif name == "pow":
            v = np.exp2(self.scalar(ins, 1) * np.log2(np.abs(self.scalar(ins, 0))))
        elif name in ("dsx", "dsy"):
            # Screen-space derivatives: a fixed affine function of the operand
            # (each shader takes them of the same values, so any mock works).
            v = (0.37 * s(0) + 0.1) if name == "dsx" else (-0.23 * s(0) + 0.05)
        elif name == "frc":
            a = s(0)
            v = a - np.floor(a)
        elif name == "nrm":
            a = s(0)
            v = a * (1.0 / np.sqrt((a[:, :3] * a[:, :3]).sum(1)))[:, None]
        elif name == "max":
            a, b = s(0), s(1)
            v = np.where(a >= b, a, b)
        elif name == "min":
            a, b = s(0), s(1)
            v = np.where(a < b, a, b)
        elif name == "cmp":
            v = np.where(s(0) >= 0, s(1), s(2))
        elif name == "lrp":
            a, b, c = s(0), s(1), s(2)
            v = a * (b - c) + c
        elif name == "slt":
            v = (s(0) < s(1)).astype(np.float64)
        elif name == "sge":
            v = (s(0) >= s(1)).astype(np.float64)
        elif name == "sgn":
            v = np.sign(s(0))
        elif name == "abs":
            v = np.abs(s(0))
        elif name == "crs":
            a, b = s(0), s(1)
            v = np.zeros_like(a)
            v[:, :3] = np.cross(a[:, :3], b[:, :3])
        elif name == "sincos":
            a = self.scalar(ins, 0)
            v = np.stack([np.cos(a), np.sin(a), np.zeros_like(a), np.zeros_like(a)], 1)
        elif name == "setp":
            v = COMPARE[ins.ctrl & 7](s(0), s(1))
        elif name in ("texld", "texldl", "texldd"):
            coord = s(0)
            if name == "texld" and ins.ctrl & 3 == 1:  # texldp
                coord = coord / coord[:, 3:4]
            # texldb (ctrl 2) / texldl / texldd: the mocks have no mips.
            v = self.sample(ins, coord, ins.srcs[1][0])
        else:
            self.fail(ins, "unsupported opcode")
        self.write(ins, v)


# ---- comparison ----------------------------------------------------------
def out_name(key, comp):
    rt, r = key
    return ("oDepth" if rt == R_DEPTHOUT else "oC%d.%s" % (r, "xyzw"[comp]))


def compare(a, b, trials, tol):
    """-> (ok, max error, message)."""
    oa, wa, ka = a
    ob, wb, kb = b
    msgs = []
    if wa != wb:
        msgs.append("written outputs differ: ours %s, orig %s" % (
            {out_name(k, 0)[:3]: "%x" % m for k, m in wb.items()},
            {out_name(k, 0)[:3]: "%x" % m for k, m in wa.items()}))
    bad = ka != kb
    worst, first = 0.0, None
    if bad.any():
        lane = int(np.argmax(bad))
        first = (lane, "texkill differs (orig killed %s, ours %s)" % (bool(ka[lane]), bool(kb[lane])))
    alive = ~(ka | kb)
    for key in sorted(set(wa) & set(wb)):
        m = wa[key] & wb[key]
        for comp in range(4):
            if not (m >> comp) & 1:
                continue
            x, y = oa[key][:, comp], ob[key][:, comp]
            both_nan = np.isnan(x) & np.isnan(y)
            same = (x == y) | both_nan
            err = np.abs(x - y)
            scale = np.maximum(1.0, np.maximum(np.abs(x), np.abs(y)))
            rel = np.where(same, 0.0, np.where(np.isnan(err), np.inf, err / scale))
            rel = np.where(alive, rel, 0.0)
            worst = max(worst, float(rel.max()))
            fail = rel > tol
            if fail.any():
                lane = int(np.argmax(fail))
                if first is None or lane < first[0]:
                    first = (lane, "%s: orig %.6g, ours %.6g" % (out_name(key, comp), x[lane], y[lane]))
    ok = first is None and not msgs
    if first is not None:
        lane, what = first
        msgs.append("first failing trial %d: %s; %s" % (lane, what, trials.summary(lane)))
    return ok, worst, msgs


# ---- perturbation (demonstrates that differences are caught) -------------
def negate_src(tok):
    """Toggle a source operand's negation: none <-> neg, abs <-> absneg."""
    mod = (tok >> 24) & 0xF
    flip = {0: 1, 1: 0, 11: 12, 12: 11}
    if mod not in flip:
        raise EmuError("cannot negate a source with modifier %d" % mod)
    return (tok & ~(0xF << 24)) | (flip[mod] << 24)


def perturb(words, how):
    w = list(words)
    i = 1
    last_oc0 = None
    ndef, want = 0, int(how.split(":")[1]) if how.startswith("def:") else 0
    while i < len(w):
        t = w[i]
        op = t & 0xFFFF
        if t == 0x0000FFFF:
            break
        if op == 0xFFFE:
            i += 1 + ((t >> 16) & 0x7FFF)
            continue
        n = (t >> 24) & 0xF
        name = OPS.get(op)
        if how.startswith("def") and name == "def":
            if ndef < want:
                ndef += 1
                i += 1 + n
                continue
            for k in range(4):
                w[i + 2 + k] = struct.unpack("<I", struct.pack("<f", f32(w[i + 2 + k]) + 0.01))[0]
            return w, "def c%d += 0.01" % (w[i + 1] & 0x7FF)
        if name not in NO_DST and name not in ("dcl", "def", "defi", "defb", "texkill") and n >= 2:
            d = w[i + 1]
            if reg_type(d) == R_COLOROUT and (d & 0x7FF) == 0:
                last_oc0 = i
        i += 1 + n
    if how == "neg" and last_oc0 is not None:
        w[last_oc0 + 2] = negate_src(w[last_oc0 + 2])
        return w, "negated src0 of %s at token %d" % (OPS[w[last_oc0] & 0xFFFF], last_oc0)
    raise EmuError("nothing to perturb (%s)" % how)


def main():
    argv = sys.argv[1:]
    opts = {"--trials": "1000", "--seed": "1", "--tol": "1e-4", "--only": None, "--perturb": None}
    self_test = False
    pos = []
    k = 0
    while k < len(argv):
        a = argv[k]
        if a in opts:
            if k + 1 >= len(argv):
                print(__doc__)
                return 2
            opts[a] = argv[k + 1]
            k += 2
            continue
        if a == "--self":
            self_test = True
        elif a.startswith("-"):
            print(__doc__)
            return 2
        else:
            pos.append(a)
        k += 1
    if not pos:
        print(__doc__)
        return 2
    container = open(pos[0], "rb").read()
    registry = pos[1] if len(pos) > 1 else os.path.join(ROOT, "src/carrier/hooks/shader_reimpl.cpp")
    gen = pos[2] if len(pos) > 2 else os.path.join(ROOT, "build/win32/generated/shaders")
    entries = re.findall(r'\{0x([0-9a-f]{8})u, "[^"]*\(shader3\.bin@(0x[0-9a-f]+)\)", mc2vr::shaders::(\w+)',
                         open(registry).read())
    if opts["--only"]:
        entries = [e for e in entries if opts["--only"] in (e[0], e[2])]
    if not entries:
        print("no registry entries found in", registry)
        return 2
    ntrials, tol = int(opts["--trials"]), float(opts["--tol"])
    bad = errors = 0
    t0 = time.time()
    for idx, (h, off, sym) in enumerate(entries):
        orig = words_at(container, int(off, 16))
        note = ""
        try:
            if self_test:
                ours = list(orig)
            else:
                path = os.path.join(gen, sym + ".h")
                if not os.path.exists(path):
                    raise EmuError("no generated header %s (build first)" % path)
                ours = load_generated(path)
            if opts["--perturb"]:
                ours, note = perturb(ours, opts["--perturb"])
                note = "  [perturbed: %s]" % note
            # Same seed per entry for both shaders; distinct per entry.
            trials = Trials(ntrials, np.random.default_rng([int(opts["--seed"]), idx]), decode(orig)[3])
            ro = Machine(orig, trials, "orig " + sym).run()
            rn = Machine(ours, trials, "ours " + sym).run()
        except EmuError as e:
            print("ERR %s %-26s %s" % (h, sym, e))
            errors += 1
            continue
        with np.errstate(all="ignore"):
            ok, worst, msgs = compare(ro, rn, trials, tol)
        print("%s %s %-26s max err %.2e%s" % ("ok " if ok else "BAD", h, sym, worst, note))
        for m in msgs:
            print("    " + m)
        bad += not ok
    print("%d of %d differ, %d errors (%d trials, tol %g, %.1f s)" % (
        bad, len(entries), errors, ntrials, tol, time.time() - t0))
    return 2 if errors else (1 if bad else 0)


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# Parse mc2vr_carrier.log M3 ViewDump blocks and analyze ViewEntry field usage.
# Usage: analyze_dumps.py [path/to/mc2vr_carrier.log] [stream_dump_file_or_dir]
#
# Produces:
#   - per-dump header summary (idx, type, entry, obj, t3, first/refresh)
#   - load-vs-steady diff for any idx dumped twice
#   - nonzero-dword field map per dump (with float interpretation)
#   - varying-across-dumps region analysis (constant vs per-view fields)
#
# S2c-0 stream-dump support (src/carrier/stream_capture.cpp): with a second
# argument (a mc2vr_stream_frame<N>.txt file, or a directory containing them,
# or omitted to auto-discover next to the log) each stream is decoded with the
# RE'd RenderCmd_ExecuteStream opcode table and a per-frame census printed
# (per-opcode counts + payload field stats). "S2c" log lines are echoed.
#
# Written during M3 field-map derivation. Entry size and dump line format must stay in sync
# with src/carrier/render_dump.cpp (dump_bytes: 32 bytes/line, "M3 entry
# +0xOFF: hex", "M3 obj +0xOFF: hex").

import glob
import os
import re
import struct
import sys

VIEW_STRIDE = 0x810
OBJ_LEN = 0x40
ELEM_SIZE = 96

# RenderCmd_ExecuteStream (0x008569d0) command size table: dwords incl. opcode.
# op 0x00 = halt. op 0x13 = advance-only no-op (shares case 0x0d's tail).
OP_SIZE = [
    1,
    3,
    4,
    4,
    3,
    3,
    3,
    3,
    7,
    3,
    1,
    1,
    1,
    2,
    2,
    3,
    5,
    5,
    3,
    2,
    3,
    2,
    3,
    4,
    2,
    1,
    1,
]
OP_NAME = [
    "halt",
    "SetRenderState",
    "SetVertexShaderConstantF",
    "SetPixelShaderConstantF",
    "BindViewTextureFamily(+0x101)",
    "StreamSourceBind",
    "SetRenderTarget0+DepthStencil",
    "SetRenderTarget",
    "SetScreenConstants",
    "SetClipPlane",
    "ReapplyClipPlaneMask",
    "DisableClipPlanes",
    "ViewType9StretchBlit",
    "StretchRectFromGlobal",
    "SetPassObject",
    "Thunk_004a05b9",
    "BeginSurfacePass",
    "EndSurfacePass",
    "BindTexture+Samplers",
    "NOOP",
    "BindViewTextureFamily",
    "ClearRTPass_0074af30",
    "DeviceSetStreamSource",
    "Thunk_0256af90",
    "SetTexture0+RS0x9a",
    "HalfResPassBegin",
    "HalfResPassEnd",
]
# Fields (dword indices after the opcode) worth aggregating per opcode.
OP_FIELDS = {
    0x01: ["state", "value"],
    0x02: ["vsReg", "dataPtr", "vec4"],
    0x03: ["psReg", "dataPtr", "vec4"],
    0x08: ["p1", "p2", "p3", "p4", "p5", "p6"],
    0x09: ["planeBit", "planePtr"],
    0x10: ["a", "b", "c", "gate"],
    0x11: ["a", "b", "c", "gate"],
}


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


# ---- view-rewrite log echo ---------------------------------------------------

VIEW_ECHO = re.compile(r"\bview:")
S2C_ECHO = re.compile(r"\bS2c")


def report_view(path):
    """Echo the view-rewrite and S2c stream-capture log lines."""
    sections = [("view rewrite", VIEW_ECHO), ("S2c stream capture", S2C_ECHO)]
    for title, pat in sections:
        echoes = []
        for line in open(path, encoding="utf-8", errors="replace"):
            if pat.search(line):
                echoes.append(line.rstrip())
        if echoes:
            print(f"\n################ {title} ################")
            for line in echoes[:80]:
                print("  " + line)
            if len(echoes) > 80:
                print(f"  ... ({len(echoes) - 80} more echoed lines)")


# ---- S2c-2 eye dumps (BMP pairs) --------------------------------------------


def load_bmp(path):
    """Minimal 24-bit BMP loader -> (width, height, bytes-of-BGR-rows)."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:2] != b"BM":
        raise ValueError("not a BMP")
    off = struct.unpack_from("<I", data, 10)[0]
    w, h = struct.unpack_from("<ii", data, 18)
    bpp = struct.unpack_from("<H", data, 28)[0]
    if bpp != 24:
        raise ValueError(f"bpp={bpp}, expected 24")
    row = (w * 3 + 3) // 4 * 4
    return w, h, data[off : off + row * h], row


def gray_rows(w, h, buf, row, step=4):
    """Downsampled grayscale rows for shift correlation."""
    rows = []
    for y in range(0, h, step * 2):
        base = y * row
        r = []
        for x in range(0, w, step):
            i = base + x * 3
            r.append((buf[i] * 299 + buf[i + 1] * 587 + buf[i + 2] * 114) // 1000)
        rows.append(r)
    return rows


def best_shift(left, right, max_shift=16):
    """Integer horizontal shift of `right` minimizing SAD vs `left`.

    max_shift is in downsampled units; among near-ties (SAD within 5% of the
    minimum) the smallest-magnitude shift wins, to avoid aliasing on periodic
    or flat content."""
    results = []
    for sh in range(-max_shift, max_shift + 1):
        sad = 0
        n = 0
        for lr, rr in zip(left, right):
            for x in range(max(0, -sh), min(len(lr), len(lr) - sh)):
                a = lr[x]
                b = rr[x + sh]
                sad += abs(a - b)
                n += 1
        if n == 0:
            continue
        results.append((sh, sad / n))
    if not results:
        return 0, float("nan"), float("nan")
    sad0 = dict(results).get(0, float("nan"))
    min_sad = min(v for _, v in results)
    best, best_sad = min(
        ((sh, v) for sh, v in results if v <= min_sad * 1.05),
        key=lambda t: (abs(t[0]), t[1]),
    )
    return best, best_sad, sad0


def report_eye_dumps(paths):
    import glob as _glob
    if os.path.isdir(paths):
        paths = sorted(_glob.glob(os.path.join(paths, "mc2vr_eye_*.bmp")))
    by_frame = {}
    for p in paths:
        m = re.search(r"mc2vr_eye_(left|right)_frame(\d+)\.bmp", p)
        if m:
            by_frame.setdefault(int(m.group(2)), {})[m.group(1)] = p
    pairs = [(f, d) for f, d in sorted(by_frame.items()) if "left" in d and "right" in d]
    if not pairs:
        return
    print(f"\n################ S2c-2 eye pair analysis ({len(pairs)} pairs) ################")
    for frame, d in pairs[:5]:
        try:
            lw, lh, lb, lrow = load_bmp(d["left"])
            rw, rh, rb, rrow = load_bmp(d["right"])
        except (ValueError, OSError) as e:
            print(f"  frame {frame}: load failed: {e}")
            continue
        if (lw, lh) != (rw, rh):
            print(f"  frame {frame}: size mismatch {lw}x{lh} vs {rw}x{rh}")
            continue
        sh, sad, sad0 = best_shift(gray_rows(lw, lh, lb, lrow),
                                   gray_rows(rw, rh, rb, rrow))
        # Convert to full-res pixel shift (downsample step = 4).
        print(f"  frame {frame}: {lw}x{lh} | best shift {sh * 4:+d}px "
              f"(downsampled units {sh:+d}), SAD {sad:.1f} vs shift0 {sad0:.1f} "
              f"| {'PARALLAX PRESENT' if sh != 0 and sad < sad0 * 0.95 else 'no measurable parallax'}")


def discover_eye_dumps(arg):
    import glob as _glob
    if arg:
        if os.path.isdir(arg):
            return arg
        return os.path.dirname(os.path.abspath(arg)) or "."
    return None


# ---- S2c stream dumps ----------------------------------------------------------


def parse_stream_dump(path):
    """Parse one mc2vr_stream_frame<N>.txt into a list of streams of dwords."""
    streams, cur, header = [], None, {}
    for line in open(path, encoding="utf-8", errors="replace"):
        line = line.strip()
        if line.startswith("#") or not line:
            continue
        if line.startswith("streams "):
            continue
        m = re.match(
            r"stream base=(\w+) arg2=(\w+) arg3=(\w+) cmds=(\d+) dwords=(\d+)"
            r" trunc=(\d)",
            line,
        )
        if m:
            cur = {
                "base": int(m.group(1), 16),
                "arg2": int(m.group(2), 16),
                "arg3": int(m.group(3), 16),
                "cmds": int(m.group(4)),
                "dwords": int(m.group(5)),
                "trunc": int(m.group(6)),
                "words": [],
            }
            streams.append(cur)
            continue
        if line.startswith("d") and cur is not None:
            cur["words"].extend(int(w, 16) for w in line.split()[1:])
    return streams


def decode_stream(s):
    """Split a dword list into commands using the opcode size table."""
    cmds, w, bad = [], 0, s["words"]
    while w < len(bad):
        op = bad[w]
        if op >= 27:
            cmds.append({"op": op, "raw": bad[w:], "runaway": True})
            break
        size = OP_SIZE[op]
        cmds.append({"op": op, "raw": bad[w : w + size]})
        if op == 0:
            break
        w += size
    return cmds


def report_stream_dumps(paths):
    for path in paths:
        print(
            f"\n################ stream dump: {os.path.basename(path)} ################"
        )
        streams = parse_stream_dump(path)
        print(f"streams: {len(streams)}")
        op_counts, op_samples, field_stats = {}, {}, {}
        for s in streams:
            for c in decode_stream(s):
                op = c["op"]
                op_counts[op] = op_counts.get(op, 0) + 1
                op_samples.setdefault(op, c["raw"])
                for i, name in enumerate(OP_FIELDS.get(op, [])):
                    key = (op, name)
                    if i + 1 < len(c["raw"]):
                        v = c["raw"][i + 1]
                        lo, hi, seen = field_stats.get(key, (v, v, set()))
                        seen.add(v)
                        field_stats[key] = (min(lo, v), max(hi, v), seen)

        print("\n== per-opcode census ==")
        for op in sorted(op_counts):
            name = OP_NAME[op] if op < 27 else "INVALID"
            sample = " ".join(f"{w:08x}" for w in op_samples[op][:7])
            print(f"  op {op:02x} x{op_counts[op]:<6} {name:<28} sample: {sample}")

        print("\n== payload field stats (op, field: min..max, distinct) ==")
        for op, name in sorted(field_stats):
            lo, hi, seen = field_stats[(op, name)]
            d = f"distinct={len(seen)}" if len(seen) <= 8 else f"distinct>8"
            print(f"  op {op:02x}.{name}: {lo:#010x}..{hi:#010x} ({d})")

        print("\n== per-stream command traces (first 12 streams, first 16 cmds) ==")
        for s in streams[:12]:
            cmds = decode_stream(s)
            trace = " ".join(f"{c['op']:02x}" for c in cmds[:16])
            print(
                f"  base={s['base']:#010x} cmds={s['cmds']:<5} "
                f"arg2={s['arg2']:#010x} arg3={s['arg3']:#010x} : {trace}..."
            )


def discover_stream_dumps(log_path, arg):
    if arg:
        if os.path.isdir(arg):
            return sorted(glob.glob(os.path.join(arg, "mc2vr_stream_frame*.txt")))
        return [arg]
    d = os.path.dirname(os.path.abspath(log_path))
    return sorted(glob.glob(os.path.join(d, "mc2vr_stream_frame*.txt")))


def main():
    path = (
        sys.argv[1]
        if len(sys.argv) > 1
        else (
            "/home/laylb/Games/mercenaries-2/drive_c/Program Files (x86)/EA Games/"
            "Mercenaries 2 World in Flames/mc2vr/mc2vr_carrier.log"
        )
    )
    if os.path.exists(path):
        report_view(path)

    stream_paths = discover_stream_dumps(
        path, sys.argv[2] if len(sys.argv) > 2 else None
    )
    if stream_paths:
        report_stream_dumps(stream_paths)

    eye_dir = discover_eye_dumps(sys.argv[2] if len(sys.argv) > 2 else None) or (
        os.path.dirname(os.path.abspath(path))
    )
    if os.path.isdir(eye_dir):
        report_eye_dumps(eye_dir)

    if not os.path.exists(path):
        print(f"(log {path} not found — stream dumps only)")
        return
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

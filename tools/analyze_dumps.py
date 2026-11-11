#!/usr/bin/env python3
# Measure the shift between the eyes in the carrier's eye image dumps
# (debug_eye_dump_frames=N: mc2vr_eye_left/right_frame<N>.bmp next to the log).
# Usage: analyze_dumps.py [dump directory or a file in it]
# (default: the mc2vr directory of launch.conf's GAME_DIR).
# Method and pitfalls (global offset, per-eye scale, a true far reference):
# docs/debugging.md.

import os
import struct
import sys



# ---- eye dumps (BMP pairs) --------------------------------------------


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


# ---- numpy full-resolution parallax (preferred path) --------------------------

MAX_SHIFT_PX = 64  # full-res pixel search window (matches the old +/-16 units @4px)


def gray_array(w, h, buf, row):
    """Full-resolution grayscale int32 array from 24-bit BMP BGR rows."""
    import numpy as np

    img = (
        np.frombuffer(buf, dtype=np.uint8).reshape(h, row)[:, : w * 3].astype(np.int32)
    )
    b, g, r = img[:, 0::3], img[:, 1::3], img[:, 2::3]
    return (b * 114 + g * 587 + r * 299) // 1000


def shift_sad(left, right, shifts):
    """Mean |left(x) - right(x+sh)| per candidate shift (full resolution).

    Sign convention: positive sh = right image content sits `sh` px to the
    RIGHT of the left image's (i.e. left camera = right pan)."""
    import numpy as np

    h, w = left.shape
    out = {}
    for s in shifts:
        if s >= 0:
            a = left[:, : w - s] if s else left
            b = right[:, s:] if s else right
        else:
            a = left[:, -s:]
            b = right[:, : w + s]
        if a.shape[1] == 0:
            continue
        out[s] = float(np.mean(np.abs(a.astype(np.int32) - b.astype(np.int32))))
    return out


def report_eye_dumps(paths):
    import glob as _glob

    try:
        import numpy  # noqa: F401 — presence selects the full-res path
    except ImportError:
        numpy = None
    if os.path.isdir(paths):
        paths = sorted(_glob.glob(os.path.join(paths, "mc2vr_eye_*.bmp")))
    by_frame = {}
    for p in paths:
        m = re.search(r"mc2vr_eye_(left|right)_frame(\d+)\.bmp", p)
        if m:
            by_frame.setdefault(int(m.group(2)), {})[m.group(1)] = p
    pairs = [
        (f, d) for f, d in sorted(by_frame.items()) if "left" in d and "right" in d
    ]
    if not pairs:
        return
    print(f"\n################ eye pair analysis ({len(pairs)} pairs) ################")
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
        if numpy is not None:
            # Full-resolution 1-px search (fp16-tonemapped dumps are big but
            # numpy makes the exhaustive window cheap).
            lg = gray_array(lw, lh, lb, lrow)
            rg = gray_array(rw, rh, rb, rrow)
            sads = shift_sad(lg, rg, range(-MAX_SHIFT_PX, MAX_SHIFT_PX + 1))
            sad0 = sads.get(0, float("nan"))
            min_sad = min(sads.values())
            sh, sad = min(
                ((s, v) for s, v in sads.items() if v <= min_sad * 1.05),
                key=lambda t: (abs(t[0]), t[1]),
            )
            unit = "px"
        else:
            sh, sad, sad0 = best_shift(
                gray_rows(lw, lh, lb, lrow), gray_rows(rw, rh, rb, rrow)
            )
            sh = sh * 4  # downsampled units -> full-res px
            unit = "px (4px grid)"
        verdict = (
            "PARALLAX PRESENT"
            if sh != 0 and sad < sad0 * 0.95
            else "no measurable parallax"
        )
        print(
            f"  frame {frame}: {lw}x{lh} | best shift {sh:+d}{unit}, "
            f"SAD {sad:.2f} vs shift0 {sad0:.2f} | {verdict}"
        )


def discover_eye_dumps(arg):
    import glob as _glob

    if arg:
        if os.path.isdir(arg):
            return arg
        return os.path.dirname(os.path.abspath(arg)) or "."
    return None


def default_dir():
    conf = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "launch.conf")
    try:
        with open(conf) as f:
            for line in f:
                if line.startswith("GAME_DIR="):
                    return os.path.join(line.split("=", 1)[1].strip().strip('"'), "mc2vr")
    except OSError:
        pass
    return "."


def main():
    eye_dir = discover_eye_dumps(sys.argv[1] if len(sys.argv) > 1 else None) or default_dir()
    if not os.path.isdir(eye_dir):
        print(f"no dump directory {eye_dir}")
        return
    report_eye_dumps(eye_dir)


if __name__ == "__main__":
    main()

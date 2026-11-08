#!/usr/bin/env python3
"""Recursive-descent tracer for SecuROM-mutated native code, read straight from the exe.

Usage: tools/securom_trace.py <exe> <start_va_hex> [--listing]

Follows jcc/jmp into the S*/.securom regions (and out-of-line junk detours), and treats
`push ret; jmp f` and `push ret; push f; ret` as calls to f that return to `ret`.
Prints the reach (instruction count, range, block count), the stops (ret / indirect jumps /
unmapped), the calls, and the absolute memory refs. Method: docs/reverse_engineering/securom_vm.md.
"""
import collections
import struct
import sys

from capstone import CS_ARCH_X86, CS_MODE_32, Cs
from capstone.x86 import X86_OP_IMM, X86_OP_MEM

PROT = ("Stext", "Sitext", ".securom")


def load(path):
    data = open(path, "rb").read()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    nsec = struct.unpack_from("<H", data, pe + 6)[0]
    opt = struct.unpack_from("<H", data, pe + 20)[0]
    secs = []
    for i in range(nsec):
        o = pe + 24 + opt + 40 * i
        name = data[o:o + 8].rstrip(b"\0").decode()
        vs, va, rs, ro = struct.unpack_from("<IIII", data, o + 8)
        secs.append((name, 0x400000 + va, max(vs, rs), ro, rs))
    return data, secs


def main():
    data, secs = load(sys.argv[1])
    start = int(sys.argv[2], 16)

    def sec(a):
        for n, va, sz, _, _ in secs:
            if va <= a < va + sz:
                return n

    def rd(a, n):
        for _, va, _, ro, rs in secs:
            if va <= a < va + rs:
                return data[ro + a - va:ro + a - va + n]

    def code_ptr(v):
        return sec(v) in (".text",) + PROT

    md = Cs(CS_ARCH_X86, CS_MODE_32)
    md.detail = True
    insns, prev = {}, {}
    work = [start]
    stops, calls = [], []

    def push_imm(a):
        p = prev.get(a)
        if p is not None and insns[p].mnemonic == "push" and insns[p].operands[0].type == X86_OP_IMM:
            return p, insns[p].operands[0].imm
        return None, None

    while work:
        a = work.pop()
        while a not in insns:
            b = rd(a, 16)
            i = next(md.disasm(b, a), None) if b else None
            if i is None:
                stops.append((a, "unmapped/bad"))
                break
            insns[a] = i
            m, nxt = i.mnemonic, a + i.size
            if m == "ret":
                # push ret; push f; ret  => call f, continue at ret
                p, f = push_imm(a)
                p2, r = push_imm(p) if p is not None else (None, None)
                if f is not None and r is not None and code_ptr(f) and code_ptr(r):
                    calls.append((a, f, "push-push-ret"))
                    work.append(r)
                else:
                    stops.append((a, "ret " + i.op_str))
                break
            if m == "jmp":
                op = i.operands[0]
                if op.type != X86_OP_IMM:
                    stops.append((a, "jmp " + i.op_str))
                    break
                t = op.imm
                p, r = push_imm(a)
                if r is not None and code_ptr(r):  # push ret; jmp f
                    calls.append((a, t, "push-jmp"))
                    work.append(r)
                    break
                # follow plain jumps (incl. junk detours into .text: they return here or end)
                prev[t] = a
                a = t
                continue
            if m.startswith("j") or m.startswith("loop"):
                t = i.operands[0].imm
                prev.setdefault(t, a)
                work.append(t)
            if m == "call":
                op = i.operands[0]
                calls.append((a, op.imm if op.type == X86_OP_IMM else i.op_str, "call"))
            prev[nxt] = a
            a = nxt

    addrs = sorted(insns)
    blocks = 1 + sum(1 for x, y in zip(addrs, addrs[1:]) if x + insns[x].size != y)
    print(f"insns {len(insns)}  range {addrs[0]:#x}..{addrs[-1]:#x}  contiguous blocks {blocks}")
    print("stops:", ", ".join(f"{a:#x} {s}" for a, s in stops))
    print("calls:")
    for a, t, k in calls:
        print(f"  {a:#x} {k:13} {t:#x} {sec(t)}" if isinstance(t, int) else f"  {a:#x} {k:13} {t}")
    mem = collections.defaultdict(list)
    for a, i in insns.items():
        for op in i.operands:
            if op.type == X86_OP_MEM and op.mem.base == 0 and op.mem.disp > 0x400000:
                mem[op.mem.disp].append(a)
            elif (op.type == X86_OP_IMM and i.mnemonic not in ("push", "call") and not i.mnemonic.startswith("j")
                  and 0x00BF5000 <= op.imm < 0x03772000):
                mem[op.imm].append(a)
    print("absolute refs:")
    for d in sorted(mem):
        print(f"  {d:#010x} {sec(d):9} x{len(mem[d])}  first {mem[d][0]:#x}")
    if "--listing" in sys.argv:
        last = None
        for a in addrs:
            if last is not None and a != last:
                print("  ----")
            print(f"{a:#010x}  {insns[a].mnemonic} {insns[a].op_str}")
            last = a + insns[a].size


if __name__ == "__main__":
    main()

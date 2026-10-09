# Model SecuROM push/jmp and push/push/ret call idioms inside the functions at
# runtime-resolved VM thunk targets (docs/reverse_engineering/vm_thunk_patches.md).
# FINDING (2026-10-03): only P2 (push ret; push callee; RET -> CALL override + fall-through) persists.
# P1 (push ret; jmp X) loses its fall-through override right after commit, which leaves the function
# falling into junk bytes (worse decompile). Health checks MUST run after commit, not before.
# APPLY=False -> everything is rolled back (dry run). Prints stats + pilot decompile.
import collections
import csv

from ghidra.app.decompiler import DecompInterface
from ghidra.program.model.address import AddressSet
from ghidra.program.model.listing import FlowOverride
from ghidra.program.model.symbol import RefType, SourceType

APPLY = False
MODEL_P1 = False  # `push ret; jmp X`: the fall-through override is cleared by Ghidra within ~1s of commit -> garbage decompile. Do NOT enable.
SKIP = set(
    [0x4F0FD0, 0x50DB90, 0x5FFEA0, 0x631DE0, 0x649EF0]
)  # closure failed in the dry run
PILOT_ONLY = None  # e.g. 0x518fa0 to restrict to one function
TLO, THI = 0x401000, 0xB04FFF
NEAR = 0x80  # continuation must sit within this many bytes after the idiom

fm = currentProgram.getFunctionManager()
lst = currentProgram.getListing()
refm = currentProgram.getReferenceManager()


def intext(v):
    return TLO <= v <= THI


def pimm(ins):
    if (
        ins is not None
        and ins.getMnemonicString() == "PUSH"
        and ins.getNumOperands() == 1
    ):
        sc = ins.getScalar(0)
        if sc is not None:
            return sc.getUnsignedValue()
    return None


rows = [
    r
    for r in csv.DictReader(
        open(
            "/home/laylb/Desktop/mc2vr/docs/reverse_engineering/data/vm_thunks_runtime.csv"
        )
    )
    if r["runtime_region"] == "text"
]
targets = sorted(set(int(r["runtime_target"], 16) for r in rows))
if PILOT_ONLY:
    targets = [PILOT_ONLY]


def closure(f):
    entry = f.getEntryPoint()
    S = AddressSet()
    work = [entry]
    seen = set()
    while work:
        a = work.pop()
        if a in seen:
            continue
        ins = lst.getInstructionAt(a)
        if ins is None:
            return None, a
        seen.add(a)
        S.add(ins.getMinAddress(), ins.getMaxAddress())
        ft = ins.getFallThrough()
        if ft is not None:
            work.append(ft)
        ftp = ins.getFlowType()
        if ftp.isCall() and not ftp.isJump():
            continue
        if ftp.isJump() or ftp.isConditional():
            for fl in ins.getFlows():
                o = fm.getFunctionAt(fl)
                if o is not None and not o.getEntryPoint().equals(entry):
                    continue
                if intext(fl.getOffset()):
                    work.append(fl)
    return S, None


def find_idioms(f):
    out = []
    for ins in lst.getInstructions(f.getBody(), True):
        mn = ins.getMnemonicString()
        prev = ins.getPrevious()
        if MODEL_P1 and mn == "JMP" and prev is not None:
            v = pimm(prev)
            if (
                v is not None
                and intext(v)
                and 0 < v - ins.getAddress().getOffset() < NEAR
                and ins.getFlowOverride()
                in (FlowOverride.NONE, FlowOverride.CALL_RETURN)
            ):
                out.append(("P1", ins, v, None))
        elif (
            mn == "RET"
            and prev is not None
            and prev.getPrevious() is not None
            and ins.getFlowOverride() == FlowOverride.NONE
        ):
            b = pimm(prev)
            a = pimm(prev.getPrevious())
            if (
                a is not None
                and b is not None
                and intext(a)
                and intext(b)
                and a != b
                and 0 < a - ins.getAddress().getOffset() < NEAR
            ):
                out.append(("P2", ins, a, b))
    return out


di = DecompInterface()
di.openProgram(currentProgram)


def decomp(f):
    r = di.decompileFunction(f, 60, monitor)
    if not r.decompileCompleted():
        return None
    return r.getDecompiledFunction().getC()


def bad(c):
    return c is None or "halt_baddata" in c or "Bad instruction" in c


stats = collections.Counter()
failed = []
changed = []
before = {}
tx = currentProgram.startTransaction("Model SecuROM push/jmp idioms")
try:
    for t in targets:
        if t in SKIP:
            continue
        f = fm.getFunctionAt(toAddr(t))
        if f is None:
            continue
        before[t] = decomp(f)
        done_any = False
        for _ in range(10):
            ids = find_idioms(f)
            if not ids:
                break
            for kind, ins, cont, callee in ids:
                ins.setFlowOverride(FlowOverride.CALL)
                ins.setFallThrough(toAddr(cont))
                if kind == "P2":
                    refm.addMemoryReference(
                        ins.getAddress(),
                        toAddr(callee),
                        RefType.UNCONDITIONAL_CALL,
                        SourceType.USER_DEFINED,
                        -1,
                    )
                if lst.getInstructionAt(toAddr(cont)) is None:
                    disassemble(toAddr(cont))
                stats[kind] += 1
                done_any = True
            S, miss = closure(f)
            if S is None:
                failed.append((hex(t), "closure missing instruction at %s" % miss))
                break
            f.setBody(S)
        if done_any:
            changed.append(t)
    # health check
    worse = []
    same = 0
    for t in changed:
        f = fm.getFunctionAt(toAddr(t))
        a = decomp(f)
        if bad(a) and not bad(before[t]):
            worse.append(hex(t))
        elif a == before[t]:
            same += 1
    print(
        "idioms modeled:",
        dict(stats),
        "functions changed:",
        len(changed),
        "closure failures:",
        len(failed),
    )
    print(
        "decompile became bad/failed:",
        worse[:10],
        len(worse),
        "| unchanged text despite change:",
        same,
    )
    for x in failed[:10]:
        print("FAIL", x)
    pil = None
    if pil is not None:
        print("---- pilot 0x518fa0 BEFORE ----")
        print(before.get(0x518FA0))
        print("---- pilot AFTER ----")
        print(decomp(pil))
    commit = APPLY and not worse and not failed
    print("committing:", commit)
    currentProgram.endTransaction(tx, commit)
except Exception as e:
    currentProgram.endTransaction(tx, False)
    print("ABORTED and rolled back:", e)
    raise
print("APPLY =", APPLY)

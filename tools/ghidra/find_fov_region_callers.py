# Locate the undefined plaintext region containing the fov consumer
# (0x00877190): scan .text for E8 rel32 calls whose target lands in
# [0x00876000, 0x0087A000), report call sites + whether a defined function
# contains the target, and dump the first 16 bytes at each distinct target.
# @category mc2vr
from ghidra.program.model.address import AddressSet

fm = currentProgram.getFunctionManager()
listing = currentProgram.getListing()
af = currentProgram.getAddressFactory()
space = currentProgram.getAddressFactory().getDefaultAddressSpace()

LO = 0x00876000
HI = 0x0087A000

def fn_at(addr):
    f = fm.getFunctionContaining(addr)
    return f.getName() + "@" + str(f.getEntryPoint()) if f else "(undefined)"

text = None
for b in currentProgram.getMemory().getBlocks():
    if b.getName() == ".text":
        text = b
if text is None:
    raise Exception("no .text")

start = text.getStart()
size = text.getSize()
from jpype import JArray, JByte
buf = JArray(JByte)([0] * size)
text.getBytes(start, buf)

hits = {}
n = len(buf)
i = 0
while i < n - 5:
    if (buf[i] & 0xFF) == 0xE8:
        rel = (buf[i+1] & 0xFF) | (buf[i+2] & 0xFF) << 8 | (buf[i+3] & 0xFF) << 16 | (buf[i+4] & 0xFF) << 24
        if rel >= 0x80000000:
            rel -= 0x100000000
        site = start.getOffset() + i
        tgt = site + 5 + rel
        if LO <= tgt < HI:
            hits.setdefault(tgt, []).append(site)
    i += 1

print("=== E8 call targets inside 0x%08X..0x%08X ===" % (LO, HI))
for tgt in sorted(hits):
    taddr = space.getAddress(tgt)
    print("target 0x%08X  calls=%d  fn=%s  sites: %s" % (
        tgt, len(hits[tgt]), fn_at(taddr),
        " ".join("0x%08X(%s)" % (s, fn_at(space.getAddress(s))) for s in sorted(hits[tgt])[:6])))

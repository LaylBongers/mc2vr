# Fast doc-ref fix: iterate comment addresses per type via getCommentAddressIterator
# (no full code-unit sweep). Idempotent.
from ghidra.program.model.listing import CodeUnit

REPL = [
    # plans/ zettelkasten migration (2026-10-09): docs/plans/* dissolved into atomic docs/ notes.
    ("docs/plans/stereo_improvements.md", "docs/camera.md"),
    ("docs/plans/frustum_cull.md", "docs/culling.md"),
    ("docs/plans/launcher.md", "docs/hooks.md"),
    ("docs/plans/hud.md", "docs/hud.md"),
    ("docs/plans/stereo_design.md", "docs/stereo.md"),
    # earlier migration (pre-plans names), for idempotency:
    ("docs/stereo_improvements_plan.md", "docs/camera.md"),
    ("docs/frustum_cull_plan.md", "docs/culling.md"),
    ("docs/launcher_plan.md", "docs/hooks.md"),
    ("docs/hud_plan.md", "docs/hud.md"),
    ("frustum_cull_plan.md", "docs/culling.md"),
    ("stereo_improvements_plan.md", "docs/camera.md"),
    ("launcher_plan.md", "docs/hooks.md"),
    ("hud_plan.md", "docs/hud.md"),
]

TYPES = [
    (CodeUnit.PLATE_COMMENT, "plate"),
    (CodeUnit.PRE_COMMENT, "pre"),
    (CodeUnit.POST_COMMENT, "post"),
    (CodeUnit.EOL_COMMENT, "eol"),
    (CodeUnit.REPEATABLE_COMMENT, "repeatable"),
]

changed = 0
listing = currentProgram.getListing()
mem = currentProgram.getMemory()
tx = currentProgram.startTransaction("fix plan doc refs (fast)")
try:
    for ctype, cname in TYPES:
        addr_it = listing.getCommentAddressIterator(ctype, mem, True)
        n = 0
        while addr_it.hasNext():
            addr = addr_it.next()
            cu = listing.getCodeUnitContaining(addr)
            if cu is None:
                continue
            text = cu.getComment(ctype)
            if text is None or ".md" not in text:
                continue
            new = text
            for a, b in REPL:
                new = new.replace(a, b)
            if new != text:
                cu.setComment(ctype, new)
                changed += 1
                print("fixed %s (%s)" % (addr, cname))
            n += 1
        print("scanned %d %s comments" % (n, cname))
finally:
    currentProgram.endTransaction(tx, True)
print("changed comments:", changed)

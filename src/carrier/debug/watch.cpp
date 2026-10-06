// Hardware-watchpoint tracer — see watch.hpp for the design and the RE
// questions this answers. Debug-only module, default off.
//
// Wine lessons (live run 2026-10-06, the load-into-gameplay crash):
//   - Arming via suspend + SetThreadContext(CONTEXT_DEBUG_REGISTERS) WORKS
//     under Wine (DR7 applied and verified back on all 27 threads).
//   - But the EXCEPTION_SINGLE_STEP that a data breakpoint raises arrives
//     with Dr6 ABSENT from the exception CONTEXT (Wine stores debug regs
//     server-side; they are not part of the signal frame). A handler that
//     requires Dr6 sees 0, passes the exception on, and the game dies on
//     the very first watched access — exactly the observed crash.
//   - Ownership is therefore decided WITHOUT Dr6: a #DB with EFlags.TF set
//     is someone else's trap-flag single-step (never swallowed); TF clear
//     while our watchpoints are armed is our data breakpoint.
//   - Slot attribution: use Dr6 when the delivered context has it (native
//     Windows fills it); otherwise record as slot-unknown ("?") — run
//     single-target (debug_watch=quat) for exact attribution under Wine.
//   - View selection: the FIRST type-2 view is a loading template with dead
//     camera fields (quat=0). Select the first type-2 view with a live
//     quaternion instead (fallback: first type-2 after 30s, logged), or pin
//     explicitly with debug_watch_view.

#include "watch.hpp"

#include <windows.h>
#include <tlhelp32.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "game_addresses.h"
#include "log.hpp"

namespace mc2vr::watch {

namespace {

// ---- conf state --------------------------------------------------------------
bool g_enabled = false;
bool g_watch_reads = true;          // full (RW=3) vs write-only (RW=1)
uint32_t g_pin_view = UINT32_MAX;   // conf-pinned view idx; default = first live type-2
volatile LONG g_detail_left = 30;   // per-unique-EIP detail lines

constexpr uint32_t TARGET_MAX = 4;  // DR0-3
constexpr uint32_t SLOT_UNKNOWN = 4;

struct TargetSpec {
    char name[24];       // "quat", "stagingpos", ... or "addr:XXXXXXXX"
    uint32_t entry_off;  // ViewEntry offset (or offset in the staged 0x30 block
                          // when is_staging); UINT32_MAX = fixed_addr below
    bool is_staging;      // resolve against this+0xc2110+idx*0x30 instead of the entry
    uint32_t fixed_addr;
    uint32_t addr;       // resolved at arm time
    bool valid;           // aligned + committed
};
TargetSpec g_specs[TARGET_MAX];
uint32_t g_spec_count = 0;

// ---- chosen view --------------------------------------------------------------
// Written by on_view (main thread), consumed by the poller (no DRs set on it,
// so its reads of these fields never trap).
volatile LONG g_have_view = 0;
uint32_t g_view_idx = 0;
uintptr_t g_view_entry = 0;

// View candidates seen by on_view (main thread), consumed by poll().
// Selection order: pinned idx > most-walked t3-live view (after a 10s settle)
// > first type-2 with nonzero quat > first type-2 after 30s.
uintptr_t g_frame_ctx = 0; // EBX through the walk; staging-block base
struct T3View {
    uint32_t idx;
    uintptr_t entry;
    uint32_t walks;
};
constexpr uint32_t T3_MAX = 24;
T3View g_t3[T3_MAX];
uint32_t g_t3_count = 0;
DWORD g_first_t3_tick = 0;
volatile LONG g_have_first_t2 = 0;
uint32_t g_first_t2_idx = 0;
uintptr_t g_first_t2_entry = 0;
DWORD g_first_t2_tick = 0;
volatile LONG g_have_live_t2 = 0; // fallback: first type-2 with nonzero quat
uint32_t g_live_t2_idx = 0;
uintptr_t g_live_t2_entry = 0;

// ---- arm state -----------------------------------------------------------------
volatile LONG g_armed = 0; // 0 = pending, 1 = armed, 2 = failed/stop retrying
uint32_t g_armed_threads = 0;
uint32_t g_armed_tids[64];
uint32_t g_armed_tid_count = 0;
uint32_t g_dr7 = 0;

// ---- hit aggregation -----------------------------------------------------------
constexpr uint32_t HIT_MAX = 256;
struct Hit {
    uint32_t eip;
    uint32_t slot; // 0..3, or SLOT_UNKNOWN when Dr6 is unobservable
    uint32_t count;
};
Hit g_hits[HIT_MAX];
uint32_t g_hit_count = 0;
uint64_t g_trap_total = 0;
volatile LONG g_lock = 0; // spinlock guarding the hit table

uintptr_t g_carrier_lo = 0, g_carrier_hi = 0; // our module range ("carrier-self")

const char *slot_name(uint32_t slot)
{
    return slot < g_spec_count ? g_specs[slot].name : "?";
}

// ---- address classification ---------------------------------------------------
const char *region_of(uint32_t a)
{
    if (a >= 0x00401000u && a <= 0x00b04fffu) return "text";
    if (a >= 0x00b05000u && a <= 0x00bf4fffu) return "rdata";
    if (a >= 0x00bf5000u && a <= 0x019f8fffu) return "data";
    if (a >= 0x019f9000u && a <= 0x01a47fffu) return "extdata";
    if (a >= 0x01a48000u && a <= 0x02463fffu) return "S* (VM sections)";
    if (a >= 0x02464000u && a <= 0x03771f0fu) return ".securom";
    return "other";
}

// Known view/camera functions (ranges from the Ghidra project). Hits outside
// these still report region + EIP; the surrounding-bytes detail line lets us
// locate them in Ghidra afterwards.
struct Fn {
    uint32_t lo, hi;
    const char *name;
};
const Fn k_fns[] = {
    {0x00488e40u, 0x00488e40u + 919, "ViewEntry_Activate"},
    {0x004891e0u, 0x004891e0u + 1486, "ViewManager_Update"},
    {0x00489e50u, 0x00489e7bu, "FUN_00489e50 (view/cam)"},
    {0x00489ea0u, 0x00489ea0u + 138, "ViewManager_RegisterTasks"},
    {0x0048a3b0u, 0x0048a420u, "FUN_0048a3b0 (view/cam)"},
    {0x0048a7b0u, 0x0048a89eu, "ViewEntry_PropagateMatrices"},
    {0x0048a8f0u, 0x0048ad41u, "ViewEntry_MatrixFromGlobalCam"},
    // VM-virtualized (semantics opaque) — an EIP here is itself a finding.
    {0x0048bf00u, 0x0048c300u, "PoseStore VM pose getter (virtualized fn)"},
    {0x0048d160u, 0x0048d160u + 741, "ViewManager_RefreshViewPoses"},
    {0x0048e620u, 0x0048f800u, "RenderQueue_SubmitWorldPackets (incl. mutated tail)"},
    {0x004d2a50u, 0x004d2c53u, "FUN_004d2a50 (view/cam)"},
};

const char *fn_of(uint32_t a)
{
    for (const auto &f : k_fns) {
        if (a >= f.lo && a <= f.hi) return f.name;
    }
    return nullptr;
}

bool carrier_self(uint32_t a)
{
    return g_carrier_lo != 0 && a >= g_carrier_lo && a < g_carrier_hi;
}

// ---- VEH -----------------------------------------------------------------------
// Reads bytes for the detail line; clamps to the region end so a straddling
// read cannot fault inside the handler (nested exceptions in a VEH are fatal).
bool read_bytes(uintptr_t addr, uint8_t *out, uint32_t *len)
{
    uint32_t done = 0;
    while (done < *len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(addr + done), &mbi, sizeof(mbi)) == 0) break;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) break;
        const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        uint32_t n = (uint32_t)(end - (addr + done));
        if (n > *len - done) n = *len - done;
        memcpy(out + done, (void *)(addr + done), n);
        done += n;
    }
    *len = done;
    return done > 0;
}

void lock_acquire()
{
    while (InterlockedCompareExchange(&g_lock, 1, 0) != 0) {
        Sleep(0);
    }
}

void lock_release()
{
    InterlockedExchange(&g_lock, 0);
}

// slot = DR index 0..3 (or SLOT_UNKNOWN); eip = the instruction AFTER the
// access (x86 data-BP traps report the following instruction). ctx = the
// trap context (registers: EAX/ECX often carry struct pointers — e.g.
// Pose_Copy's DEST/SRC pair; [ESP]/[ESP+4] are likely return addresses).
void record_hit(uint32_t slot, uint32_t eip, const CONTEXT *ctx)
{
    bool new_site = false;
    lock_acquire();
    for (uint32_t i = 0; i < g_hit_count; i++) {
        if (g_hits[i].slot == slot && g_hits[i].eip == eip) {
            g_hits[i].count++;
            lock_release();
            return;
        }
    }
    if (g_hit_count < HIT_MAX) {
        g_hits[g_hit_count++] = {eip, slot, 1};
        new_site = true;
    }
    lock_release();
    g_trap_total++;

    if (new_site && g_detail_left > 0) {
        // The detail line is the analysis payload: where + what code sits
        // around the access + live registers. Budgeted so a hot field cannot
        // flood the log.
        uint8_t before[16] = {}, after[8] = {};
        uint32_t nb = sizeof(before), na = sizeof(after);
        const bool have_before = read_bytes((uintptr_t)eip - 16, before, &nb);
        const bool have_after = read_bytes((uintptr_t)eip, after, &na);
        char hex[3 * 24 + 1];
        char *p = hex;
        for (uint32_t i = 0; i < (have_before ? nb : 0) && i < 16; i++) p += sprintf(p, "%02X", before[i]);
        if (!have_before) p += sprintf(p, "??");
        *p++ = '|';
        for (uint32_t i = 0; i < (have_after ? na : 0) && i < 8; i++) p += sprintf(p, "%02X", after[i]);
        if (!have_after) p += sprintf(p, "??");

        // Live registers + likely return addresses (stack slots, guarded).
        uint32_t ret0 = 0, ret1 = 0;
        uint32_t rn0 = 4, rn1 = 4;
        const bool have_ret0 = ctx ? read_bytes((uintptr_t)ctx->Esp, (uint8_t *)&ret0, &rn0) && rn0 == 4 : false;
        const bool have_ret1 = ctx ? read_bytes((uintptr_t)ctx->Esp + 4, (uint8_t *)&ret1, &rn1) && rn1 == 4 : false;

        InterlockedExchange(&g_detail_left, g_detail_left - 1);
        if (carrier_self(eip)) {
            MC2VR_LOG("watch hit #%u: %s eip=%08X carrier-self (our instrumentation) "
                      "bytes[-16..+8]=%s",
                      (unsigned)g_detail_left, slot_name(slot), eip, hex);
        } else {
            const char *fn = fn_of(eip);
            MC2VR_LOG("watch hit #%u: %s eip=%08X region=%s %s bytes[-16..+8]=%s "
                      "eax=%08X ecx=%08X edx=%08X esi=%08X edi=%08X ret?=%s[%08X %s] %s[%08X %s]%s",
                      (unsigned)g_detail_left, slot_name(slot), eip,
                      region_of(eip), fn ? fn : "(no known fn; see bytes)", hex,
                      ctx ? (uint32_t)ctx->Eax : 0, ctx ? (uint32_t)ctx->Ecx : 0,
                      ctx ? (uint32_t)ctx->Edx : 0, ctx ? (uint32_t)ctx->Esi : 0,
                      ctx ? (uint32_t)ctx->Edi : 0,
                      have_ret0 ? "" : "-", ret0,
                      have_ret0 ? region_of(ret0) : "",
                      have_ret1 ? "" : "-", ret1,
                      have_ret1 ? region_of(ret1) : "",
                      slot == SLOT_UNKNOWN ? " (field=? — Dr6 not observable under "
                                            "Wine; run single-target for attribution)"
                                           : "");
        }
    }
}

LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *c = ep->ContextRecord;
    DWORD dr6 = (DWORD)c->Dr6;

    if ((dr6 & 0xF) == 0) {
        // No debug-register cause in the delivered context. Under Wine this
        // is ALWAYS the case (Dr6 lives server-side, not in the signal
        // frame — the load-crash lesson). Decide by what a data-BP #DB is
        // NOT: a trap-flag single step (delivered with TF still set).
        if (c->EFlags & 0x100) {
            return EXCEPTION_CONTINUE_SEARCH; // someone else's TF stepping
        }
        if (InterlockedCompareExchange(&g_armed, 0, 0) == 0) {
            return EXCEPTION_CONTINUE_SEARCH; // not armed, not ours
        }
        // Armed (or arming — g_armed is set BEFORE the first thread gets a
        // DR, so a trap can legally land mid-sweep) + TF clear: our data
        // breakpoint (dr6 stays 0 → slot unknown).
        record_hit(SLOT_UNKNOWN, (uint32_t)c->Eip, c);
        return EXCEPTION_CONTINUE_EXECUTION;
    }

    const uint32_t eip = (uint32_t)c->Eip;
    c->Dr6 = 0;
    for (uint32_t slot = 0; slot < TARGET_MAX; slot++) {
        if (dr6 & (1u << slot)) {
            record_hit(slot, eip, c);
        }
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}

PVOID g_veh = nullptr;

// ---- arm sweep (poller thread) ---------------------------------------------------
// User mode cannot `mov dr`; the sanctioned route is SuspendThread +
// SetThreadContext(CONTEXT_DEBUG_REGISTERS) per thread. Suspends are
// microsecond-scale and happen once. All logging happens AFTER resumes, so a
// thread stopped inside the logger never deadlocks us.
//
// CONTEXT flags: CONTEXT_DEBUG_REGISTERS (0x10) is the whole Dr0-Dr7 block
// on Wine — live-proven twice (DR7=FFFF07FF applied AND read back with these
// flags alone). Adding the native-Windows Dr6/Dr7 bits (0x20|0x40) makes
// Wine's SetThreadContext fail on EVERY thread (live run 2026-10-06: "27
// failed") — do not add them back for a Wine target.
constexpr DWORD DR_CONTEXT_FLAGS = CONTEXT_DEBUG_REGISTERS | CONTEXT_i486;

uint32_t g_pre_existing_dr = 0; // threads that already had DR0-7 in use before us

bool arm_one_thread(HANDLE h, uint32_t dr7)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = DR_CONTEXT_FLAGS;
    if (!GetThreadContext(h, &ctx)) return false;
    if (ctx.Dr0 || ctx.Dr1 || ctx.Dr2 || ctx.Dr3 || ctx.Dr7) {
        // Someone (SecuROM?) already used debug registers on this thread —
        // a live answer to the doc's open DRx question, and our arm just
        // overwrote it (only DR0-3/DR7; DR6 is pure status).
        g_pre_existing_dr++;
    }
    ctx.Dr0 = g_specs[0].valid ? g_specs[0].addr : 0;
    ctx.Dr1 = g_specs[1].valid ? g_specs[1].addr : 0;
    ctx.Dr2 = g_specs[2].valid ? g_specs[2].addr : 0;
    ctx.Dr3 = g_specs[3].valid ? g_specs[3].addr : 0;
    ctx.Dr6 = 0;
    ctx.Dr7 = dr7;
    return SetThreadContext(h, &ctx) != 0;
}

void sweep_threads(uint32_t dr7)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        MC2VR_LOG("watch: CreateToolhelp32Snapshot failed (%lu) — arm aborted",
                  GetLastError());
        return;
    }

    const DWORD self_tid = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    uint32_t ok = 0, failed = 0, game_suspended = 0;
    if (dr7 == 0) g_armed_tid_count = 0;
    THREADENTRY32 te = {};
    te.dwSize = sizeof(te);
    if (Thread32First(snap, &te)) {
        do {
            if (te.th32OwnerProcessID != pid) continue;
            if (te.th32ThreadID == self_tid) continue;

            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_SET_CONTEXT | THREAD_QUERY_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h) {
                failed++;
                continue;
            }
            const DWORD prev = SuspendThread(h);
            if (prev == (DWORD)-1) {
                failed++;
            } else {
                // prev >= 1 = the game itself had it suspended; our resume
                // below only undoes OUR increment, so its suspension survives.
                if (prev >= 1) game_suspended++;
                if (arm_one_thread(h, dr7)) {
                    if (dr7 != 0 && g_armed_tid_count < 64) {
                        g_armed_tids[g_armed_tid_count++] = te.th32ThreadID;
                    }
                    ok++;
                } else {
                    failed++;
                }
                ResumeThread(h);
            }
            CloseHandle(h);
        } while (Thread32Next(snap, &te));
    }
    CloseHandle(snap);

    if (dr7 == 0) {
        MC2VR_LOG("watch: DISARMED (DR0-7 cleared on %u thread(s), %u failed)",
                  ok, failed);
        return;
    }
    MC2VR_LOG("watch: ARMED DR0-3 on %u thread(s) (%u failed, %u were already "
              "game-suspended), dr7=%08X mode=%s targets:",
              ok, failed, game_suspended, dr7,
              g_watch_reads ? "read+write" : "write-only");
    for (uint32_t i = 0; i < g_spec_count; i++) {
        MC2VR_LOG("watch:   DR%u %-8s addr=%08X (%s)", i, g_specs[i].name,
                  g_specs[i].addr, g_specs[i].valid ? "armed" : "SKIPPED");
    }
    if (failed > 0) {
        MC2VR_LOG("watch: NOTE %u thread(s) could not be armed (SetThreadContext of "
                  "debug regs can fail for some thread kinds — hits from those "
                  "threads will be missing)", failed);
    }
    g_armed_threads = ok;
}

void arm_sweep()
{
    // Resolve addresses first (no threads suspended while doing this).
    for (uint32_t i = 0; i < g_spec_count; i++) {
        TargetSpec &s = g_specs[i];
        if (s.is_staging) {
            if (g_frame_ctx == 0) {
                MC2VR_LOG("watch: target %s needs the frame-ctx pointer — not seen, "
                          "skipped", s.name);
                s.valid = false;
                continue;
            }
            s.addr = (uint32_t)(g_frame_ctx + MC2_VIEW_STAGING_OFF +
                                (uintptr_t)g_view_idx * MC2_VIEW_STAGING_STRIDE +
                                s.entry_off);
        } else if (s.entry_off != UINT32_MAX) {
            s.addr = (uint32_t)(g_view_entry + s.entry_off);
        } else {
            s.addr = s.fixed_addr;
        }
        s.valid = false;
        if (s.addr == 0) continue;
        if (s.addr & 3) {
            MC2VR_LOG("watch: target %s addr %08X not 4-aligned — skipped (DR LEN=4 "
                      "requires alignment)", s.name, s.addr);
            continue;
        }
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(uintptr_t)s.addr, &mbi, sizeof(mbi)) == 0 ||
            mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            MC2VR_LOG("watch: target %s addr %08X not committed — skipped", s.name, s.addr);
            continue;
        }
        s.valid = true;
    }

    uint32_t dr7 = 0x700; // LE|GE (recommended-1 on P6+) | reserved bit 10
    uint32_t valid = 0;
    for (uint32_t i = 0; i < g_spec_count; i++) {
        if (!g_specs[i].valid) continue;
        // DR7 per slot i: L/G enables at bits 2i/2i+1, RW at 16+4i, LEN at 18+4i.
        // RW: 01 = write only, 11 = read/write. LEN 11 = 4 bytes.
        dr7 |= (1u << (2 * i)) | (1u << (2 * i + 1)) |
               ((g_watch_reads ? 3u : 1u) << (16 + 4 * i)) | (3u << (18 + 4 * i));
        valid++;
    }
    if (valid == 0) {
        MC2VR_LOG("watch: no valid targets — arm aborted");
        InterlockedExchange(&g_armed, 2); // don't retry forever
        return;
    }
    g_dr7 = dr7;

    // CRITICAL ORDERING (live-run lesson 2026-10-06, the second load crash):
    // the ownership gate in the VEH is g_armed. Set it BEFORE the sweep —
    // threads resume with live DRs mid-sweep and the watched field is being
    // written at exactly this moment (the liveness heuristic that triggered
    // arming means the writer is active), so the first trap can land while
    // the sweep is still running.
    InterlockedExchange(&g_armed, 1);

    sweep_threads(dr7);
    if (g_pre_existing_dr > 0) {
        MC2VR_LOG("watch: NOTE %u thread(s) had debug registers in use BEFORE our "
                  "arm (SecuROM DRx usage? we overwrote them)", g_pre_existing_dr);
    }
    InterlockedExchange(&g_armed, g_armed_threads > 0 ? 1 : 2);
}

// ---- window report (poller thread; no DRs set on it) ------------------------------
void verify_and_report()
{
    // DR verification: did anyone (SecuROM?) clobber our watchpoints? The doc
    // flags DRx as untested territory for the inert-but-present anti-debug.
    uint32_t clobbered = 0;
    for (uint32_t i = 0; i < g_armed_tid_count; i++) {
        HANDLE h = OpenThread(THREAD_GET_CONTEXT, FALSE, g_armed_tids[i]);
        if (!h) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = DR_CONTEXT_FLAGS;
        if (GetThreadContext(h, &ctx) && (ctx.Dr7 != g_dr7 || ctx.Dr0 != g_specs[0].addr)) {
            clobbered++;
        }
        CloseHandle(h);
    }
    if (clobbered > 0) {
        MC2VR_LOG("watch: DR CLOBBERED on %u armed thread(s) — something rewrote the "
                  "debug registers (SecuROM DRx use?); hits after that point are "
                  "unreliable", clobbered);
    }

    // Value snapshot — safe from this thread (per-thread DRs: the poller has none).
    for (uint32_t i = 0; i < g_spec_count; i++) {
        if (!g_specs[i].valid) continue;
        const uint32_t raw = *(const uint32_t *)(uintptr_t)g_specs[i].addr;
        float f;
        memcpy(&f, &raw, 4);
        MC2VR_LOG("watch: value %s = %08X (as float %g)", g_specs[i].name, raw, f);
    }
}

} // namespace

// ---- public API -----------------------------------------------------------------

bool set_targets(const char *value)
{
    g_spec_count = 0;
    memset(g_specs, 0, sizeof(g_specs));

    struct Named {
        const char *name;
        uint32_t off;
        bool staging; // off is within the staged 0x30 block, not the ViewEntry
    };
    static const Named named[] = {
        {"slot0", (uint32_t)MC2_VIEW_OFF_SLOT0, false},
        {"slot0v", (uint32_t)MC2_VIEW_OFF_SLOT0V, false},
        {"slotdir", (uint32_t)MC2_VIEW_OFF_SLOTDIR, false},
        {"slotparams", (uint32_t)MC2_VIEW_OFF_SLOTPARAMS, false},
        {"near", (uint32_t)MC2_VIEW_OFF_NEAR, false},
        {"fov", (uint32_t)MC2_VIEW_OFF_FOVCOS, false},
        {"fovsin", (uint32_t)MC2_VIEW_OFF_FOVSIN, false},
        {"dir670", (uint32_t)MC2_VIEW_OFF_DIR670, false},
        {"posprev", (uint32_t)MC2_VIEW_OFF_POS_PREV, false},
        {"pos", (uint32_t)MC2_VIEW_OFF_POS_CUR, false},
        {"serial", (uint32_t)MC2_VIEW_OFF_SERIAL, false},
        {"quat", (uint32_t)MC2_VIEW_OFF_QUAT, false},
        {"camdata", (uint32_t)MC2_VIEW_OFF_CAMDATA, false},
        {"flags808", (uint32_t)MC2_VIEW_OFF_FLAGS808, false},
        // Staged camera block (VM consumer input) for the chosen view:
        // {pos3, serial@+0xc, quat@+0x10} at ctx+0xc2110+idx*0x30.
        {"stagingpos", 0, true},
        {"stagingserial", (uint32_t)MC2_VIEW_STAGING_SERIAL_OFF, true},
        {"stagingquat", (uint32_t)MC2_VIEW_STAGING_QUAT_OFF, true},
    };

    char buf[128];
    if (strlen(value) >= sizeof(buf)) {
        MC2VR_LOG("conf: debug_watch value too long");
        return false;
    }
    strcpy(buf, value);

    char *save = nullptr;
    for (char *tok = strtok_r(buf, "+", &save); tok; tok = strtok_r(nullptr, "+", &save)) {
        if (g_spec_count >= TARGET_MAX) {
            MC2VR_LOG("conf: debug_watch supports at most %u targets — extra '%s' dropped",
                      TARGET_MAX, tok);
            break;
        }
        TargetSpec &s = g_specs[g_spec_count];
        bool found = false;
        for (const auto &n : named) {
            if (strcmp(tok, n.name) == 0) {
                snprintf(s.name, sizeof(s.name), "%s", n.name);
                s.entry_off = n.off;
                s.is_staging = n.staging;
                s.fixed_addr = 0;
                found = true;
                break;
            }
        }
        if (!found && strncmp(tok, "addr:", 5) == 0) {
            const char *hex = tok + 5;
            char *end = nullptr;
            const unsigned long a = strtoul(hex, &end, 16);
            if (end != hex && *end == '\0' && a != 0) {
                snprintf(s.name, sizeof(s.name), "addr:%08lX", a);
                s.entry_off = UINT32_MAX;
                s.is_staging = false;
                s.fixed_addr = (uint32_t)a;
                found = true;
            }
        }
        if (!found) {
            MC2VR_LOG("conf: debug_watch target '%s' not recognized (named ViewEntry "
                      "field or addr:<hex>) — feature disabled", tok);
            g_spec_count = 0;
            return false;
        }
        g_spec_count++;
    }

    if (g_spec_count == 0) {
        MC2VR_LOG("conf: debug_watch has no targets — feature disabled");
        return false;
    }
    g_enabled = true;
    return true;
}

bool set_mode(const char *value)
{
    if (strcmp(value, "full") == 0) { g_watch_reads = true; return true; }
    if (strcmp(value, "write") == 0) { g_watch_reads = false; return true; }
    return false;
}

void set_view_index(uint32_t idx)
{
    g_pin_view = idx;
}

void set_detail_hits(uint32_t n)
{
    g_detail_left = n;
}

void on_view(uint32_t idx, uint32_t type, const uint8_t *entry, uintptr_t ebx_this)
{
    if (!g_enabled) return;

    g_frame_ctx = ebx_this; // kept in EBX through the walk (function plate)

    if (type != MC2_VIEW_TYPE2) return; // only type-2 views carry camera data we want

    if (InterlockedCompareExchange(&g_have_view, 0, 0) != 0) return; // chosen already

    if (InterlockedCompareExchange(&g_have_first_t2, 0, 0) == 0) {
        g_first_t2_idx = idx;
        g_first_t2_entry = (uintptr_t)entry;
        g_first_t2_tick = GetTickCount();
        InterlockedExchange(&g_have_first_t2, 1);
    }

    // Primary candidate set: views whose companion liveness byte is 01 —
    // the Ghidra-plate "camera data populates when this flips 0->1" marker
    // (live-verified: loading templates stay 00, the persistent world
    // views show 01). Count walks so the MOST-SUBMITTED live view wins.
    const uint8_t t3 = *(const uint8_t *)(MC2_VIEW_TABLE3 + (uintptr_t)idx *
                                              MC2_VIEW_TABLE3_STRIDE + MC2_VIEW_T3_OFF);
    if (t3 == MC2_VIEW_T3_LIVE) {
        uint32_t i = 0;
        for (; i < g_t3_count; i++) {
            if (g_t3[i].idx == idx) break;
        }
        if (i < g_t3_count) {
            g_t3[i].walks++;
        } else if (g_t3_count < T3_MAX) {
            g_t3[g_t3_count++] = {idx, (uintptr_t)entry, 1};
            if (g_first_t3_tick == 0) g_first_t3_tick = GetTickCount();
            char list[24 * 12];
            int n = 0;
            for (uint32_t j = 0; j < g_t3_count && n < (int)sizeof(list) - 16; j++) {
                n += snprintf(list + n, sizeof(list) - n, " %u", g_t3[j].idx);
            }
            MC2VR_LOG("watch: live (t3=1) type-2 view idx=%u entry=%p (live so far:%s)",
                      idx, (const void *)entry, list);
        }
    }

    // Fallback candidate: first type-2 with a nonzero quaternion (a loading
    // phase can have transient views with live quat but t3=00 — see the
    // 2026-10-06 idx23 lesson; selection prefers t3=1 views whenever present).
    if (*(const uint32_t *)(entry + MC2_VIEW_OFF_QUAT) != 0) {
        g_live_t2_idx = idx;
        g_live_t2_entry = (uintptr_t)entry;
        InterlockedExchange(&g_have_live_t2, 1);
    }

    // A pinned view is chosen the moment it appears (liveness not required —
    // the pin is an explicit instruction).
    if (g_pin_view != UINT32_MAX && idx == g_pin_view) {
        g_view_idx = idx;
        g_view_entry = (uintptr_t)entry;
        InterlockedExchange(&g_have_view, 1);
        MC2VR_LOG("watch: pinned view idx=%u entry=%p — will arm on the next "
                  "poller tick", idx, (const void *)entry);
    }
}

void poll()
{
    if (!g_enabled) return;
    if (InterlockedCompareExchange(&g_armed, 0, 0) != 0) return; // armed or failed

    // Selection (no pin), in order of confidence:
    //   1. Most-walked t3-live view after a 10s settle (lets walk counts
    //      accumulate so transient/special views lose to the main one).
    //   2. First type-2 with a live quat (loading phases may have no t3=1
    //      views yet; known to be able to pick transient views — logged).
    //   3. First type-2 after 30s (last resort).
    if (InterlockedCompareExchange(&g_have_view, 0, 0) == 0) {
        bool chose = false;
        if (g_t3_count > 0 && g_first_t3_tick != 0 &&
            GetTickCount() - g_first_t3_tick >= 10000) {
            uint32_t best = 0;
            for (uint32_t i = 1; i < g_t3_count; i++) {
                if (g_t3[i].walks > g_t3[best].walks) best = i;
            }
            g_view_idx = g_t3[best].idx;
            g_view_entry = g_t3[best].entry;
            InterlockedExchange(&g_have_view, 1);
            chose = true;
            char list[24 * 6];
            int n = 0;
            for (uint32_t i = 0; i < g_t3_count && n < (int)sizeof(list) - 24; i++) {
                n += snprintf(list + n, sizeof(list) - n, " idx%u:%u", g_t3[i].idx,
                              g_t3[i].walks);
            }
            MC2VR_LOG("watch: chose most-walked live (t3=1) view idx=%u entry=%p — "
                      "will arm on the next tick (live views:%s)", g_view_idx,
                      (const void *)g_view_entry, list);
        }
        if (!chose && InterlockedCompareExchange(&g_have_live_t2, 0, 0) != 0 &&
            GetTickCount() - g_first_t2_tick >= 30000) {
            // Only after 30s: give t3=1 views every chance to appear first.
            g_view_idx = g_live_t2_idx;
            g_view_entry = g_live_t2_entry;
            InterlockedExchange(&g_have_view, 1);
            chose = true;
            MC2VR_LOG("watch: no t3-live view after 30s — falling back to first "
                      "quat-live type-2 idx=%u (may be transient; pin a real one "
                      "with debug_watch_view)", g_view_idx);
        }
        if (!chose && InterlockedCompareExchange(&g_have_first_t2, 0, 0) != 0 &&
            GetTickCount() - g_first_t2_tick > 60000) {
            g_view_idx = g_first_t2_idx;
            g_view_entry = g_first_t2_entry;
            InterlockedExchange(&g_have_view, 1);
            MC2VR_LOG("watch: no live view after 60s — falling back to first "
                      "type-2 idx=%u (dead camera fields expected; pin a real one "
                      "with debug_watch_view)", g_view_idx);
        }
        if (InterlockedCompareExchange(&g_have_view, 0, 0) == 0) return;
    }

    // One-time module range for "carrier-self" classification.
    if (g_carrier_lo == 0) {
        HMODULE self = nullptr;
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCWSTR)&poll, &self) && self) {
            g_carrier_lo = (uintptr_t)self;
            IMAGE_NT_HEADERS32 nt = {};
            IMAGE_DOS_HEADER dos = {};
            memcpy(&dos, (void *)g_carrier_lo, sizeof(dos));
            memcpy(&nt, (void *)(g_carrier_lo + dos.e_lfanew), sizeof(nt));
            g_carrier_hi = g_carrier_lo + nt.OptionalHeader.SizeOfImage;
        }
    }
    if (g_carrier_lo == 0) return; // retry next tick

    if (g_veh == nullptr) {
        g_veh = AddVectoredExceptionHandler(1, veh);
        if (!g_veh) {
            MC2VR_LOG("watch: AddVectoredExceptionHandler failed (%lu) — feature "
                      "disabled", GetLastError());
            InterlockedExchange(&g_armed, 2);
            return;
        }
    }

    arm_sweep();
}

void report_window()
{
    if (!g_enabled) return;

    verify_and_report(); // value snapshot + DR clobber check

    // Hit table (top sites by count) — the actual RE answer.
    lock_acquire();
    const uint32_t n = g_hit_count;
    const uint64_t total = g_trap_total;
    // Simple selection: print up to 16 sites, highest count first.
    bool printed[HIT_MAX] = {};
    uint32_t shown = 0;
    char line[512];
    int w = 0;

    MC2VR_LOG("watch: window: traps=%llu unique sites=%u (view idx=%u, armed on %u "
              "threads)", (unsigned long long)total, n, g_view_idx, g_armed_threads);

    while (shown < 16) {
        uint32_t best = UINT32_MAX;
        uint32_t best_count = 0;
        for (uint32_t i = 0; i < n; i++) {
            if (!printed[i] && g_hits[i].count > best_count) {
                best = i;
                best_count = g_hits[i].count;
            }
        }
        if (best == UINT32_MAX) break;
        printed[best] = true;
        shown++;

        const uint32_t eip = g_hits[best].eip;
        const char *fn = fn_of(eip);
        if (carrier_self(eip)) {
            w += snprintf(line + w, sizeof(line) - w, " [%s@carrier-self:%08X x%u]",
                          slot_name(g_hits[best].slot), eip, g_hits[best].count);
        } else {
            w += snprintf(line + w, sizeof(line) - w, " [%s@%s:%s:%08X x%u]",
                          slot_name(g_hits[best].slot), region_of(eip),
                          fn ? fn : "?", eip, g_hits[best].count);
        }
        if (w >= (int)sizeof(line) - 1) w = sizeof(line) - 1; // snprintf truncation guard
        if (w > (int)sizeof(line) - 96) {
            MC2VR_LOG("watch:   %s", line);
            w = 0;
            line[0] = '\0';
        }
    }
    if (w > 0) {
        MC2VR_LOG("watch:   %s", line);
    }

    if (total == 0) {
        MC2VR_LOG("watch: ZERO traps in window — nothing (writer or reader, plaintext "
                  "or VM) touched the watched fields on any armed thread. If this "
                  "persists across windows, the field is dead, the access is on a "
                  "thread created after arming, or the wrong view is watched.");
    }

    // Reset window.
    for (uint32_t i = 0; i < n; i++) g_hits[i] = {};
    g_hit_count = 0;
    g_trap_total = 0;
    lock_release();
}

} // namespace mc2vr::watch

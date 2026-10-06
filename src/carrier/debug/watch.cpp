// Hardware-watchpoint tracer — see watch.hpp for the design and the RE
// questions this answers. Debug-only module, default off.

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
uint32_t g_pin_view = UINT32_MAX;   // conf-pinned view idx; default = first type-2
volatile LONG g_detail_left = 30;   // per-unique-EIP detail lines

constexpr uint32_t TARGET_MAX = 4;  // DR0-3

struct TargetSpec {
    char name[24];       // "quat", "pos", ... or "addr:XXXXXXXX"
    uint32_t entry_off;  // ViewEntry offset; UINT32_MAX = fixed_addr below
    uint32_t fixed_addr;
    uint32_t addr;       // resolved at arm time
    bool valid;           // aligned + committed
};
TargetSpec g_specs[TARGET_MAX];
uint32_t g_spec_count = 0;

// ---- chosen view --------------------------------------------------------------
// Written by on_view (main thread), read by the poller. The entry address is
// table + idx*stride, so the idx alone identifies it; entry is stored for
// convenience/verification.
volatile LONG g_have_view = 0;
uint32_t g_view_idx = 0;
uintptr_t g_view_entry = 0;
uint32_t g_type2_distinct[16];
uint32_t g_type2_count = 0;
bool g_type2_logged = false;

// ---- arm state -----------------------------------------------------------------
volatile LONG g_armed = 0;
uint32_t g_armed_threads = 0;
uint32_t g_armed_tids[64];
uint32_t g_armed_tid_count = 0;
uint32_t g_dr7 = 0;

// ---- hit aggregation -----------------------------------------------------------
constexpr uint32_t HIT_MAX = 256;
struct Hit {
    uint32_t eip;
    uint32_t slot;
    uint32_t count;
};
Hit g_hits[HIT_MAX];
uint32_t g_hit_count = 0;
uint64_t g_trap_total = 0;
volatile LONG g_lock = 0; // spinlock guarding the hit table

uintptr_t g_carrier_lo = 0, g_carrier_hi = 0; // our module range ("carrier-self")

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
    {0x0048e620u, 0x0048f030u, "RenderQueue_SubmitWorldPackets"},
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
// Reads bytes for the detail line; null-safe (some pages may not be readable).
bool read_bytes(uintptr_t addr, uint8_t *out, uint32_t len)
{
    MEMORY_BASIC_INFORMATION mbi;
    if (VirtualQuery((void *)addr, &mbi, sizeof(mbi)) == 0) return false;
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
    memcpy(out, (void *)addr, len); // same process; region length >= len checked by caller
    return true;
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

// slot = DR index 0..3; eip = the instruction AFTER the access (x86 data-BP
// traps report the following instruction — see watch.hpp).
void record_hit(uint32_t slot, uint32_t eip)
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
        // around the access. Budgeted so a hot field cannot flood the log.
        uint8_t before[16], after[8];
        const bool have_before = read_bytes((uintptr_t)eip - 16, before, 16);
        const bool have_after = read_bytes((uintptr_t)eip, after, 8);
        char hex[3 * 24 + 1];
        char *p = hex;
        if (have_before) {
            for (int i = 0; i < 16; i++) p += sprintf(p, "%02X", before[i]);
        } else {
            p += sprintf(p, "??");
        }
        *p++ = '|';
        if (have_after) {
            for (int i = 0; i < 8; i++) p += sprintf(p, "%02X", after[i]);
        } else {
            p += sprintf(p, "??");
        }

        InterlockedExchange(&g_detail_left, g_detail_left - 1);
        const char *fn = fn_of(eip);
        if (carrier_self(eip)) {
            MC2VR_LOG("watch hit #%u: %s eip=%08X carrier-self (our instrumentation) "
                      "bytes[-16..+8]=%s",
                      (unsigned)g_detail_left, g_specs[slot].name, eip, hex);
        } else {
            MC2VR_LOG("watch hit #%u: %s eip=%08X region=%s %s bytes[-16..+8]=%s",
                      (unsigned)g_detail_left, g_specs[slot].name, eip,
                      region_of(eip), fn ? fn : "(no known fn; see bytes)", hex);
        }
    }
}

LONG WINAPI veh(EXCEPTION_POINTERS *ep)
{
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT *c = ep->ContextRecord;
    const DWORD dr6 = (DWORD)c->Dr6;
    if ((dr6 & 0xF) == 0) {
        // Single-step without a debug-register cause = not ours (TF bit etc.).
        return EXCEPTION_CONTINUE_SEARCH;
    }
    const uint32_t eip = (uint32_t)c->Eip;
    c->Dr6 = 0;
    for (uint32_t slot = 0; slot < TARGET_MAX; slot++) {
        if (dr6 & (1u << slot)) {
            record_hit(slot, eip);
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
bool arm_one_thread(HANDLE h, uint32_t dr7)
{
    CONTEXT ctx = {};
    ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_i486;
    if (!GetThreadContext(h, &ctx)) return false;
    ctx.Dr0 = g_specs[0].valid ? g_specs[0].addr : 0;
    ctx.Dr1 = g_specs[1].valid ? g_specs[1].addr : 0;
    ctx.Dr2 = g_specs[2].valid ? g_specs[2].addr : 0;
    ctx.Dr3 = g_specs[3].valid ? g_specs[3].addr : 0;
    ctx.Dr6 = 0;
    ctx.Dr7 = dr7;
    return SetThreadContext(h, &ctx) != 0;
}

void arm_sweep()
{
    // Resolve addresses first (no threads suspended while doing this).
    for (uint32_t i = 0; i < g_spec_count; i++) {
        TargetSpec &s = g_specs[i];
        s.addr = (s.entry_off != UINT32_MAX) ? (uint32_t)(g_view_entry + s.entry_off)
                                             : s.fixed_addr;
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
        InterlockedExchange(&g_armed, 1); // don't retry forever
        return;
    }
    g_dr7 = dr7;

    // Suspend every process thread except this one (the poller; it must stay
    // runnable to resume the others, and its own reads of watched data stay
    // trap-free — DRs are per-thread, which is what makes value snapshots safe).
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) {
        MC2VR_LOG("watch: CreateToolhelp32Snapshot failed (%lu) — arm aborted",
                  GetLastError());
        return;
    }

    const DWORD self_tid = GetCurrentThreadId();
    const DWORD pid = GetCurrentProcessId();

    uint32_t ok = 0, failed = 0, game_suspended = 0;
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
                // prev >= 1 = the game itself had it suspended; our resume below
                // only undoes OUR increment, so the game's suspension survives.
                if (prev >= 1) game_suspended++;
                if (arm_one_thread(h, dr7)) {
                    if (g_armed_tid_count < 64) {
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

    MC2VR_LOG("watch: ARMED DR0-3 on %u thread(s) (%u failed, %u were already "
              "game-suspended), dr7=%08X mode=%s targets:",
              ok, failed, game_suspended, dr7, g_watch_reads ? "read+write" : "write-only");
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
    InterlockedExchange(&g_armed, ok > 0 ? 1 : 2); // 2 = failed arm, stop retrying
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
        ctx.ContextFlags = CONTEXT_DEBUG_REGISTERS | CONTEXT_i486;
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
    };
    static const Named named[] = {
        {"slot0", (uint32_t)MC2_VIEW_OFF_SLOT0},
        {"slot0v", (uint32_t)MC2_VIEW_OFF_SLOT0V},
        {"slotdir", (uint32_t)MC2_VIEW_OFF_SLOTDIR},
        {"slotparams", (uint32_t)MC2_VIEW_OFF_SLOTPARAMS},
        {"near", (uint32_t)MC2_VIEW_OFF_NEAR},
        {"fov", (uint32_t)MC2_VIEW_OFF_FOVCOS},
        {"fovsin", (uint32_t)MC2_VIEW_OFF_FOVSIN},
        {"dir670", (uint32_t)MC2_VIEW_OFF_DIR670},
        {"posprev", (uint32_t)MC2_VIEW_OFF_POS_PREV},
        {"pos", (uint32_t)MC2_VIEW_OFF_POS_CUR},
        {"serial", (uint32_t)MC2_VIEW_OFF_SERIAL},
        {"quat", (uint32_t)MC2_VIEW_OFF_QUAT},
        {"camdata", (uint32_t)MC2_VIEW_OFF_CAMDATA},
        {"flags808", (uint32_t)MC2_VIEW_OFF_FLAGS808},
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

void on_view(uint32_t idx, uint32_t type, const uint8_t *entry)
{
    if (!g_enabled) return;

    // Track distinct type-2 views once, for the log (which view did we pick?).
    if (type == MC2_VIEW_TYPE2) {
        bool seen = false;
        for (uint32_t i = 0; i < g_type2_count; i++) {
            if (g_type2_distinct[i] == idx) { seen = true; break; }
        }
        if (!seen && g_type2_count < 16) g_type2_distinct[g_type2_count++] = idx;
    }

    if (InterlockedCompareExchange(&g_have_view, 0, 0) != 0) return; // chosen already

    const bool wanted = (g_pin_view == UINT32_MAX) ? (type == MC2_VIEW_TYPE2)
                                                   : (idx == g_pin_view);
    if (!wanted) return;

    g_view_idx = idx;
    g_view_entry = (uintptr_t)entry;
    InterlockedExchange(&g_have_view, 1);

    if (g_pin_view != UINT32_MAX) {
        MC2VR_LOG("watch: pinned view idx=%u entry=%p (type=%u) — will arm on the next "
                  "poller tick", idx, (const void *)entry, type);
    } else if (type == MC2_VIEW_TYPE2) {
        MC2VR_LOG("watch: chose first type-2 view idx=%u entry=%p — will arm on the "
                  "next poller tick (other type-2 views seen so far: %u)", idx,
                  (const void *)entry, g_type2_count);
    }
}

void poll()
{
    if (!g_enabled) return;
    if (InterlockedCompareExchange(&g_armed, 0, 0) != 0) return;
    if (InterlockedCompareExchange(&g_have_view, 0, 0) == 0) return;

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

    if (!g_type2_logged && g_type2_count > 0) {
        g_type2_logged = true;
        char list[16 * 8];
        int n = 0;
        for (uint32_t i = 0; i < g_type2_count && n < (int)sizeof(list) - 8; i++) {
            n += snprintf(list + n, sizeof(list) - n, " %u", g_type2_distinct[i]);
        }
        MC2VR_LOG("watch: distinct type-2 view indices:%s", list);
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
        const TargetSpec &t = g_specs[g_hits[best].slot];
        if (carrier_self(eip)) {
            w += snprintf(line + w, sizeof(line) - w, " [%s@carrier-self:%08X x%u]",
                          t.name, eip, g_hits[best].count);
        } else {
            w += snprintf(line + w, sizeof(line) - w, " [%s@%s:%s:%08X x%u]",
                          t.name, region_of(eip), fn ? fn : "?", eip,
                          g_hits[best].count);
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
                  "persists across windows, the field is dead or the access is on an "
                  "unarmed thread.");
    }

    // Reset window.
    for (uint32_t i = 0; i < n; i++) g_hits[i] = {};
    g_hit_count = 0;
    g_trap_total = 0;
    lock_release();
}

} // namespace mc2vr::watch

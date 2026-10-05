// SecuROM-stub callback tracer — see stub_trace.hpp.

#include "stub_trace.hpp"

#include <safetyhook.hpp>

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <utility>

#include "game_addresses.h"
#include "hooks.hpp"
#include "log.hpp"

namespace mc2vr::trace {

namespace {

bool g_enabled = false;

// ---- phases ---------------------------------------------------------------
enum Phase : uint32_t { PRE = 0, STUB = 1, POST = 2, PHASE_COUNT = 3 };
const char *const PHASE_NAME[PHASE_COUNT] = {"PRE", "STUB", "POST"};
volatile uint32_t g_phase = PRE;
uint64_t g_phase_frame = UINT64_MAX;

// The stub call: `call 0x0050f660` at 0x004c99f9 (5 bytes); the next
// instruction is at 0x004c99fe. Both are plaintext .text.
constexpr uintptr_t STUB_CALL_SITE = 0x004c99f9u;
constexpr uintptr_t STUB_RETURN_SITE = 0x004c99feu;

// ---- regions --------------------------------------------------------------
enum Region : uint32_t { R_TEXT = 0, R_PROT = 1, R_OTHER = 2, REGION_COUNT = 3 };
const char *const REGION_NAME[REGION_COUNT] = {"text", "protected", "other"};

Region classify(uint32_t addr, const char **detail)
{
    // Section map (PE headers; docs/reverse_engineering/securom_vm.md).
    if (addr >= 0x00401000u && addr <= 0x00b04fffu) {
        *detail = ".text";
        return R_TEXT;
    }
    if (addr >= 0x01a48000u && addr <= 0x020fdfffu) { *detail = "Stext"; return R_PROT; }
    if (addr >= 0x020fe000u && addr <= 0x02104fffu) { *detail = "Sitext"; return R_PROT; }
    if (addr >= 0x02105000u && addr <= 0x02160fffu) { *detail = "Srdata"; return R_PROT; }
    if (addr >= 0x02161000u && addr <= 0x0245dfffu) { *detail = "Sdata"; return R_PROT; }
    if (addr >= 0x02464000u && addr <= 0x03771f0fu) { *detail = ".securom"; return R_PROT; }
    *detail = "other(heap/dll)";
    return R_OTHER;
}

// ---- targets (all plaintext function entries; addresses verified in the
// Ghidra project — names are the symbols there) ------------------------------
struct Target {
    const char *name;
    uintptr_t addr;
};
constexpr Target TARGETS[] = {
    {"PoseStore_GetPoseByHandle", 0x00665a60},
    {"PoseStore_ResolveHandle", 0x00649640},
    {"PoseStore_GetOwnerObject", 0x00649e90},
    {"Pose_Copy", 0x00824a10},
    {"PoseSource_LodFallback", 0x00434fe0},
    {"CameraRecords_PublishRing", 0x004906b0},
    {"ViewEntry_Activate", 0x00488e40},
    {"ViewManager_Update", 0x004891e0},
    {"ViewEntry_DeriveMatrices", 0x0048f9d0},
    {"RenderShell_InitDrawRecordTables", 0x00853ee0},
    {"PgMaterial_ctor", 0x0084e3f0},
    {"Technique_ResolveConstantRegisters", 0x0085b260},
    {"Shader_GetConstantRegisterIndex", 0x0085aeb0},
    {"PgPrimitive_SubmitToGPU", 0x00855690},
    {"RenderShell_RenderFrameTimed", 0x0085abd0},
    {"LtiRenderer_BeginSubmit", 0x0074aaa0},
};
constexpr size_t TARGET_COUNT = sizeof(TARGETS) / sizeof(TARGETS[0]);

constexpr uint32_t RET_SEEN_MAX = 16;  // distinct logged return addresses per (target, phase)

struct Stats {
    uint64_t by_phase[PHASE_COUNT];
    uint64_t by_region[REGION_COUNT];
    uint64_t prot_in_stub;       // protected-region caller while STUB
    uint32_t ret_seen[PHASE_COUNT][RET_SEEN_MAX];
    uint32_t ret_seen_n[PHASE_COUNT];
};
Stats g_stats[TARGET_COUNT];
uint64_t g_stub_calls = 0;
uint64_t g_stub_returns = 0;  // post-hook hits; < g_stub_calls means the stub sometimes does not return via the call-site successor

SafetyHookMid g_stub_pre, g_stub_post;
SafetyHookMid g_entry_mid[TARGET_COUNT];

void sync_phase_to_frame()
{
    const uint64_t frame = hooks::frame_count();
    if (frame != g_phase_frame) {
        g_phase_frame = frame;
        g_phase = PRE;
    }
}

void log_chain(size_t idx, uint32_t ret, uint32_t esp);

void on_entry(size_t idx, safetyhook::Context &ctx)
{
    sync_phase_to_frame();
    const uint32_t ret = *(volatile const uint32_t *)ctx.esp;  // MidHook at the entry instruction
    const char *detail = "";
    const Region region = classify(ret, &detail);
    Stats &st = g_stats[idx];
    const uint32_t phase = g_phase;
    st.by_phase[phase]++;
    st.by_region[region]++;
    if (region != R_TEXT && phase == STUB) {
        st.prot_in_stub++;
    }

    // One-shot per distinct (target, phase, return address).
    for (uint32_t k = 0; k < st.ret_seen_n[phase]; k++) {
        if (st.ret_seen[phase][k] == ret) {
            return;
        }
    }
    if (st.ret_seen_n[phase] < RET_SEEN_MAX) {
        st.ret_seen[phase][st.ret_seen_n[phase]++] = ret;
        MC2VR_LOG("trace: %s called from %08x [%s] phase=%s%s", TARGETS[idx].name, ret, detail,
                  PHASE_NAME[phase],
                  region != R_TEXT ? "  <-- CALLER OUTSIDE .text (protected/other code)" : "");
        if (phase == STUB) {
            log_chain(idx, ret, ctx.esp);
        }
    }
}

// Heuristic backtrace for STUB-phase hits: scan the stack above the return
// address for dwords that look like return addresses (in .text and preceded
// by an `E8 rel32` call, or any value inside the protected sections) and
// log them outermost-last. No frame pointers needed; false positives are
// possible (stale stack data) — read it as a hint, not a proof.
void log_chain(size_t idx, uint32_t ret, uint32_t esp)
{
    const uint8_t *stack_top = (const uint8_t *)((NT_TIB *)NtCurrentTeb())->StackBase;
    const uint32_t limit = (uint32_t)(esp + 4 + 1024 < (uint32_t)stack_top ? esp + 4 + 1024 : (uint32_t)stack_top);
    char line[512];
    int n = snprintf(line, sizeof(line), "%08x", ret);
    int found = 0;
    for (uint32_t a = esp + 4; a + 4 <= limit && found < 10; a += 4) {
        const uint32_t v = *(volatile const uint32_t *)a;
        const char *detail = "";
        const Region r = classify(v, &detail);
        bool plausible = false;
        if (r == R_TEXT && v >= 0x00401006u) {
            plausible = *(const volatile uint8_t *)(v - 5) == 0xE8;
        } else if (r == R_PROT) {
            plausible = true;
        }
        if (plausible && v != ret && n < (int)sizeof(line) - 24) {
            n += snprintf(line + n, sizeof(line) - n, " <- %08x%s", v, r == R_PROT ? "[PROT]" : "");
            found++;
        }
    }
    MC2VR_LOG("trace:   STUB chain for %s: %s", TARGETS[idx].name, line);
}

template <size_t I>
void entry_handler(safetyhook::Context &ctx)
{
    on_entry(I, ctx);
}

void stub_pre(safetyhook::Context &)
{
    sync_phase_to_frame();
    g_phase = STUB;
    g_stub_calls++;
}

void stub_post(safetyhook::Context &)
{
    g_stub_returns++;
    g_phase = POST;
}

template <size_t... Is>
void install_entries(std::index_sequence<Is...>)
{
    using Fn = void (*)(safetyhook::Context &);
    const Fn handlers[] = {entry_handler<Is>...};
    for (size_t i = 0; i < TARGET_COUNT; i++) {
        auto mid = SafetyHookMid::create(reinterpret_cast<uint8_t *>(TARGETS[i].addr), handlers[i]);
        if (!mid) {
            MC2VR_LOG("trace: entry hook %s @ %p failed (error %u) — skipped", TARGETS[i].name,
                      (void *)TARGETS[i].addr, (unsigned)mid.error().type);
            continue;
        }
        g_entry_mid[i] = std::move(*mid);
    }
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("trace: stub_trace = %s", value);
    return true;
}

void install()
{
    if (!g_enabled) {
        return;
    }
    auto pre = SafetyHookMid::create(reinterpret_cast<uint8_t *>(STUB_CALL_SITE), stub_pre);
    auto post = SafetyHookMid::create(reinterpret_cast<uint8_t *>(STUB_RETURN_SITE), stub_post);
    if (!pre || !post) {
        MC2VR_LOG("trace: stub bracket install failed (pre ok=%d post ok=%d) — tracer disabled",
                  pre ? 1 : 0, post ? 1 : 0);
        return;
    }
    g_stub_pre = std::move(*pre);
    g_stub_post = std::move(*post);
    install_entries(std::make_index_sequence<TARGET_COUNT>{});
    MC2VR_LOG("trace: stub brackets @ %p/%p + %u entry hooks installed",
              (void *)STUB_CALL_SITE, (void *)STUB_RETURN_SITE, (unsigned)TARGET_COUNT);
}

void report_window()
{
    if (!g_enabled) {
        return;
    }
    MC2VR_LOG("trace: window — stub calls=%llu returns=%llu%s", (unsigned long long)g_stub_calls,
              (unsigned long long)g_stub_returns,
              g_stub_returns != g_stub_calls ? "  <-- MISMATCH: STUB-phase attribution unreliable" : "");
    for (size_t i = 0; i < TARGET_COUNT; i++) {
        Stats &st = g_stats[i];
        const uint64_t total = st.by_phase[PRE] + st.by_phase[STUB] + st.by_phase[POST];
        if (total == 0) {
            continue;
        }
        MC2VR_LOG("trace:   %-34s PRE=%llu STUB=%llu POST=%llu | caller text=%llu protected=%llu "
                  "other=%llu | protected-in-STUB=%llu",
                  TARGETS[i].name, (unsigned long long)st.by_phase[PRE],
                  (unsigned long long)st.by_phase[STUB], (unsigned long long)st.by_phase[POST],
                  (unsigned long long)st.by_region[R_TEXT], (unsigned long long)st.by_region[R_PROT],
                  (unsigned long long)st.by_region[R_OTHER], (unsigned long long)st.prot_in_stub);
        memset(st.by_phase, 0, sizeof(st.by_phase));
        memset(st.by_region, 0, sizeof(st.by_region));
        st.prot_in_stub = 0;
    }
    g_stub_calls = 0;
    g_stub_returns = 0;
}

} // namespace mc2vr::trace

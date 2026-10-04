#include "stream_capture.hpp"

#include <safetyhook.hpp>
#include <windows.h>

#include "game_addresses.h"
#include "eye_replay.hpp"
#include "hooks.hpp"
#include "log.hpp"

#include <cstdio>
#include <cstring>

namespace mc2vr::s2c {

namespace {

// Command size in dwords (including the opcode dword) per opcode, from the
// RenderCmd_ExecuteStream switch (0x008569d0, plate in Ghidra). op 0x00 halts
// (its "size" is never consumed); op 0x13 shares case 0x0d's advance-only
// tail = 2-dword no-op. Every other handler advances EBP by size*4.
constexpr uint8_t OP_SIZE[27] = {
    1, 3, 4, 4, 3, 3, 3, 3, 7, 3, 1, 1, 1, 2, 2, 3, 5, 5, 3, 2, 3, 2, 3, 4, 2, 1, 1,
};
constexpr uint32_t OP_COUNT = 27;

// Walk safety: streams are observed at <=~3.4k commands/frame total, so a
// single stream cannot legitimately approach this. A missing terminator or a
// table bug stops the walk (and the copy) without touching execution.
constexpr uint32_t WALK_DWORD_CAP = 32 * 1024;

// Per-frame copy arena. Reset at each frame boundary; holds the raw dwords of
// every stream of one frame (observed order of magnitude: tens of KB).
constexpr uint32_t ARENA_DWORDS = 256 * 1024; // 1 MB
constexpr uint32_t STREAMS_PER_FRAME_MAX = 1024; // x2 for S2c-1 replay passes
constexpr uint32_t SAMPLES_PER_OP = 2;

bool g_enabled = false;
uint32_t g_dump_frames = 0;
float g_dump_delay_s = 15.0f;

// ---- S2c-1 frame replay ----------------------------------------------------

bool g_replay_enabled = false;
SafetyHookInline g_submit_hook;   // InlineHook on PgPrimitive_SubmitToGPU
uint64_t g_replays = 0;          // extra passes this window
double g_replay_ms_sum = 0.0;
double g_replay_ms_max = 0.0;

// PgPrimitive_SubmitToGPU is void(void) — cdecl/stdcall identical for ().
// Original first, side effects after (same discipline as FrameTick).
void submit_togpu_hook()
{
    eye::set_pass(1); // pass 1 = LEFT eye (override applies only if eye_pass=on)
    g_submit_hook.call<void>();

    if (!g_replay_enabled) {
        eye::set_pass(0);
        return;
    }

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    eye::set_pass(2); // pass 2 = RIGHT eye
    g_submit_hook.call<void>();

    QueryPerformanceCounter(&t1);
    const double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 /
                     (double)freq.QuadPart;
    g_replays++;
    g_replay_ms_sum += ms;
    if (ms > g_replay_ms_max) {
        g_replay_ms_max = ms;
    }
    eye::set_pass(0); // pass 2 = RIGHT eye (set below), back to none after
    (void)0;
}

// ---- per-frame state (main thread only) -------------------------------------

struct StreamRec {
    const uint32_t *base;
    uint32_t arg2;    // ExecuteStream stack arg 2 (VS-technique ctx used by op 8)
    uint32_t arg3;    // arg 3 (PS-technique ctx used by op 8)
    uint32_t cmds;
    uint32_t dwords;
    uint32_t arena_off; // in dwords
    bool truncated;
};
StreamRec g_recs[STREAMS_PER_FRAME_MAX] = {};
uint32_t g_rec_n = 0;
uint32_t g_arena[ARENA_DWORDS] = {};
uint32_t g_arena_used = 0; // dwords

uint64_t g_frame = UINT64_MAX; // hooks::frame_count() of the current frame
uint64_t g_hook_cmds = 0;      // MidHook hits this frame (execution truth)
uint64_t g_walk_cmds = 0;      // walk-derived command count this frame

// ---- window aggregates (main thread writes; poller reads+resets) ------------

uint64_t g_w_streams = 0;
uint64_t g_w_walk_cmds = 0;
uint64_t g_w_hook_cmds = 0;
uint64_t g_w_dwords = 0;
uint64_t g_w_truncated = 0;
uint64_t g_w_runaway = 0;
uint64_t g_w_frames_with_streams = 0;
uint64_t g_w_opcount[OP_COUNT] = {};
uint32_t g_w_spf_min = 0;
uint32_t g_w_spf_max = 0;
uint64_t g_w_dump_files = 0;

// ---- census samples (once per run) --------------------------------------------

uint32_t g_samples_done[OP_COUNT] = {};

const char *const OP_NAME[OP_COUNT] = {
    "halt",
    "SetRenderState",
    "SetVertexShaderConstantF(reg,ptr,cnt)",
    "SetPixelShaderConstantF(reg,ptr,cnt)",
    "BindViewTextureFamily(+0x101)",
    "StreamSource/Bind",
    "SetRenderTarget0+DepthStencil",
    "SetRenderTarget(slot)",
    "SetScreenConstants(p3,p6)",
    "SetClipPlane(bit,ptr)",
    "ReapplyClipPlaneMask",
    "DisableClipPlanes",
    "ViewType9StretchBlit",
    "StretchRectFromGlobal",
    "SetPassObject+InvalidatePassState",
    "thunk_FUN_004a05b9(0,0,p2,p1,1.0)",
    "BeginSurfacePass(gate)",
    "EndSurfacePass(gate)",
    "BindTexture+SamplerStates",
    "NO-OP (advance-only)",
    "BindViewTextureFamily",
    "FUN_0074af30 (clear/RT pass)",
    "DeviceSetStreamSource(p1,p2)",
    "thunk_FUN_0256af90(p2)",
    "SetTexture0+RS0x9a(flags)",
    "HalfResPassBegin",
    "HalfResPassEnd",
};

void log_sample(uint32_t op, const uint32_t *c, const uint32_t *base)
{
    char raw[96];
    int m = 0;
    const uint32_t n = OP_SIZE[op];
    for (uint32_t i = 0; i < n && m < (int)sizeof(raw) - 12; i++) {
        m += _snprintf(raw + m, sizeof(raw) - m, " %08x", c[i]);
    }
    raw[m] = '\0';

    // Decoded fields for the ops whose payload layout is fully RE'd.
    char note[96] = "";
    switch (op) {
    case 1:
        _snprintf(note, sizeof(note), " state=%02x value=%08x", c[1], c[2]);
        break;
    case 2:
        _snprintf(note, sizeof(note), " vsReg=%u dataPtr=%08x vec4=%u", c[1], c[2], c[3]);
        break;
    case 3:
        _snprintf(note, sizeof(note), " psReg=%u dataPtr=%08x vec4=%u", c[1], c[2], c[3]);
        break;
    case 8:
        _snprintf(note, sizeof(note), " p3=%u p6=%u (screen consts from ctx args)", c[3], c[6]);
        break;
    case 9:
        _snprintf(note, sizeof(note), " planeBit=%u planePtr=%08x", c[1], c[2]);
        break;
    case 0x10:
    case 0x11:
        _snprintf(note, sizeof(note), " gate=%u (applies when 1 or 2)", c[4]);
        break;
    default:
        break;
    }

    MC2VR_LOG("S2c census op=%02u %-44s stream=%p:{%s }%s", op,
              OP_NAME[op < OP_COUNT ? op : 0], (const void *)base, raw, note);
}

// ---- frame lifecycle ------------------------------------------------------------

bool dump_armed()
{
    if (g_dump_frames == 0) {
        return false;
    }
    static LARGE_INTEGER freq = {};
    static LARGE_INTEGER first = {};
    if (freq.QuadPart == 0) {
        QueryPerformanceFrequency(&freq);
    }
    if (first.QuadPart == 0) {
        QueryPerformanceCounter(&first);
        MC2VR_LOG("S2c: stream capture started; dump window arms in %.1fs "
                  "(debug_dump_delay)", (double)g_dump_delay_s);
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (double)(now.QuadPart - first.QuadPart) / (double)freq.QuadPart >=
           (double)g_dump_delay_s;
}

void write_dump(uint64_t frame)
{
    // Directory: next to this DLL (same derivation as log.cpp).
    HMODULE self = nullptr;
    wchar_t dir[MAX_PATH];
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCWSTR)&write_dump, &self) ||
        GetModuleFileNameW(self, dir, MAX_PATH) == 0) {
        MC2VR_LOG("S2c: dump skipped — cannot locate deploy dir");
        return;
    }
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) {
        return;
    }
    *slash = L'\0';

    wchar_t path[MAX_PATH];
    _snwprintf(path, MAX_PATH, L"%s\\mc2vr_stream_frame%llu.txt", dir,
               (unsigned long long)frame);

    FILE *f = _wfopen(path, L"wb");
    if (!f) {
        MC2VR_LOG("S2c: dump FAILED to open %ls", path);
        return;
    }

    fprintf(f, "# mc2vr S2c-0 stream dump, frame %llu\n", (unsigned long long)frame);
    fprintf(f, "# dwords are little-endian u32; opcode size table:\n");
    fprintf(f, "# op: 1,3,4,4,3,3,3,3,7,3,1,1,1,2,2,3,5,5,3,2,3,2,3,4,2,1,1 (dwords)\n");
    fprintf(f, "# arg2/arg3 = ExecuteStream stack args 2/3 (technique ctx used by op 8)\n");
    fprintf(f, "streams %u arenaDwords %u\n", g_rec_n, g_arena_used);

    for (uint32_t i = 0; i < g_rec_n; i++) {
        const StreamRec &r = g_recs[i];
        fprintf(f, "\nstream base=%08x arg2=%08x arg3=%08x cmds=%u dwords=%u trunc=%u\n",
                (uint32_t)(uintptr_t)r.base, r.arg2, r.arg3, r.cmds, r.dwords,
                r.truncated ? 1u : 0u);
        const uint32_t *d = g_arena + r.arena_off;
        // Truncated records: only dump what actually landed in the arena.
        uint32_t dumpable = r.dwords;
        if (r.arena_off + dumpable > ARENA_DWORDS) {
            dumpable = ARENA_DWORDS - r.arena_off;
        }
        for (uint32_t w = 0; w < dumpable; w += 8) {
            fprintf(f, "d");
            const uint32_t n = dumpable - w < 8 ? dumpable - w : 8;
            for (uint32_t j = 0; j < n; j++) {
                fprintf(f, " %08x", d[w + j]);
            }
            fprintf(f, "\n");
        }
    }
    fclose(f);
    g_w_dump_files++;
    MC2VR_LOG("S2c: dumped frame %llu -> %ls (%u streams, %u dwords)",
              (unsigned long long)frame, path, g_rec_n, g_arena_used);
}

void reset_frame()
{
    g_rec_n = 0;
    g_arena_used = 0;
    g_hook_cmds = 0;
    g_walk_cmds = 0;
}

void finalize_frame(uint64_t frame)
{
    if (g_rec_n > 0) {
        g_w_frames_with_streams++;
        g_w_streams += g_rec_n;
        if (g_w_spf_min == 0 || g_rec_n < g_w_spf_min) {
            g_w_spf_min = g_rec_n;
        }
        if (g_rec_n > g_w_spf_max) {
            g_w_spf_max = g_rec_n;
        }
    }
    g_w_walk_cmds += g_walk_cmds;
    g_w_hook_cmds += g_hook_cmds;

    static uint32_t dump_remaining = 0;
    static bool window_done = false;
    if (!window_done && dump_remaining == 0 && dump_armed()) {
        dump_remaining = g_dump_frames;
        window_done = true;
        MC2VR_LOG("S2c: dump window open — dumping the next %u frames",
                  g_dump_frames);
    }
    if (dump_remaining > 0) {
        write_dump(frame);
        dump_remaining--;
        if (dump_remaining == 0) {
            MC2VR_LOG("S2c: dump window closed");
        }
    }
}

} // namespace

// ---- public entry points -------------------------------------------------------

void on_opcode(uint32_t esp, uint32_t ebp, uint32_t eax)
{
    // (eax = opcode; unused here — render_dump.cpp keeps the histogram.)
    (void)eax;
    if (!g_enabled) {
        return;
    }

    const uint64_t frame = hooks::frame_count();
    if (frame != g_frame) {
        if (g_frame != UINT64_MAX) {
            finalize_frame(g_frame);
        }
        g_frame = frame;
        reset_frame();
    }
    g_hook_cmds++;

    // First command of a stream? At the hooked `cmp eax,0x1a` the live stack
    // is: [esp+0x10]=return addr, [esp+0x14]=stream, [esp+0x18/0x1c]=args 2/3.
    const uint32_t *stream = *(const uint32_t **)(uintptr_t)(esp + 0x14);
    if ((const uint32_t *)(uintptr_t)ebp != stream) {
        return;
    }
    const uint32_t arg2 = *(const uint32_t *)(uintptr_t)(esp + 0x18);
    const uint32_t arg3 = *(const uint32_t *)(uintptr_t)(esp + 0x1c);

    // Walk + copy the whole stream.
    uint32_t cmds = 0;
    uint32_t dwords = 0;
    bool runaway = false;
    bool truncated = false;
    uint32_t copy_off = g_arena_used;

    const uint32_t *p = stream;
    for (;;) {
        const uint32_t op = *p;
        if (op >= OP_COUNT) {
            runaway = true; // missing terminator or size-table bug — stop reading
            break;
        }
        const uint32_t sz = OP_SIZE[op];
        if (dwords + sz > WALK_DWORD_CAP) {
            runaway = true;
            break;
        }
        if (g_samples_done[op] < SAMPLES_PER_OP) {
            log_sample(op, p, stream);
            g_samples_done[op]++;
        }
        g_w_opcount[op]++;
        // Copy including the op-0 terminator (sz == 1): a replayed copy
        // must be a complete, terminated stream.
        if (!truncated && g_arena_used + sz <= ARENA_DWORDS) {
            memcpy(g_arena + g_arena_used, p, sz * sizeof(uint32_t));
            g_arena_used += sz;
        } else {
            truncated = true;
        }
        p += sz;
        dwords += sz;
        cmds++; // the MidHook also fires for the terminator's cmp — keep the
               // walk count comparable to the hook count
        if (op == 0) {
            break; // halt (terminator dword already copied above)
        }
    }

    if (runaway) {
        g_w_runaway++;
        MC2VR_LOG("S2c: RUNAWAY walk stopped at stream %p (cmd %u, dword %u) — "
                  "size-table bug or missing terminator (execution unaffected)",
                  (const void *)stream, cmds, dwords);
    }
    if (truncated) {
        g_w_truncated++;
    }

    if (g_rec_n < STREAMS_PER_FRAME_MAX) {
        g_recs[g_rec_n++] = {stream, arg2, arg3, cmds, dwords, copy_off, truncated};
    }
    g_walk_cmds += cmds;
    g_w_dwords += dwords;
}

void report_window()
{
    // Replay stats report even without stream_capture (independent feature).
    if (g_replays > 0) {
        MC2VR_LOG("S2c replay: extraPasses=%llu avgMs=%.2f maxMs=%.2f",
                  (unsigned long long)g_replays,
                  g_replays ? g_replay_ms_sum / (double)g_replays : 0.0,
                  g_replay_ms_max);
    }
    g_replays = 0;
    g_replay_ms_sum = 0.0;
    g_replay_ms_max = 0.0;

    if (!g_enabled) {
        return;
    }

    const int64_t delta = (int64_t)g_w_hook_cmds - (int64_t)g_w_walk_cmds;
    MC2VR_LOG("S2c window: streams=%llu framesWithStreams=%llu streams/frame=%u..%u "
              "cmds walk=%llu hook=%llu (delta=%lld — zero is healthy; nonzero = "
              "size-table bug or mid-frame stream mutation) "
              "dwords=%llu trunc=%llu runaway=%llu dumps=%llu",
              (unsigned long long)g_w_streams,
              (unsigned long long)g_w_frames_with_streams, g_w_spf_min, g_w_spf_max,
              (unsigned long long)g_w_walk_cmds, (unsigned long long)g_w_hook_cmds,
              (long long)delta, (unsigned long long)g_w_dwords,
              (unsigned long long)g_w_truncated, (unsigned long long)g_w_runaway,
              (unsigned long long)g_w_dump_files);

    char hist[640];
    int n = 0;
    for (uint32_t op = 0; op < OP_COUNT && n < (int)sizeof(hist) - 24; op++) {
        if (g_w_opcount[op] > 0) {
            n += _snprintf(hist + n, sizeof(hist) - n, " %02u=%llu", op,
                           (unsigned long long)g_w_opcount[op]);
        }
    }
    hist[n] = '\0';
    MC2VR_LOG("S2c walk ops:%s", n ? hist : " (none — no streams in window)");

    g_w_streams = 0;
    g_w_walk_cmds = 0;
    g_w_hook_cmds = 0;
    g_w_dwords = 0;
    g_w_truncated = 0;
    g_w_runaway = 0;
    g_w_frames_with_streams = 0;
    memset(g_w_opcount, 0, sizeof(g_w_opcount));
    g_w_spf_min = 0;
    g_w_spf_max = 0;
    g_w_dump_files = 0;
}

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("S2c: debug_stream_capture=%s (census %s)", value,
              g_enabled ? "armed" : "idle");
    return true;
}

void set_dump_frames(uint32_t n)
{
    g_dump_frames = n;
    MC2VR_LOG("S2c: debug_stream_dump_frames=%u", n);
}

void set_dump_delay(float seconds)
{
    g_dump_delay_s = seconds;
    MC2VR_LOG("S2c: debug_dump_delay=%.1fs", (double)seconds);
}

bool set_replay_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) {
        g_replay_enabled = true;
    } else if (strcmp(value, "off") == 0) {
        g_replay_enabled = false;
    } else {
        return false;
    }
    MC2VR_LOG("S2c: frame_replay=%s (S2c-1 second draw pass, same eye/RTs)",
              g_replay_enabled ? "on" : "off");
    return true;
}

void install()
{
    if (g_submit_hook) {
        return; // already installed
    }

    auto result = SafetyHookInline::create(
        reinterpret_cast<uint8_t *>(MC2_PGPRIMITIVE_SUBMITTOGPU),
        reinterpret_cast<uint8_t *>(&submit_togpu_hook));
    if (!result) {
        MC2VR_LOG("S2c: warning — SubmitToGPU InlineHook failed @ %p (error %u) "
                  "— frame replay disabled, capture unaffected",
                  (void *)MC2_PGPRIMITIVE_SUBMITTOGPU,
                  (unsigned)result.error().type);
        return;
    }
    g_submit_hook = std::move(*result);
    MC2VR_LOG("S2c: installed SubmitToGPU InlineHook @ %p (replay %s)",
              (void *)MC2_PGPRIMITIVE_SUBMITTOGPU,
              g_replay_enabled ? "ARMED" : "idle");
}

} // namespace mc2vr::s2c

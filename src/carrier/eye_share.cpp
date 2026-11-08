#include "eye_share.hpp"

#include <windows.h>
#include <d3d9.h> // type/layout constants only

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "device.hpp"
#include "debug/gpu_census.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "view_rewrite.hpp"

namespace mc2vr::eye_share {

namespace {

// Device/texture/query vtable slots (d3d9.h order; the family is live-verified
// in eye_replay.cpp — GetBackBuffer=18, CreateRenderTarget=28, StretchRect=34).
constexpr size_t DSLOT_CreateTexture = 23;
constexpr size_t DSLOT_CreateQuery = 118;
// IDirect3DTexture9 vtable (d3d9.h full count, verified live 2026-10-04 after
// the slot-12 mistake): IUnknown 0-2, resource 3-10, BaseTexture9 11-16
// (SetLOD, GetLOD, GetLevelCount, SetAutoGenFilterType, GetAutoGenFilterType,
// GenerateMipSubLevels), then 17 GetLevelDesc, 18 GetSurfaceLevel, 19
// LockRect, 20 UnlockRect, 21 AddDirtyRect.
constexpr size_t TSLOT_GetSurfaceLevel = 18;
constexpr size_t COM_Release = 2;
// IDirect3DQuery9 vtable (d3d9.h): IUnknown 0-2, GetDevice=3, GetType=4,
// GetDataSize=5, Issue=6, GetData=7. (Slot-order bug lived here once: the two
// reversed meant "Issue(1)" actually called GetData with pData=1 — a completed
// event query then wrote through pointer 0x1 and crashed the game on the SECOND
// boundary; the first survived only because the query was never really issued
// and frame-pointer epilogues healed the arg-count stack drift.)
constexpr size_t QSLOT_Issue = 6;
constexpr size_t QSLOT_GetData = 7;

using CreateTexture_t = HRESULT(__stdcall *)(void *, UINT, UINT, UINT, DWORD,
                                            D3DFORMAT, D3DPOOL, void **, HANDLE *);
using CreateQuery_t = HRESULT(__stdcall *)(void *, D3DQUERYTYPE, void **);
using GetSurfaceLevel_t = HRESULT(__stdcall *)(void *, UINT, void **);
using Release_t = HRESULT(__stdcall *)(void *);
using SurfaceDesc_t = HRESULT(__stdcall *)(void *, D3DSURFACE_DESC *);
using QueryIssue_t = HRESULT(__stdcall *)(void *, DWORD);
using QueryGetData_t = HRESULT(__stdcall *)(void *, void *, DWORD, DWORD);

constexpr uint32_t RING_SLOTS = 4u; // per eye; >=3 so the host's read window
                                    // never overlaps the carrier's write slot
                                    // in practice at 30 Hz game cadence
constexpr uint32_t SYNC_BUDGET_MS = 8u; // bounded; the next BeginSubmit spin
                                       // waits for the same work anyway

bool g_broken = false;       // resource creation failed — module off this run
void *g_device = nullptr;
void *g_bb = nullptr;
uint32_t g_w = 0, g_h = 0, g_fmt = 0;

struct Slot {
    void *tex;    // IDirect3DTexture9 (owned)
    void *surf;   // level-0 surface (owned ref from GetSurfaceLevel)
    HANDLE handle;
};
Slot g_slots[2][RING_SLOTS] = {};
bool g_slots_ok = false;
bool g_config_pending = false; // CONFIG to send once the host is connected
void *g_query = nullptr; // event query for the synchronous path

// ---- async completion (default) ----------------------------------------------
// One event query per ring slot; the render thread issues it after the RIGHT
// capture and moves on, the completion thread polls it and sends the pair.
bool g_async = true;  // debug_sync_frame_ready=on -> false
void *g_qring[RING_SLOTS] = {};
struct Pending {
    volatile LONG state;  // 0 free, 1 issued (completion thread owns it)
    uint64_t frame_id;
    HANDLE hl, hr;
    uint32_t pose_l, pose_r;
    double t_issue;
};
Pending g_pend[RING_SLOTS] = {};
CRITICAL_SECTION g_poll_lock;  // completion thread vs Reset (query lifetime)
HANDLE g_poll_event = nullptr;  // signalled when a pair is issued
HANDLE g_poll_thread = nullptr;
uint64_t g_async_done = 0, g_ring_waits = 0;
double g_done_sum_ms = 0.0, g_done_max_ms = 0.0;

uint32_t g_ring = 0;     // ring index for the frame being built
uint64_t g_frame_id = 0; // published (complete) frame ids
// LEFT captured this frame, its FRAME_READY deferred to the RIGHT capture:
// the host consumes complete pairs only, so ONE GPU sync after pass 2 covers
// both eyes (a sync at 1->2 would stall the CPU on pass 1's GPU work before
// it could queue pass 2).
bool g_left_pending = false;
uint32_t g_left_pose_id = 0;

// ---- window counters (render thread writes; poller reads+resets) ----------
uint64_t g_captures[2] = {0, 0};
uint64_t g_publishes = 0;
uint64_t g_sync_timeouts = 0;
uint64_t g_ring_full = 0;
uint64_t g_ipc_skipped = 0; // captures while no host
// CPU time stalled in gpu_sync (Issue -> query done): the cost of having no
// cross-process fence — a native OpenXR app would not wait here.
uint64_t g_sync_n = 0;
double g_sync_sum_ms = 0.0, g_sync_max_ms = 0.0;

double qpc_ms()
{
    static LARGE_INTEGER f = {};
    if (f.QuadPart == 0) {
        QueryPerformanceFrequency(&f);
    }
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return (double)t.QuadPart * 1000.0 / (double)f.QuadPart;
}

DWORD WINAPI completion_thread(void *);

bool ensure_resources(void *device, void *bb)
{
    if (g_slots_ok) return true;
    if (g_broken || device == nullptr || bb == nullptr) return false;

    g_device = device;
    g_bb = bb;

    D3DSURFACE_DESC desc = {};
    if (FAILED(((SurfaceDesc_t)(*(void ***)bb)[12])(bb, &desc))) {
        MC2VR_LOG("share: backbuffer GetDesc failed — shared eyes inactive");
        g_broken = true;
        return false;
    }
    g_w = desc.Width;
    g_h = desc.Height;
    g_fmt = (uint32_t)desc.Format;

    void **vt = *(void ***)device;
    auto create_texture = (CreateTexture_t)vt[DSLOT_CreateTexture];

    char fmt_name[32];
    snprintf(fmt_name, sizeof fmt_name, "D3DFMT_%u", g_fmt);
    MC2VR_LOG("share: creating %u slots/eye %ux%u fmt %s shared RT ring "
              "(backbuffer MS=%u/%u)",
              RING_SLOTS, g_w, g_h, fmt_name,
              (unsigned)desc.MultiSampleType, desc.MultiSampleQuality);

    for (uint32_t eye = 0; eye < 2; ++eye) {
        for (uint32_t s = 0; s < RING_SLOTS; ++s) {
            Slot &slot = g_slots[eye][s];
            HANDLE h = nullptr;
            // RENDERTARGET usage: the boundary capture blits run through
            // device::blit_surfaces (StretchRect), which requires RT surfaces.
            // RT + pSharedHandle opened fine in the probe (usage-0) — RT usage
            // is verified live by this module's own counters.
            HRESULT hr = create_texture(device, g_w, g_h, 1, D3DUSAGE_RENDERTARGET,
                                        desc.Format, D3DPOOL_DEFAULT, &slot.tex, &h);
            if (FAILED(hr) || slot.tex == nullptr || h == nullptr) {
                MC2VR_LOG("share: CreateTexture(shared RT) FAILED hr=%08lx "
                          "h=%p — shared eyes inactive this run",
                          (unsigned long)hr, (void *)h);
                g_broken = true;
                return false;
            }
            hr = ((GetSurfaceLevel_t)(*(void ***)slot.tex)[TSLOT_GetSurfaceLevel])(
                slot.tex, 0, &slot.surf);
            if (FAILED(hr) || slot.surf == nullptr) {
                MC2VR_LOG("share: GetSurfaceLevel FAILED hr=%08lx",
                          (unsigned long)hr);
                g_broken = true;
                return false;
            }
            slot.handle = h;
        }
    }

    // One reusable event query (the same GPU-sync primitive the game's
    // BeginSubmit spins on; the probe proved cross-process reads are coherent
    // behind it — no fence needed on legacy handles).
    HRESULT hr = ((CreateQuery_t)vt[DSLOT_CreateQuery])(device, D3DQUERYTYPE_EVENT,
                                                        &g_query);
    if (FAILED(hr) || g_query == nullptr) {
        MC2VR_LOG("share: CreateQuery FAILED hr=%08lx — shared eyes inactive",
                  (unsigned long)hr);
        g_broken = true;
        return false;
    }

    if (g_async) {
        if (g_poll_thread == nullptr) {
            InitializeCriticalSection(&g_poll_lock);
            g_poll_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            g_poll_thread = CreateThread(nullptr, 0, completion_thread, nullptr, 0, nullptr);
        }
        EnterCriticalSection(&g_poll_lock);
        bool ok = g_poll_thread != nullptr;
        for (uint32_t i = 0; i < RING_SLOTS && ok; ++i) {
            g_pend[i].state = 0;
            if (FAILED(((CreateQuery_t)vt[DSLOT_CreateQuery])(device, D3DQUERYTYPE_EVENT,
                                                              &g_qring[i])) ||
                g_qring[i] == nullptr) {
                ok = false;
            }
        }
        LeaveCriticalSection(&g_poll_lock);
        if (!ok) {
            MC2VR_LOG("share: async completion unavailable (thread/query creation failed) — "
                      "synchronous end-of-frame wait");
            g_async = false;
        }
    }

    g_slots_ok = true;
    g_config_pending = true;
    MC2VR_LOG("share: shared RT ring ready (handles eye0: 0x%08x 0x%08x 0x%08x 0x%08x, "
              "eye1: 0x%08x 0x%08x 0x%08x 0x%08x)",
              (unsigned)(uintptr_t)g_slots[0][0].handle,
              (unsigned)(uintptr_t)g_slots[0][1].handle,
              (unsigned)(uintptr_t)g_slots[0][2].handle,
              (unsigned)(uintptr_t)g_slots[0][3].handle,
              (unsigned)(uintptr_t)g_slots[1][0].handle,
              (unsigned)(uintptr_t)g_slots[1][1].handle,
              (unsigned)(uintptr_t)g_slots[1][2].handle,
              (unsigned)(uintptr_t)g_slots[1][3].handle);
    return true;
}

// Front-load the GPU wait for everything enqueued so far (capture blit + the
// pass's composite). Bounded — on timeout we publish anyway and count it: the
// host reads the newest COMPLETE slot, worst case it sees the previous ring
// slot's content for one frame. DXVK needs D3DGETDATA_FLUSH here or the
// pending command buffer is never submitted (probe-proven 2026-10-04).
bool gpu_sync()
{
    if (g_query == nullptr) return true;
    const double t0 = qpc_ms();
    ((QueryIssue_t)(*(void ***)g_query)[QSLOT_Issue])(g_query, D3DISSUE_END);
    const ULONGLONG deadline = GetTickCount64() + SYNC_BUDGET_MS;
    bool done = true;
    for (;;) {
        DWORD data = 0;
        const HRESULT hr =
            ((QueryGetData_t)(*(void ***)g_query)[QSLOT_GetData])(
                g_query, &data, sizeof data, D3DGETDATA_FLUSH);
        if (hr != S_FALSE) break; // S_OK (done) or error (logged by count)
        if (GetTickCount64() > deadline) {
            g_sync_timeouts++;
            done = false;
            break;
        }
        Sleep(0);
    }
    const double ms = qpc_ms() - t0;
    g_sync_n++;
    g_sync_sum_ms += ms;
    if (ms > g_sync_max_ms) g_sync_max_ms = ms;
    return done;
}

// Completion thread: sends each issued pair once its event query signals, in
// frame order. GetData from this thread is safe because the device is created
// D3DCREATE_MULTITHREADED in async mode (device.cpp).
DWORD WINAPI completion_thread(void *)
{
    for (;;) {
        bool any = false;
        EnterCriticalSection(&g_poll_lock);
        // oldest issued pair first
        int best = -1;
        for (uint32_t i = 0; i < RING_SLOTS; ++i) {
            if (g_pend[i].state == 1 &&
                (best < 0 || g_pend[i].frame_id < g_pend[best].frame_id)) {
                best = (int)i;
            }
        }
        if (best >= 0 && g_qring[best] != nullptr) {
            any = true;
            Pending &p = g_pend[best];
            DWORD data = 0;
            const HRESULT hr = ((QueryGetData_t)(*(void ***)g_qring[best])[QSLOT_GetData])(
                g_qring[best], &data, sizeof data, 0);
            if (hr != S_FALSE) {  // S_OK (done) or an error: publish either way
                const bool ok_l = ipc::send_frame_ready(p.frame_id, (uint64_t)(uintptr_t)p.hl,
                                                        (uint32_t)best, 0u, g_w, g_h, p.pose_l);
                const bool ok_r = ok_l && ipc::send_frame_ready(p.frame_id,
                                                                (uint64_t)(uintptr_t)p.hr,
                                                                (uint32_t)best, 1u, g_w, g_h,
                                                                p.pose_r);
                if (ok_r) {
                    const double ms = qpc_ms() - p.t_issue;
                    g_done_sum_ms += ms;
                    if (ms > g_done_max_ms) g_done_max_ms = ms;
                    g_async_done++;
                    g_publishes++;
                    InterlockedExchange(&p.state, 0);
                } else {
                    g_ring_full++;  // retried on the next poll
                }
            }
        }
        LeaveCriticalSection(&g_poll_lock);
        if (any) {
            Sleep(0);
        } else {
            WaitForSingleObject(g_poll_event, 2);
        }
    }
    return 0;
}

// Wait (render thread) until ring slot `i` is no longer owned by the
// completion thread — only when the host fell 4 pairs behind.
void wait_slot_free(uint32_t i)
{
    if (g_pend[i].state == 0) return;
    g_ring_waits++;
    const double t0 = qpc_ms();
    while (g_pend[i].state != 0 && qpc_ms() - t0 < 50.0) {
        Sleep(0);
    }
}

} // namespace

bool set_sync_frame_ready(const char *value)
{
    if (strcmp(value, "on") == 0) g_async = false;
    else if (strcmp(value, "off") == 0) g_async = true;
    else return false;
    MC2VR_LOG("share: debug_sync_frame_ready=%s (%s)", value,
              g_async ? "completion thread" : "synchronous end-of-frame wait");
    return true;
}

bool async_completion()
{
    return g_async;
}

// ---- public entry points ------------------------------------------------------

void on_pass_boundary(uint32_t next_pass, void *device, void *bb)
{
    // 2 = 1->2 boundary (LEFT capture); 0 = 2->0 boundary (RIGHT capture).
    // Pass 1 (the 0->1 boundary) carries no fresh eye — the backbuffer still
    // holds the previous frame's already-presented RIGHT — so it is ignored.
    if (next_pass != 2u && next_pass != 0u) return;
    if (!ipc::connected()) {
        // No host (permanent for the run — the carrier only connects at
        // attach): count the boundaries and skip. Avoids allocating ~118 MB of
        // shared RTs that nobody would read.
        g_ipc_skipped++;
        return;
    }
    if (!ensure_resources(device, bb)) return;

    const uint32_t eye = (next_pass == 2) ? 0u : 1u; // 1->2 = LEFT, 2->0 = RIGHT
    Slot &slot = g_slots[eye][g_ring];

    if (g_config_pending && ipc::send_config(g_w, g_h, g_fmt)) {
        g_config_pending = false;
    }

    if (eye == 0) {
        g_left_pending = false;
        if (g_async) {
            wait_slot_free(g_ring);  // never overwrite a pair still in flight
        }
    }
    gpu_census::carrier_begin();
    const bool blitted = device::blit_surfaces(bb, slot.surf);
    gpu_census::carrier_end();
    if (!blitted) {
        // Counted by eye_replay's window as blit failures there; nothing else
        // to do — publishing a half-copied slot is worse than skipping.
        return;
    }
    g_captures[eye]++;

    if (eye == 0) {
        g_left_pending = true;
        g_left_pose_id = view_rewrite::current_pose_id();
        return;  // published together with RIGHT, after one GPU sync
    }
    if (!g_left_pending) {
        return;  // no LEFT this frame: a lone RIGHT can never complete a pair
    }
    g_left_pending = false;

    if (g_async) {
        // Fence the pair and hand it to the completion thread; the render
        // thread goes straight on to the next frame.
        void *q = g_qring[g_ring];
        ((QueryIssue_t)(*(void ***)q)[QSLOT_Issue])(q, D3DISSUE_END);
        DWORD data = 0;
        ((QueryGetData_t)(*(void ***)q)[QSLOT_GetData])(q, &data, sizeof data,
                                                         D3DGETDATA_FLUSH);  // submit, no wait
        Pending &p = g_pend[g_ring];
        p.frame_id = g_frame_id;
        p.hl = g_slots[0][g_ring].handle;
        p.hr = slot.handle;
        p.pose_l = g_left_pose_id;
        p.pose_r = view_rewrite::current_pose_id();
        p.t_issue = qpc_ms();
        InterlockedExchange(&p.state, 1);
        SetEvent(g_poll_event);
        g_frame_id++;
        g_ring = (g_ring + 1) % RING_SLOTS;
        return;
    }

    gpu_sync();

    const Slot &left = g_slots[0][g_ring];
    const bool ok_l = ipc::send_frame_ready(g_frame_id, (uint64_t)(uintptr_t)left.handle,
                                            g_ring, 0u, g_w, g_h, g_left_pose_id);
    const bool ok_r = ok_l && ipc::send_frame_ready(g_frame_id,
                                                    (uint64_t)(uintptr_t)slot.handle, g_ring,
                                                    1u, g_w, g_h,
                                                    view_rewrite::current_pose_id());
    if (ok_r) {
        // Advance only on a fully pushed pair (the host pairs by frameId; a
        // half-pushed pair is superseded by the next frame's same-id retry).
        g_publishes++;
        g_frame_id++;
        g_ring = (g_ring + 1) % RING_SLOTS;
    } else {
        g_ring_full++;
    }
}

void on_reset()
{
    if (g_poll_thread != nullptr) {
        // Keep the completion thread off the queries while they go away;
        // pending pairs are dropped (their textures go with the Reset).
        EnterCriticalSection(&g_poll_lock);
        for (uint32_t i = 0; i < RING_SLOTS; ++i) {
            g_pend[i].state = 0;
            if (g_qring[i]) {
                ((Release_t)(*(void ***)g_qring[i])[COM_Release])(g_qring[i]);
                g_qring[i] = nullptr;
            }
        }
        LeaveCriticalSection(&g_poll_lock);
    }
    if (g_query) {
        ((Release_t)(*(void ***)g_query)[COM_Release])(g_query);
        g_query = nullptr;
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        for (uint32_t s = 0; s < RING_SLOTS; ++s) {
            Slot &slot = g_slots[eye][s];
            if (slot.surf) {
                ((Release_t)(*(void ***)slot.surf)[COM_Release])(slot.surf);
                slot.surf = nullptr;
            }
            if (slot.tex) {
                ((Release_t)(*(void ***)slot.tex)[COM_Release])(slot.tex);
                slot.tex = nullptr;
            }
            slot.handle = nullptr;
        }
    }
    g_slots_ok = false;
    g_left_pending = false;
    g_broken = false; // retry after Reset — the probe is cheap to re-run
    g_config_pending = false; // new ring -> config re-sent on next boundary
    MC2VR_LOG("share: Reset — shared RT ring dropped");
}

void report_window()
{
    if (g_captures[0] || g_captures[1] || g_publishes || g_ring_full ||
        g_sync_timeouts || g_ipc_skipped) {
        MC2VR_LOG("share window: capturesL=%llu capturesR=%llu published=%llu "
                  "ringFull=%llu syncTimeouts=%llu noHostSkips=%llu | sync stall "
                  "avg=%.2fms max=%.2fms (n=%llu)",
                  (unsigned long long)g_captures[0],
                  (unsigned long long)g_captures[1],
                  (unsigned long long)g_publishes,
                  (unsigned long long)g_ring_full,
                  (unsigned long long)g_sync_timeouts,
                  (unsigned long long)g_ipc_skipped,
                  g_sync_n ? g_sync_sum_ms / (double)g_sync_n : 0.0, g_sync_max_ms,
                  (unsigned long long)g_sync_n);
        if (g_async) {
            MC2VR_LOG("share window: async completion pairs=%llu fence->done avg=%.2fms "
                      "max=%.2fms ringWaits=%llu",
                      (unsigned long long)g_async_done,
                      g_async_done ? g_done_sum_ms / (double)g_async_done : 0.0, g_done_max_ms,
                      (unsigned long long)g_ring_waits);
        }
    }
    g_captures[0] = 0;
    g_captures[1] = 0;
    g_publishes = 0;
    g_ring_full = 0;
    g_sync_timeouts = 0;
    g_ipc_skipped = 0;
    g_sync_n = 0;
    g_sync_sum_ms = g_sync_max_ms = 0.0;
    g_async_done = g_ring_waits = 0;
    g_done_sum_ms = g_done_max_ms = 0.0;
}

} // namespace mc2vr::eye_share

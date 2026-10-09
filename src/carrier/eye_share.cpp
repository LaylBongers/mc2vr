#include "eye_share.hpp"

#include <windows.h>
#include <d3d9.h> // type/layout constants only

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "device.hpp"
#include "ipc.hpp"
#include "log.hpp"
#include "view_rewrite.hpp"

namespace mc2vr::share {

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

bool g_enabled = false;
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
void *g_query = nullptr; // event query for the pre-publish GPU sync

uint32_t g_ring = 0;     // ring index for the frame being built
uint64_t g_frame_id = 0; // published (complete) frame ids

// ---- window counters (render thread writes; poller reads+resets) ----------
uint64_t g_captures[2] = {0, 0};
uint64_t g_publishes = 0;
uint64_t g_sync_timeouts = 0;
uint64_t g_ring_full = 0;
uint64_t g_ipc_skipped = 0; // captures while no host

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
    ((QueryIssue_t)(*(void ***)g_query)[QSLOT_Issue])(g_query, D3DISSUE_END);
    const ULONGLONG deadline = GetTickCount64() + SYNC_BUDGET_MS;
    for (;;) {
        DWORD data = 0;
        const HRESULT hr =
            ((QueryGetData_t)(*(void ***)g_query)[QSLOT_GetData])(
                g_query, &data, sizeof data, D3DGETDATA_FLUSH);
        if (hr != S_FALSE) return true; // S_OK (done) or error (logged by count)
        if (GetTickCount64() > deadline) {
            g_sync_timeouts++;
            return false;
        }
        Sleep(0);
    }
}

} // namespace

// ---- public entry points ------------------------------------------------------

void on_pass_boundary(uint32_t next_pass, void *device, void *bb)
{
    // 2 = 1->2 boundary (LEFT capture); 0 = 2->0 boundary (RIGHT capture).
    // Pass 1 (the 0->1 boundary) carries no fresh eye — the backbuffer still
    // holds the previous frame's already-presented RIGHT — so it is ignored.
    if (!g_enabled) return;
    if (next_pass != 2u && next_pass != 0u) return;
    if (!ipc::connected()) {
        // No host (permanent for the run — the carrier only connects at stage
        // 1): count the boundaries and skip. Avoids allocating ~118 MB of
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

    if (!device::blit_surfaces(bb, slot.surf)) {
        // Counted by eye_replay's window as blit failures there; nothing else
        // to do — publishing a half-copied slot is worse than skipping.
        return;
    }
    g_captures[eye]++;

    gpu_sync();

    if (ipc::send_frame_ready(g_frame_id, (uint64_t)(uintptr_t)slot.handle, g_ring,
                              eye, g_w, g_h, view_rewrite::current_pose_id())) {
        // Publish only on a successful push — a dropped message would leave
        // the host pairing a stale right eye against a new left (the host
        // pairs by frameId and shows the newest complete pair).
        if (eye == 1) { // both eyes published for this ring slot
            g_publishes++;
            g_frame_id++;
            g_ring = (g_ring + 1) % RING_SLOTS;
        }
    } else {
        g_ring_full++;
    }
}

void on_reset()
{
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
    g_broken = false; // retry after Reset — the probe is cheap to re-run
    g_config_pending = false; // new ring -> config re-sent on next boundary
    MC2VR_LOG("share: Reset — shared RT ring dropped");
}

void report_window()
{
    if (g_captures[0] || g_captures[1] || g_publishes || g_ring_full ||
        g_sync_timeouts || g_ipc_skipped) {
        MC2VR_LOG("share window: capturesL=%llu capturesR=%llu published=%llu "
                  "ringFull=%llu syncTimeouts=%llu noHostSkips=%llu",
                  (unsigned long long)g_captures[0],
                  (unsigned long long)g_captures[1],
                  (unsigned long long)g_publishes,
                  (unsigned long long)g_ring_full,
                  (unsigned long long)g_sync_timeouts,
                  (unsigned long long)g_ipc_skipped);
    }
    g_captures[0] = 0;
    g_captures[1] = 0;
    g_publishes = 0;
    g_ring_full = 0;
    g_sync_timeouts = 0;
    g_ipc_skipped = 0;
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
    MC2VR_LOG("share: eye_share=%s (pass-boundary backbuffer capture -> shared "
              "handles -> host FRAME_READY)",
              g_enabled ? "on" : "off");
    return true;
}

} // namespace mc2vr::share

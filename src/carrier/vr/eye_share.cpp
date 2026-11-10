#include "vr/eye_share.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstdio>

#include "debug/gpu_census.hpp"
#include "hooks/device.hpp"
#include "log.hpp"
#include "vr/dxvk_interop.hpp"
#include "vr/view_rewrite.hpp"
#include "vr/xr_session.hpp"

namespace mc2vr::eye_share {

namespace {

// Pairs per ring. The session thread takes each pair within about a frame of
// its fence, so one slot being written, one being copied and one spare cover
// it; each slot is 2 eyes x render size x 4 bytes (~36 MB at 2016x2240).
constexpr uint32_t kSlots = 3;
constexpr double kSlotWaitMs = 50.0;   // render thread: slot still being copied
constexpr double kResetWaitMs = 500.0; // Reset: log interval while the session thread holds a slot
constexpr double kGpuWaitMs = 50.0;    // session thread: a taken pair's GPU work

enum class State { Free, Writing, Issued, InUse };

struct Slot {
    IDirect3DTexture9 *tex[2] = {};           // owned
    IDirect3DSurface9 *surf[2] = {};          // level 0 (owned ref)
    dxvk::ID3D9VkInteropTexture *vk[2] = {};  // owned
    IDirect3DQuery9 *query = nullptr;         // the pair's fence
    State state = State::Free;
    uint64_t frame_id = 0;
    uint32_t pose_id = 0;
};

// Guards the slot states and metadata; the render thread and the session
// thread wait on g_cv for each other.
SRWLOCK g_lock = SRWLOCK_INIT;
CONDITION_VARIABLE g_cv = CONDITION_VARIABLE_INIT;
Slot g_slots[kSlots];
bool g_ok = false;        // ring created (render thread writes, under g_lock)
bool g_broken = false;    // creation failed — off until the next Reset
bool g_resetting = false; // Reset in progress: the session thread takes nothing
IDirect3DDevice9 *g_device = nullptr;
uint32_t g_w = 0, g_h = 0;
VkFormat g_format = VK_FORMAT_UNDEFINED;

// Render thread only.
uint32_t g_ring = 0;        // slot of the frame being captured
uint64_t g_next_frame = 1;  // id of the next issued pair
bool g_left_pending = false;

// Window counters (approximate across threads; logs only).
uint64_t g_captures[2] = {};
uint64_t g_issued = 0, g_taken_fresh = 0, g_taken_late = 0, g_dropped = 0;
uint64_t g_slot_waits = 0, g_slot_timeouts = 0, g_gpu_timeouts = 0, g_no_session = 0;
double g_gpu_sum_ms = 0.0, g_gpu_max_ms = 0.0;

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

void release_resources()
{
    for (Slot &s : g_slots) {
        for (int e = 0; e < 2; e++) {
            if (s.vk[e]) s.vk[e]->Release();
            if (s.surf[e]) s.surf[e]->Release();
            if (s.tex[e]) s.tex[e]->Release();
            s.vk[e] = nullptr;
            s.surf[e] = nullptr;
            s.tex[e] = nullptr;
        }
        if (s.query) s.query->Release();
        s.query = nullptr;
        s.state = State::Free;
    }
}

bool create_resources(IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    D3DSURFACE_DESC desc = {};
    if (FAILED(source->GetDesc(&desc))) {
        MC2VR_LOG("share: front GetDesc failed — eye images off");
        return false;
    }
    for (Slot &s : g_slots) {
        for (int e = 0; e < 2; e++) {
            // RENDERTARGET: the captures are StretchRect blits. The shared
            // handle keeps the VkImage fixed (eye_share.hpp); it is not used.
            HANDLE shared = nullptr;
            HRESULT hr = device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format,
                                               D3DPOOL_DEFAULT, &s.tex[e], &shared);
            if (FAILED(hr) || s.tex[e] == nullptr) {
                MC2VR_LOG("share: CreateTexture(%ux%u fmt %u, shared RT) FAILED hr=%08lx — eye images off. "
                          "mc2vr needs DXVK's d3d9.dll (docs/platforms.md)",
                          desc.Width, desc.Height, (unsigned)desc.Format, (unsigned long)hr);
                return false;
            }
            if (FAILED(s.tex[e]->GetSurfaceLevel(0, &s.surf[e])) ||
                FAILED(s.tex[e]->QueryInterface(dxvk::kIID_InteropTexture, reinterpret_cast<void **>(&s.vk[e]))) ||
                s.vk[e] == nullptr) {
                MC2VR_LOG("share: surface / ID3D9VkInteropTexture unavailable — eye images off (not DXVK?)");
                return false;
            }
        }
        if (FAILED(device->CreateQuery(D3DQUERYTYPE_EVENT, &s.query)) || s.query == nullptr) {
            MC2VR_LOG("share: CreateQuery(EVENT) failed — eye images off");
            return false;
        }
    }
    VkImage img = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    if (FAILED(g_slots[0].vk[0]->GetVulkanImageInfo(&img, &layout, &info)) || img == VK_NULL_HANDLE) {
        MC2VR_LOG("share: GetVulkanImageInfo failed — eye images off");
        return false;
    }
    g_device = device;
    g_w = desc.Width;
    g_h = desc.Height;
    g_format = info.format;
    MC2VR_LOG("share: eye ring ready: %u pairs %ux%u, D3DFMT %u = VkFormat %d, layout %d, usage 0x%x",
              kSlots, g_w, g_h, (unsigned)desc.Format, (int)info.format, (int)layout, (unsigned)info.usage);
    return true;
}

bool ensure_resources(IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    if (g_ok) {
        return true;
    }
    if (g_broken || device == nullptr || source == nullptr) {
        return false;
    }
    if (!create_resources(device, source)) {
        release_resources();
        g_broken = true;
        return false;
    }
    AcquireSRWLockExclusive(&g_lock);
    g_ok = true;
    g_resetting = false;
    ReleaseSRWLockExclusive(&g_lock);
    return true;
}

// Render thread, LEFT: claims the frame's slot. False when the session thread
// still copies from it after the bounded wait (the frame is not captured).
bool claim_slot()
{
    AcquireSRWLockExclusive(&g_lock);
    Slot &s = g_slots[g_ring];
    if (s.state == State::InUse) {
        g_slot_waits++;
        const double t0 = qpc_ms();
        while (s.state == State::InUse) {
            const double left = kSlotWaitMs - (qpc_ms() - t0);
            if (left <= 0.0) break;
            SleepConditionVariableSRW(&g_cv, &g_lock, (DWORD)left + 1, 0);
        }
    }
    bool ok = s.state != State::InUse;
    if (ok) {
        if (s.state == State::Issued) {
            g_dropped++;  // never taken (session not consuming)
        }
        s.state = State::Writing;
    } else {
        g_slot_timeouts++;
    }
    ReleaseSRWLockExclusive(&g_lock);
    return ok;
}

void set_state(uint32_t slot, State st)
{
    AcquireSRWLockExclusive(&g_lock);
    g_slots[slot].state = st;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
}

} // namespace

// ---- render thread --------------------------------------------------------------

void on_pass_boundary(uint32_t next_pass, IDirect3DDevice9 *device, IDirect3DSurface9 *source)
{
    // 2 = 1->2 boundary (LEFT capture); 0 = 2->0 boundary (RIGHT capture).
    // The 0->1 boundary carries no fresh eye.
    if (next_pass != 2u && next_pass != 0u) return;
    if (source == nullptr) {
        // Front not owned (boot, mid-Reset) or the post-Reset transition
        // frame: no capture, the session re-submits the previous pair.
        g_left_pending = false;
        return;
    }
    const uint32_t eye = next_pass == 2 ? 0u : 1u;
    if (eye == 0) {
        g_left_pending = false;
        if (!xr_session::presenting()) {
            g_no_session++;  // nobody would take the pair
            return;
        }
    } else if (!g_left_pending) {
        return;  // no LEFT this frame: a lone RIGHT can never complete a pair
    }
    if (!ensure_resources(device, source)) return;
    if (eye == 0 && !claim_slot()) return;

    Slot &s = g_slots[g_ring];
    gpu_census::carrier_begin();
    const bool blitted = device::blit_surfaces(source, s.surf[eye]);
    gpu_census::carrier_end();
    if (!blitted) {
        // Counted by eye_replay as a blit failure; a half-copied pair is
        // worse than none.
        g_left_pending = false;
        set_state(g_ring, State::Free);
        return;
    }
    g_captures[eye]++;
    if (eye == 0) {
        g_left_pending = true;
        return;
    }
    g_left_pending = false;

    // Fence the pair and submit it (DXVK never submits a query's work
    // without the FLUSH); the session thread polls the fence.
    s.query->Issue(D3DISSUE_END);
    DWORD data = 0;
    s.query->GetData(&data, sizeof data, D3DGETDATA_FLUSH);
    AcquireSRWLockExclusive(&g_lock);
    s.frame_id = g_next_frame++;
    s.pose_id = view_rewrite::current_pose_id();  // one pose per frame (sampled at pass 1)
    s.state = State::Issued;
    ReleaseSRWLockExclusive(&g_lock);
    WakeAllConditionVariable(&g_cv);
    g_issued++;
    g_ring = (g_ring + 1) % kSlots;
}

void on_reset()
{
    // The session thread may be waiting on a fence or recording a copy from
    // a slot: let it finish, keep it off the ring, then wait out the GPU
    // copies (DXVK does not know about them) before the images go.
    // The session thread's hold is bounded by its own timeouts, and it needs
    // nothing from this thread to finish (its GetData takes DXVK's device
    // lock, which isn't held here), so this waits until it lets go.
    AcquireSRWLockExclusive(&g_lock);
    g_resetting = true;
    for (uint32_t waited = 0;; waited++) {
        bool busy = false;
        for (const Slot &s : g_slots) busy = busy || s.state == State::InUse;
        if (!busy) break;
        if (waited % 10 == 9) {
            MC2VR_LOG("share: Reset — still waiting for the session thread to release a pair");
        }
        SleepConditionVariableSRW(&g_cv, &g_lock, (DWORD)kResetWaitMs / 10, 0);
    }
    g_ok = false;
    ReleaseSRWLockExclusive(&g_lock);
    xr_session::wait_gpu_copies();

    release_resources();
    g_left_pending = false;
    g_broken = false;  // retry after the Reset
    MC2VR_LOG("share: Reset — eye ring dropped");
}

void report_window()
{
    if (g_captures[0] || g_captures[1] || g_issued || g_slot_timeouts || g_gpu_timeouts) {
        MC2VR_LOG("share window: capturesL=%llu capturesR=%llu pairs=%llu taken fresh=%llu late=%llu "
                  "dropped=%llu | slot waits=%llu timeouts=%llu | fence->taken avg=%.2fms max=%.2fms "
                  "gpuTimeouts=%llu",
                  (unsigned long long)g_captures[0], (unsigned long long)g_captures[1],
                  (unsigned long long)g_issued, (unsigned long long)g_taken_fresh,
                  (unsigned long long)g_taken_late, (unsigned long long)g_dropped,
                  (unsigned long long)g_slot_waits, (unsigned long long)g_slot_timeouts,
                  (g_taken_fresh + g_taken_late) ? g_gpu_sum_ms / (double)(g_taken_fresh + g_taken_late) : 0.0,
                  g_gpu_max_ms, (unsigned long long)g_gpu_timeouts);
    }
    g_captures[0] = g_captures[1] = 0;
    g_issued = g_taken_fresh = g_taken_late = g_dropped = 0;
    g_slot_waits = g_slot_timeouts = g_gpu_timeouts = g_no_session = 0;
    g_gpu_sum_ms = g_gpu_max_ms = 0.0;
}

// ---- session thread ---------------------------------------------------------------

Take take_pair(uint64_t after_frame, uint32_t want_pose, uint32_t timeout_ms, Pair *out)
{
    const double t0 = qpc_ms();
    AcquireSRWLockExclusive(&g_lock);
    int pick = -1;
    Take kind = Take::None;
    for (;;) {
        // The newest issued pair; anything older is superseded.
        int newest = -1;
        for (uint32_t i = 0; i < kSlots; i++) {
            const Slot &s = g_slots[i];
            if (s.state == State::Issued && s.frame_id > after_frame &&
                (newest < 0 || s.frame_id > g_slots[newest].frame_id)) {
                newest = (int)i;
            }
        }
        const bool ready = newest >= 0 && (g_slots[newest].pose_id == 0 ||
                                           (int32_t)(g_slots[newest].pose_id - want_pose) >= 0);
        const double left = (double)timeout_ms - (qpc_ms() - t0);
        if (!g_ok || g_resetting) {
            break;
        }
        if (ready || (newest >= 0 && left <= 0.0)) {
            pick = newest;
            kind = ready ? Take::Fresh : Take::Late;
            break;
        }
        if (left <= 0.0) {
            break;
        }
        SleepConditionVariableSRW(&g_cv, &g_lock, (DWORD)left + 1, 0);
    }
    if (pick < 0) {
        ReleaseSRWLockExclusive(&g_lock);
        return Take::None;
    }
    for (uint32_t i = 0; i < kSlots; i++) {
        Slot &s = g_slots[i];
        if ((int)i != pick && s.state == State::Issued && s.frame_id < g_slots[pick].frame_id) {
            s.state = State::Free;
            g_dropped++;
        }
    }
    Slot &s = g_slots[pick];
    s.state = State::InUse;
    out->slot = (uint32_t)pick;
    out->frame_id = s.frame_id;
    out->pose_id = s.pose_id;
    out->w = g_w;
    out->h = g_h;
    out->format = g_format;
    out->device = g_device;
    ReleaseSRWLockExclusive(&g_lock);

    // The pair's GPU work (the device is MULTITHREADED, so polling from this
    // thread is safe). Done = DXVK has submitted it, so our copy, submitted
    // later on the same queue, reads the finished images.
    const double g0 = qpc_ms();
    bool done = false;
    for (;;) {
        DWORD data = 0;
        const HRESULT hr = s.query->GetData(&data, sizeof data, 0);
        if (hr != S_FALSE) {
            done = hr == S_OK;
            break;
        }
        if (qpc_ms() - g0 > kGpuWaitMs) break;
        SwitchToThread();
    }
    for (int e = 0; done && e < 2; e++) {
        done = SUCCEEDED(s.vk[e]->GetVulkanImageInfo(&out->eye[e].image, &out->eye[e].layout, nullptr)) &&
               out->eye[e].image != VK_NULL_HANDLE;
    }
    if (!done) {
        g_gpu_timeouts++;
        set_state(out->slot, State::Free);
        return Take::None;
    }
    const double ms = qpc_ms() - g0;
    g_gpu_sum_ms += ms;
    if (ms > g_gpu_max_ms) g_gpu_max_ms = ms;
    (kind == Take::Fresh ? g_taken_fresh : g_taken_late)++;
    return kind;
}

void release_pair(const Pair &p)
{
    set_state(p.slot, State::Free);
}

} // namespace mc2vr::eye_share

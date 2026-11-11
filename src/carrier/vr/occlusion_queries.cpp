// The carrier-owned per-eye occlusion-query ring — see occlusion_queries.hpp.

#include "vr/occlusion_queries.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "game/dx9.hpp"
#include "game/pg_primitive.hpp"
#include "game/render_shell.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"

namespace mc2vr::occlusion_queries {

namespace {

// Window counters. GetData runs on the engine's polling thread, the rest on
// the render thread.
std::atomic<uint32_t> g_poll_visible{0}, g_poll_hidden{0}, g_poll_pending{0};
std::atomic<uint32_t> g_poll_left_only{0};  // hidden for the right eye, seen by the left
uint32_t g_issued[2] = {};                  // query END per eye (LEFT, RIGHT)
uint32_t g_boxes = 0, g_boxes_no_vcd = 0;
uint32_t g_created = 0, g_released = 0, g_create_failed = 0;
uint32_t g_ring_full = 0;                     // boxes given no id: the ring was full
std::atomic<uint32_t> g_aliased_reads{0};     // reads of a slot reissued since its owner's issue

// The engine's frame number the readers stamp visibility with
// (ObjectOcclusionQueries_PollAndMarkVisible: [g_RenderShellPtrB + 0x2be4]).
constexpr uintptr_t kShellFrame = 0x2be4;
uint32_t frame_now()
{
    uint32_t f;
    std::memcpy(&f, game::g_RenderShellPtrB::get() + kShellFrame, 4);
    return f;
}

// Per slot: the frame its id was last handed out (allocation and reads run
// in the scene build, render thread).
uint32_t g_handed_out[kRingSize];
bool g_handed_out_valid[kRingSize];

// Local copies: DEFINE_GUID in d3d9.h only declares them (no dxguid link).
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kIID_IDirect3DQuery9 = {0xd9771460, 0xa695, 0x4f26,
                                       {0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc}};

// One ring slot: a query per eye, eye_[0] = LEFT (pass 1), eye_[1] = RIGHT
// (pass 2). Static, never freed; the engine never AddRefs or Releases the
// entries it reads through g_OcclusionQueryArray.
class Slot final : public IDirect3DQuery9 {
public:
    void set_index(uint32_t i) { index_ = i; }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_IDirect3DQuery9)) {
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
    ULONG STDMETHODCALLTYPE Release() override { return 1; }

    // ---- IDirect3DQuery9 ----
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override
    {
        if (eye_[1] == nullptr) {
            return D3DERR_INVALIDCALL;
        }
        return eye_[1]->GetDevice(device);
    }
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return D3DQUERYTYPE_OCCLUSION; }
    DWORD STDMETHODCALLTYPE GetDataSize() override { return sizeof(DWORD); }

    // Pass 1 = LEFT, pass 2 = RIGHT; outside a submit (never seen) RIGHT.
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override
    {
        const int e = eye_replay::current_pass() == 1 ? 0 : 1;
        IDirect3DQuery9 *const q = eye_[e];
        if (q == nullptr) {
            return S_OK;  // no queries (released): the box is drawn unmeasured
        }
        if (flags & D3DISSUE_END) {
            issued_[e] = true;
            g_issued[e]++;
        }
        return q->Issue(flags);
    }

    // Not ready (S_FALSE) while either issued eye is, or while no eye has
    // been issued; otherwise S_OK with the larger eye's pixel count: nonzero
    // iff either eye saw the box (the pollers' test), and a per-eye magnitude
    // for OcclusionQuery_GetPixelCount's callers, one of which needs >= 64
    // pixels (a sum would roughly double that).
    HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override
    {
        // Its owner reads two frames after its issue; a younger hand-out
        // means the slot was reissued in between (another object's box).
        if (g_handed_out_valid[index_] && frame_now() - g_handed_out[index_] < kIdLifetime - 1) {
            g_aliased_reads++;
        }
        DWORD px[2] = {0, 0};
        bool any = false;
        for (int e = 0; e < 2; e++) {
            if (!issued_[e] || eye_[e] == nullptr) continue;
            const HRESULT hr = eye_[e]->GetData(&px[e], sizeof(DWORD), flags);
            if (hr != S_OK) {
                g_poll_pending++;
                return hr;
            }
            any = true;
        }
        if (!any) {
            return S_FALSE;
        }
        const DWORD px_max = px[0] > px[1] ? px[0] : px[1];
        if (px_max != 0) {
            g_poll_visible++;
            if (px[1] == 0) {
                g_poll_left_only++;
            }
        } else {
            g_poll_hidden++;
        }
        if (data != nullptr && size != 0) {
            memcpy(data, &px_max, size < sizeof(px_max) ? size : sizeof(px_max));
        }
        return S_OK;
    }

    bool create(IDirect3DDevice9 *dev)
    {
        for (int e = 0; e < 2; e++) {
            if (eye_[e] != nullptr) continue;
            if (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &eye_[e])) || eye_[e] == nullptr) {
                eye_[e] = nullptr;
                return false;
            }
        }
        return true;
    }
    void release()
    {
        for (int e = 0; e < 2; e++) {
            if (eye_[e] != nullptr) {
                eye_[e]->Release();
                eye_[e] = nullptr;
            }
            issued_[e] = false;
        }
    }

private:
    IDirect3DQuery9 *eye_[2] = {nullptr, nullptr};
    bool issued_[2] = {false, false};
    uint32_t index_ = 0;
};

Slot g_slots[kRingSize];
IDirect3DQuery9 *g_ring[kRingSize];
bool g_have_queries = false;

} // namespace

IDirect3DQuery9 **ring()
{
    if (g_ring[0] == nullptr) {
        for (uint32_t i = 0; i < kRingSize; i++) {
            g_slots[i].set_index(i);
            g_ring[i] = &g_slots[i];
        }
    }
    return g_ring;
}

uint32_t on_id_allocated(uint32_t id)
{
    if (id >= kRingSize) {
        return id;  // not ours (cannot happen with the mask)
    }
    const uint32_t now = frame_now();
    if (g_handed_out_valid[id] && now - g_handed_out[id] < kIdLifetime) {
        g_ring_full++;
        return kNoQuery;
    }
    g_handed_out[id] = now;
    g_handed_out_valid[id] = true;
    return id;
}

void on_release_render_targets()
{
    if (!g_have_queries) {
        return;
    }
    for (Slot &s : g_slots) s.release();
    g_have_queries = false;
    g_released++;
}

void on_submit_begin()
{
    const uint8_t *const lti = game::g_LtiRenderer::get();
    if (lti == nullptr || g_have_queries) {
        return;
    }
    const uint32_t flags = *reinterpret_cast<const uint32_t *>(lti + game::kLtiRendererFlags);
    IDirect3DDevice9 *const dev = game::lti_device();
    // Our ring is in place once the engine set up its own (ring()); queries
    // off = the ring is never read.
    if (((flags >> 4) & 1) == 0 || dev == nullptr || game::g_OcclusionQueryArray::get() != ring()) {
        return;
    }
    for (Slot &s : g_slots) {
        if (!s.create(dev)) {
            g_create_failed++;
        }
    }
    g_have_queries = true;
    g_created++;
}

void on_box_draw(uint32_t vcd_regs)
{
    g_boxes++;
    if (vcd_regs == 0) {
        g_boxes_no_vcd++;
    }
}

void report_window()
{
    // Oracles: created = 1 once after boot / each render-target setup (0 per
    // window after that), createFailed = 0; every box is drawn in both
    // passes, so issued left == right — checked as pairDrift (left − right
    // since start): per window the two can differ by one frame that straddles
    // the report, so only a lasting pairDrift != 0 is a real desync;
    // boxesNoVcd = 0 (else those boxes are drawn with the mono camera); polls
    // hidden > 0 (occlusion actually culls; 0 with polls visible > 0 = the
    // boxes always pass, e.g. drawn without depth); leftOnly = objects a
    // single query would have culled wrongly; ringFull = boxes given no id
    // because every id was still referenced (raise kRingSize); aliasedReads = 0
    // (else some object reads another object's box: wrong hides).
    static int64_t s_drift = 0;
    s_drift += (int64_t)g_issued[0] - (int64_t)g_issued[1];
    MC2VR_LOG("occlusion: window: ring=%u created=%u released=%u createFailed=%u issuedL=%u issuedR=%u "
              "pairDrift=%lld boxes=%u boxesNoVcd=%u ringFull=%u | polls visible=%u hidden=%u pending=%u "
              "leftOnly=%u aliasedReads=%u",
              kRingSize, g_created, g_released, g_create_failed, g_issued[0], g_issued[1], (long long)s_drift,
              g_boxes, g_boxes_no_vcd, g_ring_full, g_poll_visible.exchange(0), g_poll_hidden.exchange(0),
              g_poll_pending.exchange(0), g_poll_left_only.exchange(0), g_aliased_reads.exchange(0));
    g_created = g_released = g_create_failed = g_ring_full = 0;
    g_issued[0] = g_issued[1] = 0;
    g_boxes = g_boxes_no_vcd = 0;
}

} // namespace mc2vr::occlusion_queries

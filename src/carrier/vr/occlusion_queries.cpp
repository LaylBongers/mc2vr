// Per-eye occlusion queries — see occlusion_queries.hpp.

#include "vr/occlusion_queries.hpp"

#include <windows.h>
#include <d3d9.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "game/dx9.hpp"
#include "game/pg_primitive.hpp"
#include "log.hpp"
#include "vr/eye_replay.hpp"

namespace mc2vr::occlusion_queries {

namespace {

using game::kOcclusionQueryCount;

// Window counters. GetData runs on the engine's polling thread, the rest on
// the render thread.
std::atomic<uint32_t> g_poll_visible{0}, g_poll_hidden{0}, g_poll_pending{0};
std::atomic<uint32_t> g_poll_left_only{0};  // hidden for the right eye, seen by the left
uint32_t g_issued[2] = {};                  // query END per eye (LEFT, RIGHT)
uint32_t g_boxes = 0, g_boxes_no_vcd = 0;
uint32_t g_wrapped = 0, g_create_failed = 0;

// Local copies: DEFINE_GUID in d3d9.h only declares them (no dxguid link).
constexpr GUID kIID_IUnknown = {0x00000000, 0x0000, 0x0000, {0xc0, 0, 0, 0, 0, 0, 0, 0x46}};
constexpr GUID kIID_IDirect3DQuery9 = {0xd9771460, 0xa695, 0x4f26,
                                       {0xbb, 0xd3, 0x27, 0xb8, 0x40, 0xb5, 0x41, 0xcc}};

class QueryPair;
QueryPair *g_pairs[kOcclusionQueryCount] = {};  // the pair installed per ring slot

// One ring slot's query for both eyes. eye_[0] = LEFT (carrier-created),
// eye_[1] = RIGHT (the engine's own; this pair owns the engine's reference).
class QueryPair final : public IDirect3DQuery9 {
public:
    QueryPair(IDirect3DQuery9 *left, IDirect3DQuery9 *right, uint32_t slot)
        : eye_{left, right}, slot_(slot)
    {
    }

    // ---- IUnknown ----
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **out) override
    {
        if (out == nullptr) {
            return E_POINTER;
        }
        if (IsEqualGUID(riid, kIID_IUnknown) || IsEqualGUID(riid, kIID_IDirect3DQuery9)) {
            AddRef();
            *out = this;
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs_; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs_;
        if (n == 0) {
            // The engine released its ring entry (render targets released,
            // e.g. before a device Reset): both eyes go with it.
            if (g_pairs[slot_] == this) {
                g_pairs[slot_] = nullptr;
            }
            eye_[0]->Release();
            eye_[1]->Release();
            delete this;
        }
        return n;
    }

    // ---- IDirect3DQuery9 ----
    HRESULT STDMETHODCALLTYPE GetDevice(IDirect3DDevice9 **device) override
    {
        return eye_[1]->GetDevice(device);
    }
    D3DQUERYTYPE STDMETHODCALLTYPE GetType() override { return eye_[1]->GetType(); }
    DWORD STDMETHODCALLTYPE GetDataSize() override { return eye_[1]->GetDataSize(); }

    // Pass 1 = LEFT, pass 2 = RIGHT; outside a submit (never seen) RIGHT,
    // the engine's own query.
    HRESULT STDMETHODCALLTYPE Issue(DWORD flags) override
    {
        const int e = eye_replay::current_pass() == 1 ? 0 : 1;
        if (flags & D3DISSUE_END) {
            issued_[e].store(true, std::memory_order_relaxed);
            g_issued[e]++;
        }
        return eye_[e]->Issue(flags);
    }

    // Not ready (S_FALSE) or failed while either issued eye is; otherwise
    // S_OK with the larger eye's pixel count: nonzero iff either eye saw the
    // box (the pollers' test), and a per-eye magnitude for
    // OcclusionQuery_GetPixelCount's callers, one of which needs >= 64 pixels
    // (a sum would roughly double that). An eye that was never issued is left
    // out (its query has no result).
    HRESULT STDMETHODCALLTYPE GetData(void *data, DWORD size, DWORD flags) override
    {
        const bool left = issued_[0].load(std::memory_order_relaxed);
        if (!left || !issued_[1].load(std::memory_order_relaxed)) {
            return eye_[left ? 0 : 1]->GetData(data, size, flags);
        }
        DWORD px[2] = {0, 0};
        for (int e = 0; e < 2; e++) {
            const HRESULT hr = eye_[e]->GetData(&px[e], sizeof(DWORD), flags);
            if (hr != S_OK) {
                g_poll_pending++;
                return hr;
            }
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

private:
    IDirect3DQuery9 *eye_[2];
    uint32_t slot_;
    std::atomic<ULONG> refs_{1};  // the engine's ring reference
    std::atomic<bool> issued_[2] = {false, false};  // set on the render thread, read by GetData
};

} // namespace

void on_submit_begin()
{
    const uint8_t *const lti = game::g_LtiRenderer::get();
    if (lti == nullptr) {
        return;
    }
    const uint32_t flags = *reinterpret_cast<const uint32_t *>(lti + game::kLtiRendererFlags);
    IDirect3DQuery9 **const ring = game::g_OcclusionQueryArray::get();
    IDirect3DDevice9 *const dev = game::lti_device();
    if (((flags >> 4) & 1) == 0 || ring == nullptr || dev == nullptr) {
        return;  // occlusion queries off: the ring is never read
    }
    for (uint32_t i = 0; i < kOcclusionQueryCount; i++) {
        IDirect3DQuery9 *const q = ring[i];
        if (q == nullptr || q == g_pairs[i]) {
            continue;  // released, or already ours
        }
        IDirect3DQuery9 *left = nullptr;
        if (FAILED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &left)) || left == nullptr) {
            g_create_failed++;  // stays the engine's single query: right-eye counts only
            continue;
        }
        QueryPair *const pair = new QueryPair(left, q, i);
        g_pairs[i] = pair;
        ring[i] = pair;
        g_wrapped++;
    }
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
    // Oracles: wrapped = 1024 once after each SetupRenderTargets (0 per window
    // after that); issuedL == issuedR (every box is drawn in both passes);
    // boxesNoVcd = 0 (else those boxes are drawn with the mono camera); polls
    // hidden > 0 (occlusion actually culls; 0 with polls visible > 0 = the
    // boxes always pass, e.g. drawn without depth); leftOnly = objects the
    // single-query version would have culled wrongly.
    MC2VR_LOG("occlusion: window: wrapped=%u createFailed=%u issuedL=%u issuedR=%u boxes=%u "
              "boxesNoVcd=%u | polls visible=%u hidden=%u pending=%u leftOnly=%u",
              g_wrapped, g_create_failed, g_issued[0], g_issued[1], g_boxes, g_boxes_no_vcd,
              g_poll_visible.exchange(0), g_poll_hidden.exchange(0), g_poll_pending.exchange(0),
              g_poll_left_only.exchange(0));
    g_wrapped = g_create_failed = 0;
    g_issued[0] = g_issued[1] = 0;
    g_boxes = g_boxes_no_vcd = 0;
}

} // namespace mc2vr::occlusion_queries

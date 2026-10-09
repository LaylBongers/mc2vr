#include "shared_eyes.hpp"

#include <windows.h>
#include <d3d11.h>

#include <cstdint>
#include <cstdio>
#include <vector>

#include "log.hpp"

namespace seyes {

namespace {

constexpr uint32_t MIRROR_W = 1280, MIRROR_H = 720;
constexpr size_t OPEN_CACHE_MAX = 64;  // handles ever opened (Reset-proof:
                                       // new rings get new handles)

bool g_inited = false;
HWND g_hwnd = nullptr;
ID3D11Device* g_dev = nullptr;
ID3D11DeviceContext* g_ctx = nullptr;

struct Entry {
    uint64_t handle = 0;
    ID3D11Texture2D* tex = nullptr;   // opened shared texture
    ID3D11Texture2D* stg = nullptr;   // same-desc CPU-read staging
    ID3D11ShaderResourceView* srv = nullptr;  // UNORM-cast view (blit)
    uint32_t w = 0, h = 0;
};
std::vector<Entry> g_cache;

struct Latest {
    Entry* e = nullptr;
    uint64_t frameId = 0;
    uint32_t poseId = 0;
    bool fresh = false;
};
Latest g_latest[2];

std::vector<uint8_t> g_scratch;  // packed BGRA row buffer (GDI wants tight rows)
uint64_t g_zero_handles = 0;
uint64_t g_open_fails = 0;
uint64_t g_received[2] = {0, 0};
uint64_t g_drawn[2] = {0, 0};
bool g_stats_armed = false;
uint32_t g_stat_eyes[2][2] = {{0, 0}, {0, 0}};  // per window: received, drawn

LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
        case WM_PAINT: {
            PAINTSTRUCT ps;
            BeginPaint(h, &ps);
            EndPaint(h, &ps);
            // The next pump() redraws the panes; nothing to do here.
            return 0;
        }
        case WM_DESTROY:
            g_hwnd = nullptr;
            return 0;
        default:
            return DefWindowProcW(h, m, w, l);
    }
}

// (drawing lives below — one pitch-aware implementation)

}  // namespace

namespace {

// Draw one pane: staging-read the shared texture and StretchDIBits it scaled
// into the pane rect. GDI 32bpp BI_RGB is BGRA with alpha ignored — the shared
// textures are B8G8R8A8/B8G8R8X8 UNORM (X8R8G8B8 opens as the latter,
// probe-proven), so the rows pass through untouched. GDI assumes tight rows;
// re-pack only when the staging pitch is padded.
void draw_pane_impl(HDC dc, const RECT& pane, const Entry& e) {
    // Shared texture -> staging (the copy I managed to forget in the first
    // live run: Map on a never-copied staging texture reads its zero-init
    // contents — the black-mirror bug) ...
    g_ctx->CopyResource(e.stg, e.tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(g_ctx->Map(e.stg, 0, D3D11_MAP_READ, 0, &m))) return;

    const size_t tight = (size_t)e.w * 4;
    const uint8_t* src = (const uint8_t*)m.pData;
    const uint8_t* bits = src;
    if (m.RowPitch != tight) {
        if (g_scratch.size() < tight * e.h) g_scratch.resize(tight * e.h);
        for (uint32_t y = 0; y < e.h; ++y) {
            memcpy(g_scratch.data() + (size_t)y * tight, src + (size_t)y * m.RowPitch,
                   tight);
        }
        bits = g_scratch.data();
    }

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = (LONG)e.w;
    bi.bmiHeader.biHeight = -(LONG)e.h;  // top-down (D3D row order)
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    StretchDIBits(dc, pane.left, pane.top, pane.right - pane.left,
                  pane.bottom - pane.top, 0, 0, e.w, e.h, bits, &bi,
                  DIB_RGB_COLORS, SRCCOPY);
    g_ctx->Unmap(e.stg, 0);
}

}  // namespace

bool init(d3d::Device* d) {
    if (g_inited) return true;
    if (d == nullptr || d->dev == nullptr || d->ctx == nullptr) {
        hostlog::write("seyes: no D3D11 device — shared-eye mirror off");
        return false;
    }
    g_dev = d->dev;
    g_ctx = d->ctx;

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"mc2vr_mirror";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassW(&wc);
    g_hwnd = CreateWindowExW(WS_EX_TOPMOST, wc.lpszClassName,
                             L"mc2vr host mirror  [ L | R ]", WS_OVERLAPPEDWINDOW,
                             CW_USEDEFAULT, CW_USEDEFAULT, MIRROR_W, MIRROR_H, nullptr,
                             nullptr, wc.hInstance, nullptr);
    if (g_hwnd == nullptr) {
        hostlog::write("seyes: mirror window creation failed (%lu)", GetLastError());
        return false;
    }
    ShowWindow(g_hwnd, SW_SHOWNOACTIVATE);
    // Fixed capacity up front: Entry pointers stored in g_latest[].e must stay
    // stable across push_backs (a realloc would dangle them; later Resets mint
    // new handles and keep pushing).
    g_cache.reserve(OPEN_CACHE_MAX);
    g_inited = true;
    hostlog::write("seyes: mirror window up (%ux%u); waiting for FRAME_READY",
                   MIRROR_W, MIRROR_H);
    return true;
}

void on_config(uint32_t width, uint32_t height, uint32_t format) {
    hostlog::write("seyes: carrier ring config %ux%u fmt=%u (D3DFMT; opened "
                   "textures report the DXGI mapping)",
                   width, height, format);
}

void on_frame_ready(uint64_t frameId, uint64_t handle, uint32_t slot,
                    uint32_t eye, uint32_t w, uint32_t h, uint32_t poseId) {
    (void)slot;
    if (handle == 0) {
        if (g_zero_handles++ == 0) hostlog::write("seyes: FRAME_READY with handle 0 ignored");
        return;
    }
    if (eye > 1) return;
    g_received[eye]++;
    if (!g_inited) return;  // e.g. --mock selftest traffic; nothing to show

    // Find or open the shared texture (handles are stable per ring; the
    // carrier's Reset drops a ring and mints new handles — old entries simply
    // stay cached and stop being referenced).
    Entry* e = nullptr;
    for (auto& c : g_cache) {
        if (c.handle == handle) { e = &c; break; }
    }
    if (e == nullptr) {
        if (g_cache.size() >= OPEN_CACHE_MAX) {
            static bool warned = false;
            if (!warned) { warned = true; hostlog::write("seyes: open cache full"); }
            return;
        }
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = g_dev->OpenSharedResource((HANDLE)(UINT_PTR)handle,
                                               __uuidof(ID3D11Texture2D), (void**)&tex);
        if (FAILED(hr) || tex == nullptr) {
            g_open_fails++;
            hostlog::write("seyes: OpenSharedResource(0x%llx) FAILED hr=0x%08lx",
                           (unsigned long long)handle, (unsigned long)hr);
            return;
        }
        D3D11_TEXTURE2D_DESC d;
        tex->GetDesc(&d);
        if (d.Width != w || d.Height != h) {
            hostlog::write("seyes: dims mismatch handle 0x%llx (%ux%u vs %ux%u)",
                           (unsigned long long)handle, d.Width, d.Height, w, h);
            tex->Release();
            return;
        }
        D3D11_TEXTURE2D_DESC sd = d;
        sd.Usage = D3D11_USAGE_STAGING;
        sd.BindFlags = 0;
        sd.MiscFlags = 0;
        sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Texture2D* stg = nullptr;
        if (FAILED(g_dev->CreateTexture2D(&sd, nullptr, &stg))) {
            hostlog::write("seyes: staging creation failed for handle 0x%llx",
                           (unsigned long long)handle);
            tex->Release();
            return;
        }
        // Plain-UNORM-cast SRV on the BGRA8-family texture (87 or 88
        // typed — both cast to 87). Raw bytes, no sRGB decode: the blit
        // shader passes them through and the compositor decodes the sRGB
        // swapchain — the intended path for display-referred finals. Failure
        // here only disables the OpenXR submission for this texture (the
        // mirror keeps working off e.tex).
        ID3D11ShaderResourceView* srv = nullptr;
        D3D11_SHADER_RESOURCE_VIEW_DESC sv = {};
        sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        sv.Texture2D.MostDetailedMip = 0;
        sv.Texture2D.MipLevels = 1;
        if (FAILED(g_dev->CreateShaderResourceView(tex, &sv, &srv))) {
            hostlog::write("seyes: SRV creation failed for handle 0x%llx "
                           "(hr-ignored, texture stays mirror-only)",
                           (unsigned long long)handle);
        }
        Entry ne;
        ne.handle = handle;
        ne.tex = tex;
        ne.stg = stg;
        ne.srv = srv;
        ne.w = w;
        ne.h = h;
        g_cache.push_back(ne);
        e = &g_cache.back();
        hostlog::write("seyes: opened handle 0x%llx (eye %u, %ux%u, DXGI fmt %u) — "
                       "%zu cached",
                       (unsigned long long)handle, eye, w, h, d.Format, g_cache.size());
    }

    g_latest[eye].e = e;
    g_latest[eye].frameId = frameId;
    g_latest[eye].poseId = poseId;
    g_latest[eye].fresh = true;
}

void pump() {
    if (!g_inited) return;

    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) return;
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (!g_hwnd) return;  // user closed it — keep the pipes open, just stop drawing
    if (!g_latest[0].fresh && !g_latest[1].fresh) return;

    RECT rc;
    if (!GetClientRect(g_hwnd, &rc)) return;
    const LONG mid = (rc.left + rc.right) / 2;
    RECT paneL = {rc.left, rc.top, mid, rc.bottom};
    RECT paneR = {mid, rc.top, rc.right, rc.bottom};

    HDC dc = GetDC(g_hwnd);
    SetStretchBltMode(dc, COLORONCOLOR);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        Latest& lt = g_latest[eye];
        if (!lt.fresh || lt.e == nullptr) continue;
        draw_pane_impl(dc, eye == 0 ? paneL : paneR, *lt.e);
        lt.fresh = false;
        g_drawn[eye]++;
    }
    ReleaseDC(g_hwnd, dc);

    // ~10s activity line (diagnostic; the log is the acceptance evidence).
    static ULONGLONG next_stats = 0;
    const ULONGLONG now = GetTickCount64();
    if (!g_stats_armed) {
        g_stats_armed = true;
        next_stats = now + 10000;
    } else if (now > next_stats) {
        next_stats = now + 10000;
        hostlog::write("seyes: window stats recvL=%llu recvR=%llu drawnL=%llu "
                       "drawnR=%llu opened=%zu openFails=%llu",
                       (unsigned long long)(g_received[0] - g_stat_eyes[0][0]),
                       (unsigned long long)(g_received[1] - g_stat_eyes[1][0]),
                       (unsigned long long)(g_drawn[0] - g_stat_eyes[0][1]),
                       (unsigned long long)(g_drawn[1] - g_stat_eyes[1][1]),
                       g_cache.size(), (unsigned long long)g_open_fails);
        g_stat_eyes[0][0] = (uint32_t)g_received[0];  // 32-bit is fine for a window delta
        g_stat_eyes[1][0] = (uint32_t)g_received[1];
        g_stat_eyes[0][1] = (uint32_t)g_drawn[0];
        g_stat_eyes[1][1] = (uint32_t)g_drawn[1];
    }
}

bool latest(uint32_t eye, LatestImage& out) {
    out = LatestImage();
    if (!g_inited || eye > 1) return false;
    Latest& lt = g_latest[eye];
    if (lt.e == nullptr || lt.e->srv == nullptr) return false;
    out.srv = lt.e->srv;
    out.frameId = lt.frameId;
    out.poseId = lt.poseId;
    out.w = lt.e->w;
    out.h = lt.e->h;
    return true;
}

}  // namespace seyes

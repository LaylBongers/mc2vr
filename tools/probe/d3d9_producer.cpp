// S4-2 probe producer (win32, runs in the game's Proton prefix => D3D9 = DXVK).
//
// Question this answers (stereo_design.md §S4 risk 1): does a D3D9 texture
// created with a legacy pSharedHandle open in DXVK's D3D11 via
// OpenSharedResource in ANOTHER process?
//
// Behavior: creates a WxH DEFAULT-pool texture in --fmt (21=A8R8G8B8,
// 22=X8R8G8B8) with pSharedHandle, fills it from a SYSTEMMEM staging copy
// (deterministic pattern + a rotating frame counter in blue), GPU-syncs with
// an event query (the same primitive the game's BeginSubmit spins on), then
// writes "fmt 0xhandle w h" to handle.txt and keeps redrawing every 500 ms
// until the consumer drops mc2vr_probe_done.txt in the CWD (or --seconds
// elapse). Keeping the texture live and redrawing also tests cross-process
// synchronization (risk 2): the consumer must observe consistent frames with
// an advancing counter using only the event-query ordering on this side.
//
// All evidence goes to mc2vr_probe_producer.log too ("proton run" eats
// stdout). "--ex 1" creates the device via Direct3DCreate9Ex (the interface
// the carrier would use); "--ex 0" uses plain Direct3DCreate9.

#include <windows.h>
#include <d3d9.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

static FILE* g_log;

static void logf_(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
    fflush(stdout);
}

static bool done_file_exists() {
    return GetFileAttributesA("mc2vr_probe_done.txt") != INVALID_FILE_ATTRIBUTES;
}

// Deterministic per-pixel pattern; k (the redraw counter) lands in blue so the
// consumer can both verify content and observe redraws. Alpha is masked by the
// consumer (X8R8G8B8 alpha is undefined).
static uint32_t pixel(uint32_t x, uint32_t y, uint32_t k) {
    const uint32_t r = (x * 3 + 17) & 0xff;
    const uint32_t g = (y * 7 + 29) & 0xff;
    const uint32_t b = k & 0xff;
    return 0xff000000u | (r << 16) | (g << 8) | b;
}

int main(int argc, char** argv) {
    uint32_t fmt = 21, w = 256, h = 128, seconds = 60, interval = 500;
    int wantEx = 1;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--fmt")) fmt = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--ex")) wantEx = atoi(argv[i + 1]);
        else if (!strcmp(argv[i], "--w")) w = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--h")) h = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--seconds")) seconds = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--interval")) interval = strtoul(argv[i + 1], 0, 0);
    }

    g_log = fopen("mc2vr_probe_producer.log", "w");
    logf_("producer: fmt=%u ex=%d %ux%u seconds=%u interval=%ums\n",
          fmt, wantEx, w, h, seconds, interval);

    HWND hwnd = CreateWindowExA(0, "STATIC", "mc2vr_probe", 0, 0, 0, 64, 64,
                                NULL, NULL, GetModuleHandleA(NULL), NULL);
    if (!hwnd) { logf_("producer: CreateWindow failed %lu\n", GetLastError()); return 2; }

    IDirect3D9* d3d = NULL;
    IDirect3D9Ex* d3dex = NULL;
    int usedEx = 0;
    if (wantEx) {
        if (SUCCEEDED(Direct3DCreate9Ex(D3D_SDK_VERSION, &d3dex))) {
            d3d = d3dex;
            usedEx = 1;
        } else {
            logf_("producer: Direct3DCreate9Ex failed, falling back to plain D3D9\n");
        }
    }
    if (!d3d) d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d) { logf_("producer: no Direct3D\n"); return 2; }
    logf_("producer: d3d9 created (ex=%d)\n", usedEx);

    D3DPRESENT_PARAMETERS pp;
    ZeroMemory(&pp, sizeof pp);
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    IDirect3DDevice9* dev = NULL;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd,
                                   D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
    if (FAILED(hr)) { logf_("producer: CreateDevice failed hr=0x%08lx\n", (unsigned long)hr); return 2; }
    logf_("producer: device ok\n");

    IDirect3DTexture9* tex = NULL;
    HANDLE shared = NULL;
    hr = dev->CreateTexture(w, h, 1, 0, (D3DFORMAT)fmt, D3DPOOL_DEFAULT, &tex, &shared);
    if (FAILED(hr) || !shared) {
        logf_("producer: CreateTexture(shared) FAILED hr=0x%08lx shared=%p\n",
              (unsigned long)hr, shared);
        FILE* f = fopen("handle.txt", "w");
        if (f) { fprintf(f, "ERR 0x%08lx\n", (unsigned long)hr); fclose(f); }
        return 2;
    }
    logf_("producer: shared texture ok, handle=0x%08x\n", (unsigned)(uintptr_t)shared);

    IDirect3DTexture9* sys = NULL;
    hr = dev->CreateTexture(w, h, 1, 0, (D3DFORMAT)fmt, D3DPOOL_SYSTEMMEM, &sys, NULL);
    if (FAILED(hr)) { logf_("producer: sysmem texture failed hr=0x%08lx\n", (unsigned long)hr); return 2; }

    IDirect3DQuery9* q = NULL;
    hr = dev->CreateQuery(D3DQUERYTYPE_EVENT, &q);
    if (FAILED(hr)) { logf_("producer: event query failed hr=0x%08lx (continuing without sync)\n", (unsigned long)hr); }

    uint32_t k = 0;
    const ULONGLONG tEnd = GetTickCount64() + (ULONGLONG)seconds * 1000;
    for (;; ++k) {
        D3DLOCKED_RECT lr;
        if (FAILED(sys->LockRect(0, &lr, NULL, 0))) { logf_("producer: LockRect failed\n"); return 2; }
        for (uint32_t y = 0; y < h; ++y) {
            uint32_t* row = (uint32_t*)((uint8_t*)lr.pBits + (size_t)y * lr.Pitch);
            for (uint32_t x = 0; x < w; ++x) row[x] = pixel(x, y, k);
        }
        sys->UnlockRect(0);

        hr = dev->UpdateTexture(sys, tex);
        if (FAILED(hr)) { logf_("producer: UpdateTexture failed hr=0x%08lx\n", (unsigned long)hr); return 2; }

        // GPU sync: same primitive the game's LtiRenderer_BeginSubmit spins on.
        // D3DGETDATA_FLUSH is required: DXVK only submits the pending command
        // buffer when polled with it (without the flag GetData stays S_FALSE
        // forever and the copies never land — caught live in this probe).
        if (q) {
            q->Issue(D3DISSUE_END);
            ULONGLONG tSyncEnd = GetTickCount64() + 10000;
            for (;;) {
                DWORD data = 0;
                hr = q->GetData(&data, sizeof data, D3DGETDATA_FLUSH);
                if (hr != S_FALSE) break;
                if (GetTickCount64() > tSyncEnd) { logf_("producer: sync spin TIMEOUT\n"); break; }
                Sleep(1);
            }
            if (k == 0) logf_("producer: event-query sync ok (hr=0x%08lx)\n", (unsigned long)hr);
        }

        if (k == 0) {
            FILE* f = fopen("handle.txt", "w");
            if (!f) { logf_("producer: cannot write handle.txt\n"); return 2; }
            fprintf(f, "%u 0x%08x %u %u\n", fmt, (unsigned)(uintptr_t)shared, w, h);
            fclose(f);
            logf_("producer: handle published, redrawing every 500 ms\n");
        }
        if (k == 8) logf_("producer: still alive (k=%u)\n", k);

        if (done_file_exists()) { logf_("producer: done file seen, exiting (k=%u)\n", k); break; }
        if (GetTickCount64() > tEnd) { logf_("producer: timeout, exiting (k=%u)\n", k); break; }
        Sleep(interval);
    }

    if (q) q->Release();
    sys->Release();
    tex->Release();
    dev->Release();
    if (d3dex) d3dex->Release(); else d3d->Release();
    logf_("producer: EXIT 0\n");
    return 0;
}

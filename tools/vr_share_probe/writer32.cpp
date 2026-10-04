// S4-2c probe A: DXVK shared-handle handoff, 32-bit writer.
//
// The relay design (docs/s4_handover.md §S4, option C with shared handles)
// needs: a 32-bit D3D9Ex process (the game's bitness) creating a shared
// RENDERTARGET texture, and a 64-bit wine process (the helper) opening it as
// D3D11 — cross-process AND cross-bitness, same wineserver/prefix. This
// writer creates the texture in the exact shape the carrier will use
// (RT usage, DEFAULT pool, pSharedHandle out), fills a verifiable pattern,
// publishes the handle, and stays alive until the reader reports.
//
// Build: i686-w64-mingw32-g++ -static -mconsole -o writer32.exe writer32.cpp -ld3d9
// Run:   <proton>/proton run writer32.exe   (see run.sh)
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>

// Wine console stdout is unreliable under `proton run` — log to a file next
// to the exe (cwd of the run).
static FILE *g_out;
#define printf(...) fprintf(g_out, __VA_ARGS__)

int main()
{
    g_out = fopen("writer32.log", "w");

    HMODULE d3d9 = LoadLibraryA("d3d9.dll");
    if (!d3d9) {
        printf("FATAL: d3d9.dll not found\n");
        return 1;
    }
    auto create9ex = (HRESULT(__stdcall *)(UINT, IDirect3D9Ex **))GetProcAddress(
        d3d9, "Direct3DCreate9Ex");
    if (!create9ex) {
        printf("FATAL: Direct3DCreate9Ex not exported\n");
        return 1;
    }

    IDirect3D9Ex *d3d = nullptr;
    HRESULT hr = create9ex(D3D_SDK_VERSION, &d3d);
    if (FAILED(hr) || !d3d) {
        printf("FATAL: Direct3DCreate9Ex hr=%08lx\n", (unsigned long)hr);
        return 1;
    }
    printf("IDirect3D9Ex created\n");

    D3DPRESENT_PARAMETERS pp = {};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 1;
    pp.BackBufferHeight = 1;
    pp.hDeviceWindow = GetDesktopWindow();

    IDirect3DDevice9 *dev = nullptr;
    hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                           GetDesktopWindow(),
                           D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                           &pp, &dev);
    if (FAILED(hr) || !dev) {
        printf("FATAL: CreateDevice hr=%08lx\n", (unsigned long)hr);
        return 1;
    }
    printf("device created\n");

    // The real capture shape: RT usage, DEFAULT pool, shared handle out.
    const UINT W = 1024, H = 1024;
    HANDLE shared = nullptr;
    IDirect3DTexture9 *tex = nullptr;
    hr = dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8,
                            D3DPOOL_DEFAULT, &tex, &shared);
    printf("CreateTexture(shared RT) hr=%08lx shared=%p\n", (unsigned long)hr,
           shared);
    if (FAILED(hr) || !tex || !shared) {
        printf("RESULT: SHARED-RT CREATE FAILED — DXVK d3d9 shared handles "
               "unusable in this shape; relay falls back to sysmem\n");
        fclose(g_out);
        return 2;
    }

    // Fill via the REAL capture mechanism: Clear on an intermediate RT, then
    // StretchRect into the shared RT surface — the exact shape the carrier
    // uses (device::blit_surfaces). The first attempt used UpdateTexture from
    // a sysmem texture and the reader saw all zeros, so the fill path was the
    // suspect, not the sharing.
    IDirect3DSurface9 *dst_surf = nullptr;
    hr = tex->GetSurfaceLevel(0, &dst_surf);
    if (FAILED(hr) || !dst_surf) {
        printf("FATAL: GetSurfaceLevel hr=%08lx\n", (unsigned long)hr);
        return 1;
    }
    IDirect3DSurface9 *src_rt = nullptr;
    hr = dev->CreateRenderTarget(W, H, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE,
                                 0, FALSE, &src_rt, nullptr);
    if (FAILED(hr) || !src_rt) {
        printf("FATAL: src CreateRenderTarget hr=%08lx\n", (unsigned long)hr);
        return 1;
    }
    hr = dev->SetRenderTarget(0, src_rt);
    hr = dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0xFFA5A5A5, 0.0f, 0);
    printf("Clear(src RT) hr=%08lx\n", (unsigned long)hr);
    hr = dev->StretchRect(src_rt, nullptr, dst_surf, nullptr, D3DTEXF_NONE);
    printf("StretchRect(src -> shared) hr=%08lx\n", (unsigned long)hr);
    if (FAILED(hr)) {
        printf("RESULT: STRETCH FAILED — cannot fill the shared RT\n");
        fclose(g_out);
        return 3;
    }

    // GPU sync: StretchRect is queued; the reader must not race it. An event
    // query flushed to completion is the classic D3D9 CPU-GPU barrier.
    {
        IDirect3DQuery9 *q = nullptr;
        if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) && q) {
            q->Issue(D3DISSUE_END);
            int spins = 0;
            while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE &&
                   ++spins < 1000000) {
                Sleep(0);
            }
            printf("GPU sync done (spins=%d)\n", spins);
            q->Release();
        } else {
            printf("WARNING: CreateQuery failed — no GPU sync\n");
        }
    }

    // Writer SELF-CHECK: read the shared texture back on the WRITER's own
    // device. If this shows the fill but reader64 sees zeros, the sharing is
    // broken; if this also shows zeros, the fill never landed and the
    // sharing is not yet proven either way.
    {
        IDirect3DSurface9 *sysmem = nullptr;
        hr = dev->CreateOffscreenPlainSurface(W, H, D3DFMT_A8R8G8B8,
                                              D3DPOOL_SYSTEMMEM, &sysmem,
                                              nullptr);
        if (SUCCEEDED(hr) && sysmem) {
            hr = dev->GetRenderTargetData(dst_surf, sysmem);
            D3DLOCKED_RECT self = {};
            if (SUCCEEDED(hr) &&
                SUCCEEDED(sysmem->LockRect(&self, nullptr, 0))) {
                uint32_t bad = 0;
                for (uint32_t s = 0; s < 256; s++) {
                    const uint32_t x = (s * 7) % W, y = (s * 13) % H;
                    const uint32_t px =
                        ((const uint32_t *)((const uint8_t *)self.pBits +
                                            y * self.Pitch))[x];
                    if (px != 0xFFA5A5A5u) {
                        bad++;
                    }
                }
                sysmem->UnlockRect();
                printf("writer self-check: %s (%u/256 mismatch)\n",
                       bad == 0 ? "FILL LANDED in the shared RT" : "FILL MISSING",
                       bad);
            } else {
                printf("writer self-check: readback failed hr=%08lx\n",
                       (unsigned long)hr);
            }
            sysmem->Release();
        }
    }

    // Publish the handle; keep the device alive (the shared memory lives
    // with it) while the reader verifies.
    FILE *hf = fopen("share_handle.txt", "w");
    fprintf(hf, "%llu\n", (unsigned long long)(uintptr_t)shared);
    fclose(hf);
    printf("handle published; waiting for reader64 ...\n");
    fflush(g_out);

    for (int i = 0; i < 600; i++) { // 120 s max
        Sleep(200);
        FILE *ok = fopen("share_ok.txt", "r");
        if (ok) {
            char verdict[256] = {};
            size_t n = fread(verdict, 1, sizeof(verdict) - 1, ok);
            (void)n;
            fclose(ok);
            printf("reader verdict: %s\n", verdict);
            printf("RESULT: DONE (see reader64.log for the handoff verdict)\n");
            fclose(g_out);
            return 0;
        }
    }
    printf("RESULT: TIMEOUT — reader never reported; see reader64.log\n");
    fclose(g_out);
    return 4;
}

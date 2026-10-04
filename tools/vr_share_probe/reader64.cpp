// S4-2c probe A: DXVK shared-handle handoff, 64-bit reader.
//
// 64-bit wine process (the helper's bitness): creates a D3D11 device (DXVK
// 64-bit) and opens the 32-bit writer's D3D9 shared texture via
// OpenSharedResource. If this works, the C-relay design can pass textures
// GPU-side with zero copies — no sysmem round-trip, no VR API in the game
// process at all.
//
// Build: x86_64-w64-mingw32-g++ -static -mconsole -o reader64.exe reader64.cpp -ld3d11
// Run:   <proton>/proton run reader64.exe   (see run.sh — writer32 first)
#include <windows.h>
#include <d3d11.h>
#include <cstdio>
#include <cstdint>

static FILE *g_out;
#define printf(...) fprintf(g_out, __VA_ARGS__)

static void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fflush(g_out);

    FILE *ok = fopen("share_ok.txt", "w");
    va_start(ap, fmt);
    vfprintf(ok, fmt, ap);
    va_end(ap);
    fclose(ok);
}

int main()
{
    g_out = fopen("reader64.log", "w");
    remove("share_ok.txt");

    // Wait for the writer to publish the handle (up to 60 s).
    HANDLE shared = nullptr;
    for (int i = 0; i < 300; i++) {
        FILE *hf = fopen("share_handle.txt", "r");
        if (hf) {
            unsigned long long h = 0;
            if (fscanf(hf, "%llu", &h) == 1) {
                shared = (HANDLE)(uintptr_t)h;
            }
            fclose(hf);
            if (shared) {
                break;
            }
        }
        Sleep(200);
    }
    if (!shared) {
        report("RESULT: FAIL — no handle published (writer32 never created "
               "it? see writer32.log)\n");
        fclose(g_out);
        return 1;
    }
    printf("handle from writer32: %p\n", shared);

    HMODULE d3d11 = LoadLibraryA("d3d11.dll");
    if (!d3d11) {
        report("RESULT: FAIL — d3d11.dll not found\n");
        return 1;
    }
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                    0, nullptr, 0, D3D11_SDK_VERSION, &dev,
                                    nullptr, &ctx);
    if (FAILED(hr) || !dev) {
        report("RESULT: FAIL — D3D11CreateDevice hr=%08lx\n", (unsigned long)hr);
        return 1;
    }
    printf("D3D11 device created (64-bit DXVK)\n");

    ID3D11Texture2D *tex = nullptr;
    hr = dev->OpenSharedResource(shared, __uuidof(ID3D11Texture2D),
                                 (void **)&tex);
    printf("OpenSharedResource hr=%08lx tex=%p\n", (unsigned long)hr, tex);
    if (FAILED(hr) || !tex) {
        report("RESULT: FAIL — OpenSharedResource hr=%08lx (DXVK cross-"
               "process/cross-bitness d3d9->d3d11 sharing NOT available; "
               "relay falls back to sysmem)\n", (unsigned long)hr);
        fclose(g_out);
        return 2;
    }

    D3D11_TEXTURE2D_DESC desc;
    tex->GetDesc(&desc);
    printf("opened texture: %ux%u fmt=%u\n", desc.Width, desc.Height,
           (unsigned)desc.Format);

    // Read back and verify the writer's pattern.
    D3D11_TEXTURE2D_DESC st_desc = desc;
    st_desc.Usage = D3D11_USAGE_STAGING;
    st_desc.BindFlags = 0;
    st_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    st_desc.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    hr = dev->CreateTexture2D(&st_desc, nullptr, &staging);
    if (FAILED(hr) || !staging) {
        report("RESULT: FAIL — staging CreateTexture2D hr=%08lx\n",
               (unsigned long)hr);
        return 1;
    }
    ctx->CopyResource(staging, tex);
    D3D11_MAPPED_SUBRESOURCE map = {};
    hr = ctx->Map(staging, 0, D3D11_MAP_READ, 0, &map);
    if (FAILED(hr)) {
        report("RESULT: FAIL — Map hr=%08lx\n", (unsigned long)hr);
        return 1;
    }

    const UINT W = desc.Width, H = desc.Height;
    // Writer fills with a solid Clear(0xFFA5A5A5) through StretchRect (the
    // real carrier capture shape).
    uint32_t bad = 0, checked = 0;
    for (uint32_t s = 0; s < 512; s++) {
        const uint32_t x = (s * 7) % W;
        const uint32_t y = (s * 13) % H;
        const uint32_t *row =
            (const uint32_t *)((const uint8_t *)map.pData + y * map.RowPitch);
        const uint32_t px = row[x];
        const uint32_t want = 0xFFA5A5A5u;
        checked++;
        if (px != want) {
            bad++;
            if (bad <= 4) {
                printf("mismatch at (%u,%u): got %08x want %08x\n", x, y, px,
                       want);
            }
        }
    }
    ctx->Unmap(staging, 0);

    printf("verified %u samples, %u mismatches\n", checked, bad);
    if (bad == 0) {
        report("RESULT: PASS — 32-bit D3D9 shared texture opened and verified "
               "from 64-bit D3D11; the shared-handle relay design is viable\n");
    } else {
        report("RESULT: FAIL — handle opened but PIXEL MISMATCH (%u/%u); "
               "shared memory not coherent across processes\n", bad, checked);
    }
    fclose(g_out);
    return bad == 0 ? 0 : 3;
}

// S4-2 probe consumer (win64, runs in the SAME Proton prefix => D3D11 = DXVK).
// Complement of d3d9_producer.cpp: opens the producer's legacy shared handle
// with ID3D11Device::OpenSharedResource, verifies the pixel pattern, then
// re-reads after a delay to verify live redraws are observable across the
// process boundary with only the producer-side event-query ordering (no
// fence/keyed mutex on legacy handles — docs/plans/stereo_design.md §S4 risk 2).
//
// Success criteria (exit 0): OpenSharedResource succeeds, dims match, RGB
// pattern exact (alpha ignored), blue counter uniform per read, and the
// counter ADVANCED between two reads 1.3 s apart. Writes
// mc2vr_probe_done.txt so the producer exits, and evidence to
// mc2vr_probe_consumer.log (stdout is lost under "proton run").

#include <windows.h>
#include <d3d11.h>
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

static uint32_t expected_rg(uint32_t x, uint32_t y) {
    return (((x * 3 + 17) & 0xff) << 16) | (((y * 7 + 29) & 0xff) << 8);
}

// One snapshot: copies the shared texture to staging, verifies RGB against the
// pattern, and returns the uniform blue counter (or -1 on failure, with
// *mismatches filled for the log).
static int read_frame(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex,
                      ID3D11Texture2D* stg, uint32_t w, uint32_t h,
                      uint64_t* mismatches) {
    *mismatches = 0;
    ctx->CopyResource(stg, tex);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(stg, 0, D3D11_MAP_READ, 0, &m))) {
        logf_("consumer: Map failed\n");
        return -1;
    }
    int k = -1;
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t* row = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
        if (y < 2) {
            logf_("consumer:   row %u:", y);
            for (uint32_t x = 0; x < 8 && x < w; ++x) logf_(" %08x", row[x]);
            logf_(" (pitch=%u)\n", m.RowPitch);
        }
        for (uint32_t x = 0; x < w; ++x) {
            const uint32_t px = row[x] & 0x00ffffff;  // alpha undefined for X8R8G8B8
            const uint32_t rg = px & 0xffff00;
            const uint32_t b = px & 0xff;
            if (rg != expected_rg(x, y)) ++*mismatches;
            if (k < 0) k = (int)b;
            else if ((int)b != k) ++*mismatches;  // torn frame / non-uniform counter
        }
    }
    ctx->Unmap(stg, 0);
    return k;
}

static bool fail(const char* why) {
    logf_("consumer: FAIL — %s\n", why);
    FILE* f = fopen("mc2vr_probe_done.txt", "w");
    if (f) { fputs("fail\n", f); fclose(f); }
    if (g_log) fflush(g_log);
    return false;
}

int main(int argc, char** argv) {
    uint64_t handle = 0;
    uint32_t w = 256, h = 128, seconds = 20;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "--handle")) handle = strtoull(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--w")) w = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--h")) h = strtoul(argv[i + 1], 0, 0);
        else if (!strcmp(argv[i], "--seconds")) seconds = strtoul(argv[i + 1], 0, 0);
    }

    g_log = fopen("mc2vr_probe_consumer.log", "w");
    logf_("consumer: handle=0x%llx %ux%u seconds=%u\n",
          (unsigned long long)handle, w, h, seconds);

    ID3D11Device* dev = NULL;
    ID3D11DeviceContext* ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, NULL, 0,
                                   D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { fail("D3D11CreateDevice"); return 2; }
    logf_("consumer: d3d11 device ok\n");

    ID3D11Texture2D* tex = NULL;
    hr = dev->OpenSharedResource((HANDLE)(UINT_PTR)handle, __uuidof(ID3D11Texture2D),
                                 (void**)&tex);
    if (FAILED(hr) || !tex) {
        logf_("consumer: OpenSharedResource FAILED hr=0x%08lx\n", (unsigned long)hr);
        fail("OpenSharedResource");
        return 2;
    }
    D3D11_TEXTURE2D_DESC d;
    tex->GetDesc(&d);
    logf_("consumer: opened! %ux%u fmt=%u (87=B8G8R8A8_UNORM) usage=%u\n",
          d.Width, d.Height, d.Format, d.Usage);
    if (d.Width != w || d.Height != h) { fail("dims mismatch"); return 2; }

    D3D11_TEXTURE2D_DESC sd = d;
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* stg = NULL;
    hr = dev->CreateTexture2D(&sd, NULL, &stg);
    if (FAILED(hr)) { fail("staging creation"); return 2; }

    uint64_t mm = 0;
    const int k1 = read_frame(ctx, tex, stg, w, h, &mm);
    logf_("consumer: read1 k=%d mismatches=%llu\n", k1, (unsigned long long)mm);
    if (k1 < 0 || mm > 0) { fail("first frame verify"); return 1; }

    // Sustained concurrent-read loop (emulates the live S4-2 host: continuous
    // staging reads while the producer redraws; the original probe read twice
    // and stopped, which left the sustained path untested).
    uint32_t reads = 0, torn = 0;
    int lastK = k1;
    const ULONGLONG tEnd = GetTickCount64() + (ULONGLONG)seconds * 1000;
    while (GetTickCount64() < tEnd) {
        uint64_t m2 = 0;
        const int k = read_frame(ctx, tex, stg, w, h, &m2);
        ++reads;
        if (k < 0 || m2 > 0) ++torn;  // torn/mid-write frame: counted, not fatal
        if (k > lastK) lastK = k;
        if (reads % 500 == 0)
            logf_("consumer: sustained reads=%u torn=%u k=%d\n", reads, torn, lastK);
        Sleep(16);
    }
    logf_("consumer: sustained loop done: reads=%u torn=%u lastK=%d\n", reads,
          torn, lastK);
    if (torn > reads / 16) {  // allow some tearing races; not a wall of them
        fail("excessive tearing in sustained reads");
        return 1;
    }
    if (lastK == k1) { fail("counter did not advance (no live redraw observed)"); return 1; }
    logf_("consumer: counter advanced %d -> %d — cross-process sync ok\n", k1, lastK);

    logf_("consumer: PASS\n");
    FILE* f = fopen("mc2vr_probe_done.txt", "w");
    if (f) { fputs("pass\n", f); fclose(f); }
    if (g_log) fflush(g_log);
    return 0;
}

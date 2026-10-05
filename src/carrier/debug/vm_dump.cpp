// Live VM-chain reader — see vm_dump.hpp.

#include "vm_dump.hpp"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "log.hpp"

namespace mc2vr::vmdump {

namespace {

bool g_enabled = false;

constexpr uint32_t SLOT_THUNK = 0x024dc4ecu;  // [0x0046ab80] jmp target slot
constexpr uint32_t SLOT_HANDLER = 0x02299230u; // [0x01b950a0] jmp target slot
constexpr DWORD SECOND_READ_DELAY_MS = 15000;

// ---- on-disk image (VA -> file bytes) -----------------------------------------
struct Section {
    uint32_t va, vsize, raw_off, raw_size;
};
std::vector<Section> g_sections;
FILE *g_exe = nullptr;

bool open_image()
{
    wchar_t path[MAX_PATH];
    if (GetModuleFileNameW(nullptr, path, MAX_PATH) == 0) return false;
    g_exe = _wfopen(path, L"rb");
    if (!g_exe) return false;

    uint8_t hdr[4096];
    if (fread(hdr, 1, sizeof(hdr), g_exe) != sizeof(hdr)) return false;
    auto *dos = reinterpret_cast<IMAGE_DOS_HEADER *>(hdr);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    auto *nt = reinterpret_cast<IMAGE_NT_HEADERS32 *>(hdr + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    const uint32_t base = nt->OptionalHeader.ImageBase;
    auto *sec = IMAGE_FIRST_SECTION(nt);
    for (unsigned i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        g_sections.push_back({base + sec[i].VirtualAddress, sec[i].Misc.VirtualSize,
                              sec[i].PointerToRawData, sec[i].SizeOfRawData});
    }
    return true;
}

// Fill out[0..len) with the file image of [va, va+len). Bytes past a
// section's raw data read as zero (as the loader maps them). Returns false if
// va is outside every section.
bool read_file_image(uint32_t va, uint8_t *out, uint32_t len)
{
    for (const auto &s : g_sections) {
        if (va < s.va || va + len > s.va + (s.vsize > s.raw_size ? s.vsize : s.raw_size)) {
            continue;
        }
        memset(out, 0, len);
        const uint32_t rel = va - s.va;
        if (rel < s.raw_size) {
            const uint32_t n = (rel + len <= s.raw_size) ? len : s.raw_size - rel;
            if (fseek(g_exe, (long)(s.raw_off + rel), SEEK_SET) != 0) return false;
            if (fread(out, 1, n, g_exe) != n) return false;
        }
        return true;
    }
    return false;
}

// ---- runtime memory -------------------------------------------------------------
bool read_runtime(uint32_t va, uint8_t *out, uint32_t len)
{
    uint32_t done = 0;
    while (done < len) {
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery((void *)(uintptr_t)(va + done), &mbi, sizeof(mbi)) == 0) return false;
        if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD))) return false;
        const uintptr_t end = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
        uint32_t n = (uint32_t)(end - (va + done));
        if (n > len - done) n = len - done;
        memcpy(out + done, (void *)(uintptr_t)(va + done), n);
        done += n;
    }
    return true;
}

void log_hex(uint32_t va, const uint8_t *b, uint32_t len)
{
    for (uint32_t i = 0; i < len; i += 32) {
        char line[32 * 3 + 1];
        char *p = line;
        for (uint32_t j = i; j < i + 32 && j < len; j++) p += sprintf(p, "%02X ", b[j]);
        MC2VR_LOG("vmdump:   %08X: %s", va + i, line);
    }
}

// pass 0/1 select the snapshot slot used for the pass-1 vs pass-0 comparison.
struct Snap {
    uint32_t va;
    std::vector<uint8_t> bytes;
};
std::vector<Snap> g_prev;

void dump_region(int pass, const char *label, uint32_t va, uint32_t len, std::vector<Snap> &cur)
{
    std::vector<uint8_t> rt(len), fi(len);
    if (!read_runtime(va, rt.data(), len)) {
        MC2VR_LOG("vmdump: [%d] %s @%08X +%u: NOT READABLE at runtime", pass, label, va, len);
        return;
    }
    cur.push_back({va, rt});

    const bool have_file = read_file_image(va, fi.data(), len);
    uint32_t diffs = 0;
    if (have_file) {
        for (uint32_t i = 0; i < len; i++) diffs += (rt[i] != fi[i]);
    }
    if (!have_file) {
        MC2VR_LOG("vmdump: [%d] %s @%08X +%u: no file image (outside PE sections)", pass, label, va, len);
    } else if (diffs == 0) {
        MC2VR_LOG("vmdump: [%d] %s @%08X +%u: runtime == file", pass, label, va, len);
    } else {
        MC2VR_LOG("vmdump: [%d] %s @%08X +%u: runtime DIFFERS from file in %u bytes", pass, label, va, len, diffs);
    }
    log_hex(va, rt.data(), len);
    if (have_file && diffs) {
        MC2VR_LOG("vmdump:   file image:");
        log_hex(va, fi.data(), len);
    }
    if (pass == 1) {
        for (const auto &p : g_prev) {
            if (p.va == va && p.bytes.size() == len) {
                uint32_t d = 0;
                for (uint32_t i = 0; i < len; i++) d += (rt[i] != p.bytes[i]);
                MC2VR_LOG("vmdump: [1] %s @%08X: %u bytes changed since first read", label, va, d);
            }
        }
    }
}

uint32_t read_u32(uint32_t va)
{
    uint32_t v = 0;
    read_runtime(va, reinterpret_cast<uint8_t *>(&v), 4);
    return v;
}

void dump_chain(int pass)
{
    std::vector<Snap> cur;
    dump_region(pass, "thunk .text (jmp [slot])", 0x0046ab80u, 8, cur);
    dump_region(pass, "thunk slot", SLOT_THUNK, 4, cur);

    const uint32_t stub = read_u32(SLOT_THUNK);
    MC2VR_LOG("vmdump: [%d] thunk slot -> %08X (file image said 025628E0)", pass, stub);
    dump_region(pass, "stub (slot target)", stub, 64, cur);

    dump_region(pass, "Stext handler 01b950a0", 0x01b950a0u, 64, cur);
    dump_region(pass, "handler slot", SLOT_HANDLER, 4, cur);

    const uint32_t entry = read_u32(SLOT_HANDLER);
    MC2VR_LOG("vmdump: [%d] handler slot -> %08X (file image said 02AB0000)", pass, entry);
    dump_region(pass, "VM entry (handler slot target)", entry, 256, cur);

    if (pass == 0) g_prev = cur;
}


// ---- thunk-slot census ------------------------------------------------------------
// Every `jmp [slot]` (FF 25 <slot32>) in .text whose slot lies in the SecuROM
// range, with the target the FILE image names vs the target the slot holds NOW.
// A patched slot pointing back into .text means the "VM thunk" is native at
// runtime (found for 0x0046ab80, see docs/reverse_engineering/securom_vm.md).
constexpr uint32_t TEXT_LO = 0x00401000u, TEXT_HI = 0x00b04fffu;
constexpr uint32_t SROM_LO = 0x01a48000u, SROM_HI = 0x03771f0fu;

const char *region_of(uint32_t a)
{
    if (a >= TEXT_LO && a <= TEXT_HI) return "text";
    if (a >= SROM_LO && a <= SROM_HI) return "securom";
    return "other";
}

void scan_thunks()
{
    const uint32_t len = TEXT_HI - TEXT_LO + 1;
    std::vector<uint8_t> text(len);
    if (!read_runtime(TEXT_LO, text.data(), len)) {
        MC2VR_LOG("vmdump: thunk scan: .text not readable");
        return;
    }

    wchar_t path[MAX_PATH];
    HMODULE self = nullptr;
    FILE *csv = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                               GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&scan_thunks, &self) &&
        GetModuleFileNameW(self, path, MAX_PATH)) {
        wchar_t *slash = wcsrchr(path, L'\\');
        if (slash) {
            wcscpy(slash + 1, L"vm_thunks.csv");
            csv = _wfopen(path, L"w");
        }
    }
    if (csv) fprintf(csv, "thunk,slot,file_target,runtime_target,file_region,runtime_region,patched,aligned16\n");

    uint32_t total = 0, patched = 0, to_text = 0, unreadable = 0;
    uint32_t matrix[3][3] = {}; // [file region][runtime region]
    const char *const names[3] = {"text", "securom", "other"};
    auto idx = [](const char *r) { return r[0] == 't' ? 0 : r[0] == 's' ? 1 : 2; };

    for (uint32_t i = 0; i + 6 <= len; i++) {
        if (text[i] != 0xFF || text[i + 1] != 0x25) continue;
        uint32_t slot;
        memcpy(&slot, &text[i + 2], 4);
        if (slot < SROM_LO || slot > SROM_HI) continue;

        const uint32_t thunk = TEXT_LO + i;
        uint32_t file_t = 0, run_t = 0;
        uint8_t b[4];
        const bool have_file = read_file_image(slot, b, 4);
        if (have_file) memcpy(&file_t, b, 4);
        const bool have_run = read_runtime(slot, reinterpret_cast<uint8_t *>(&run_t), 4);
        total++;
        if (!have_run) { unreadable++; continue; }
        const bool pat = have_file && file_t != run_t;
        const char *fr = have_file ? region_of(file_t) : "?";
        const char *rr = region_of(run_t);
        if (pat) patched++;
        if (rr[0] == 't') to_text++;
        if (have_file) matrix[idx(fr)][idx(rr)]++;
        if (csv) {
            fprintf(csv, "%08X,%08X,%08X,%08X,%s,%s,%d,%d\n", thunk, slot, file_t, run_t,
                    fr, rr, pat ? 1 : 0, (thunk & 0xF) == 0 ? 1 : 0);
        }
    }
    if (csv) fclose(csv);

    MC2VR_LOG("vmdump: thunk scan: %u thunks (jmp [slot in SecuROM range]); %u slots patched vs file; "
              "%u resolve into .text now; %u slots unreadable; csv=%s",
              total, patched, to_text, unreadable, csv ? "vm_thunks.csv" : "(not written)");
    for (int f = 0; f < 3; f++) {
        for (int r = 0; r < 3; r++) {
            if (matrix[f][r]) {
                MC2VR_LOG("vmdump:   file target %-7s -> runtime target %-7s : %u",
                          names[f], names[r], matrix[f][r]);
            }
        }
    }
}

DWORD WINAPI thread_main(void *)
{
    if (!open_image()) {
        MC2VR_LOG("vmdump: cannot open/parse the exe image — runtime-vs-file diff unavailable");
    }
    dump_chain(0);
    Sleep(SECOND_READ_DELAY_MS);
    dump_chain(1);
    scan_thunks();
    if (g_exe) fclose(g_exe);
    MC2VR_LOG("vmdump: done");
    return 0;
}

} // namespace

bool set_enabled(const char *value)
{
    if (strcmp(value, "on") == 0) { g_enabled = true; return true; }
    if (strcmp(value, "off") == 0) { g_enabled = false; return true; }
    return false;
}

void install()
{
    if (!g_enabled) return;
    HANDLE h = CreateThread(nullptr, 0, thread_main, nullptr, 0, nullptr);
    if (h) CloseHandle(h);
    else MC2VR_LOG("vmdump: CreateThread failed (error %lu)", GetLastError());
}

} // namespace mc2vr::vmdump

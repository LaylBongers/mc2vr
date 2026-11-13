// hlslc: compiles one HLSL shader with Microsoft's d3dcompiler_43 into a C++
// header holding its bytecode (docs/build.md "HLSL shader compiler").
// Win32 (i686 mingw), run under Wine at build time by cmake/hlsl.cmake.
//
//   hlslc --dll <d3dcompiler_43.dll> --entry <fn> --profile <ps_3_0|vs_3_0>
//         --symbol <name> --out <header> [--depfile <file>] [-I <dir>]...
//         [-D <name>[=<value>]]... <source.hlsl>
//
// Paths may be Wine paths (Z:/...); the depfile gets them back as host paths
// (the Z: prefix dropped). #include "x" resolves against the including file's
// directory, then each -I directory. Refuses Wine's built-in d3dcompiler:
// that is vkd3d-shader, which maps bool uniforms to float registers.

#include <windows.h>
#include <d3dcompiler.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

typedef HRESULT(WINAPI *D3DCompile_t)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO *, ID3DInclude *, LPCSTR,
                                      LPCSTR, UINT, UINT, ID3DBlob **, ID3DBlob **);

bool read_file(const std::string &path, std::vector<char> &out)
{
    FILE *f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return false;
    char buf[8192];
    size_t n;
    out.clear();
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) out.insert(out.end(), buf, buf + n);
    std::fclose(f);
    return true;
}

std::string dir_of(const std::string &path)
{
    const size_t s = path.find_last_of("/\\");
    return s == std::string::npos ? std::string(".") : path.substr(0, s);
}

std::string host_path(std::string p)
{
    if (p.size() >= 2 && (p[0] == 'Z' || p[0] == 'z') && p[1] == ':') p = p.substr(2);
    for (char &c : p) {
        if (c == '\\') c = '/';
    }
    return p;
}

// #include handler: remembers every file it opened (for the depfile).
class Includes : public ID3DInclude {
public:
    std::vector<std::string> dirs;   // -I
    std::vector<std::string> files;  // opened, in order
    std::string root_dir;            // the source's directory

    HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE, LPCSTR name, LPCVOID parent, LPCVOID *data, UINT *bytes) override
    {
        std::vector<std::string> candidates;
        candidates.push_back(dir_of_data(parent) + "/" + name);
        for (const std::string &d : dirs) candidates.push_back(d + "/" + name);
        for (const std::string &c : candidates) {
            auto *buf = new std::vector<char>;
            if (read_file(c, *buf)) {
                files.push_back(c);
                opened_.push_back(Opened{buf, dir_of(c)});
                *data = buf->data();
                *bytes = (UINT)buf->size();
                return S_OK;
            }
            delete buf;
        }
        std::fprintf(stderr, "hlslc: #include \"%s\" not found\n", name);
        return E_FAIL;
    }

    HRESULT STDMETHODCALLTYPE Close(LPCVOID data) override
    {
        for (Opened &o : opened_) {
            if (o.buf != nullptr && o.buf->data() == data) {
                delete o.buf;
                o.buf = nullptr;
            }
        }
        return S_OK;
    }

private:
    struct Opened {
        std::vector<char> *buf;
        std::string dir;
    };
    std::vector<Opened> opened_;

    std::string dir_of_data(LPCVOID parent) const
    {
        for (const Opened &o : opened_) {
            if (o.buf != nullptr && o.buf->data() == parent) return o.dir;
        }
        return root_dir;  // the top-level source
    }
};

int usage()
{
    std::fprintf(stderr,
                 "usage: hlslc --dll <d3dcompiler_43.dll> --entry <fn> --profile <p> --symbol <name> "
                 "--out <header> [--depfile <file>] [-I <dir>]... [-D <name>[=<value>]]... <source.hlsl>\n");
    return 2;
}

} // namespace

int main(int argc, char **argv)
{
    std::string dll, entry, profile, symbol, out, depfile, source;
    Includes inc;
    std::vector<std::string> defines;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        auto next = [&](std::string &v) {
            if (i + 1 >= argc) return false;
            v = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--dll" && next(dll)) continue;
        if (a == "--entry" && next(entry)) continue;
        if (a == "--profile" && next(profile)) continue;
        if (a == "--symbol" && next(symbol)) continue;
        if (a == "--out" && next(out)) continue;
        if (a == "--depfile" && next(depfile)) continue;
        if (a == "-I" && next(v)) {
            inc.dirs.push_back(v);
            continue;
        }
        if (a == "-D" && next(v)) {
            defines.push_back(v);
            continue;
        }
        if (!a.empty() && a[0] != '-' && source.empty()) {
            source = a;
            continue;
        }
        return usage();
    }
    if (dll.empty() || entry.empty() || profile.empty() || symbol.empty() || out.empty() || source.empty()) {
        return usage();
    }

    // Wine's built-in d3dcompiler (vkd3d-shader) would compile, but wrongly
    // for the game's interface: refuse it outright.
    std::vector<char> dll_bytes;
    if (!read_file(dll, dll_bytes)) {
        std::fprintf(stderr, "hlslc: %s not found — set up the compiler prefix (README, setup step 3)\n", dll.c_str());
        return 1;
    }
    static const char kBuiltin[] = "Wine builtin DLL";
    if (std::search(dll_bytes.begin(), dll_bytes.end(), kBuiltin, kBuiltin + sizeof kBuiltin - 1) != dll_bytes.end()) {
        std::fprintf(stderr, "hlslc: %s is Wine's built-in d3dcompiler, not Microsoft's (README, setup step 3)\n",
                     dll.c_str());
        return 1;
    }
    HMODULE m = LoadLibraryA(dll.c_str());
    if (m == nullptr) {
        std::fprintf(stderr, "hlslc: LoadLibrary(%s) failed (%lu)\n", dll.c_str(), GetLastError());
        return 1;
    }
    auto compile = (D3DCompile_t)GetProcAddress(m, "D3DCompile");
    if (compile == nullptr) {
        std::fprintf(stderr, "hlslc: no D3DCompile in %s\n", dll.c_str());
        return 1;
    }

    std::vector<char> src;
    if (!read_file(source, src)) {
        std::fprintf(stderr, "hlslc: cannot read %s\n", source.c_str());
        return 1;
    }
    inc.root_dir = dir_of(source);
    std::vector<std::string> names, values;
    for (const std::string &d : defines) {
        const size_t eq = d.find('=');
        names.push_back(d.substr(0, eq));
        values.push_back(eq == std::string::npos ? std::string("1") : d.substr(eq + 1));
    }
    std::vector<D3D_SHADER_MACRO> macros;
    for (size_t i = 0; i < names.size(); i++) macros.push_back({names[i].c_str(), values[i].c_str()});
    macros.push_back({nullptr, nullptr});

    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = compile(src.data(), src.size(), host_path(source).c_str(), macros.data(), &inc, entry.c_str(),
                               profile.c_str(), D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (errors != nullptr) {
        std::fprintf(stderr, "%.*s", (int)errors->GetBufferSize(), (const char *)errors->GetBufferPointer());
    }
    if (FAILED(hr) || code == nullptr) {
        std::fprintf(stderr, "hlslc: %s: compile failed (0x%08lx)\n", source.c_str(), (unsigned long)hr);
        return 1;
    }

    const auto *words = (const uint32_t *)code->GetBufferPointer();
    const size_t n = code->GetBufferSize() / 4;
    FILE *f = std::fopen(out.c_str(), "wb");
    if (f == nullptr) {
        std::fprintf(stderr, "hlslc: cannot write %s\n", out.c_str());
        return 1;
    }
    std::fprintf(f,
                 "// Generated by tools/hlslc from %s (%s, %s, d3dcompiler_43). Do not edit.\n"
                 "#pragma once\n\n#include <cstdint>\n\nnamespace mc2vr::shaders {\n\n"
                 "inline constexpr uint32_t %s[%u] = {",
                 host_path(source).c_str(), entry.c_str(), profile.c_str(), symbol.c_str(), (unsigned)n);
    for (size_t i = 0; i < n; i++) std::fprintf(f, "%s0x%08x,", (i % 8 == 0) ? "\n    " : " ", words[i]);
    std::fprintf(f, "\n};\n\n} // namespace mc2vr::shaders\n");
    std::fclose(f);

    if (!depfile.empty()) {
        FILE *d = std::fopen(depfile.c_str(), "wb");
        if (d == nullptr) {
            std::fprintf(stderr, "hlslc: cannot write %s\n", depfile.c_str());
            return 1;
        }
        std::fprintf(d, "%s: %s", host_path(out).c_str(), host_path(source).c_str());
        for (const std::string &file : inc.files) std::fprintf(d, " \\\n  %s", host_path(file).c_str());
        std::fprintf(d, "\n");
        std::fclose(d);
    }
    return 0;
}

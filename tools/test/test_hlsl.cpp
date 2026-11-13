// The build-time HLSL pipeline (cmake/hlsl.cmake, tools/hlslc): a shader
// compiled at build time must be SM3 bytecode whose bool constants are real
// bool registers with static `if bN` branches, as the game's shaders use (the
// game uploads them with SetPixelShaderConstantB). Wine's built-in compiler
// (vkd3d-shader) emits float registers instead (docs/build.md).
//   cmake --build build/win32 --target mc2vr_hlsl_test && wine build/win32/bin/mc2vr_hlsl_test.exe

#include <cstdint>
#include <cstdio>

#include "shaders/test_static_bool_ps.h"

namespace {

constexpr uint32_t kRegConstBool = 14;  // D3DSPR_CONSTBOOL
constexpr uint32_t kOpIf = 40;          // D3DSIO_IF
constexpr uint32_t kOpComment = 0xfffe;
constexpr uint32_t kOpEnd = 0xffff;

uint32_t reg_type(uint32_t token)
{
    return ((token >> 28) & 7) | (((token >> 11) & 3) << 3);
}

} // namespace

int main()
{
    const uint32_t *t = mc2vr::shaders::test_static_bool_ps;
    const size_t n = sizeof mc2vr::shaders::test_static_bool_ps / 4;
    int failed = 0;
    if (t[0] != 0xffff0300u) {
        std::printf("FAIL version token %08x, want ps_3_0 (ffff0300)\n", t[0]);
        return 1;
    }
    int bool_ifs = 0, other_ifs = 0;
    bool ended = false;
    for (size_t i = 1; i < n;) {
        const uint32_t op = t[i] & 0xffff;
        if (op == kOpEnd) {
            ended = true;
            break;
        }
        if (op == kOpComment) {
            i += 1 + ((t[i] >> 16) & 0x7fff);
            continue;
        }
        const uint32_t len = (t[i] >> 24) & 0xf;
        if (op == kOpIf && len >= 1) {
            (reg_type(t[i + 1]) == kRegConstBool ? bool_ifs : other_ifs)++;
        }
        i += 1 + len;
    }
    if (!ended) {
        std::printf("FAIL no end token\n");
        failed++;
    }
    if (bool_ifs != 2 || other_ifs != 0) {
        std::printf("FAIL `if` on bool registers: %d (want 2), on others: %d (want 0)\n", bool_ifs, other_ifs);
        failed++;
    }
    std::printf("%s: %zu words, %d bool static branches\n", failed ? "FAILED" : "ok", n, bool_ifs);
    return failed ? 1 : 0;
}

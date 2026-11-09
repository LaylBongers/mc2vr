// PgPrimitive_SubmitToGPU — see submit_to_gpu.hpp.
//
// Follows the original block by block (VA ranges in the comments). Floats are
// scalar SSE in the original (mulss/addss, no x87); the matrix keeps its
// operation order (one multiply, plus one add for row 3) so results are
// bit-identical. Deviations: none intended.

#include "reimpl/render/submit_to_gpu.hpp"

#include "reimpl/render/begin_submit.hpp"
#include "reimpl/render/render_cmd.hpp"

#include <windows.h>
#include <d3d9.h>

#include <cstdint>
#include <cstring>

#include "debug/camera_census.hpp"
#include "debug/gui_census.hpp"
#include "game/dx9.hpp"
#include "game/lti.hpp"
#include "game/pg_primitive.hpp"
#include "game/render_shell.hpp"
#include "vr/adaptation.hpp"
#include "vr/eye_replay.hpp"
#include "vr/hud.hpp"
#include "vr/world_markers.hpp"
#include "vr/view_rewrite.hpp"

namespace mc2vr::reimpl {

using namespace game;

namespace {

// Named textures bound to RenderShell view-texture slots each frame
// (64-bit name hashes; which textures they are is unknown).
struct NamedTextureBind {
    uint64_t key;
    uint32_t slot;
};
constexpr NamedTextureBind kNamedTextures[2] = {
    {0xf011157a5e84ea6dull, 0xd},
    {0xf011157a16085a8dull, 0xe},
};

void bind_named_textures()  // 0x855771-0x85586f
{
    for (const NamedTextureBind &b : kNamedTextures) {
        void *const table = g_TextureRegistry::get();
        const uint64_t key = b.key;
        EnterCriticalSection(texture_registry_lock());
        uint8_t *const entry = HashTable_FindByHash64(table, &key);
        LeaveCriticalSection(texture_registry_lock());
        if (entry != nullptr) {
            const uint16_t kind = *reinterpret_cast<const uint16_t *>(entry + 0x14);
            if (kind == 2 || kind == 4) {
                RenderShell_BindViewTexture(render_shell(), b.slot, (uint32_t)(uintptr_t)(entry + 0x1c));
            }
        }
    }
}

bool is_view_type_1_2_3_7(uint8_t vt) { return vt == 1 || vt == 2 || vt == 3 || vt == 7; }

// EffectPass.materialMatrix: 6 vec4 from the material rows and the view
// entry's scale/offset (0x855e40-0x8561b5).
void build_material_matrix(float m[6][4], const PgMaterial *mat, const ViewEntry *view)
{
    const float *s = view->scale;
    const float *o = view->offset;
    const float(*r)[4] = mat->rows;
    for (int i = 0; i < 4; i++) {
        m[0][i] = r[0][i] * s[i];
        m[1][i] = r[1][i] * s[i];
        m[4][i] = r[4][i] * s[i];
        m[2][i] = r[2][i] * s[i];
        m[3][i] = r[3][i] * s[i] + o[i];
    }
    m[2][3] = r[2][3];  // .w unscaled (0x855fb2 overwrites it)
    m[5][0] = r[5][0];
    m[5][1] = r[5][1];
    m[5][2] = r[5][2];
    m[5][3] = r[5][3] * s[3];
    if ((mat->flagsA0 >> 1) & 1) {
        const float f = m[5][3];
        for (int row = 0; row < 5; row++) {
            for (int i = 0; i < 4; i++) {
                m[row][i] = f * m[row][i];
            }
        }
    }
}

// Frame end for the last view type (0x8563c5-0x85646b; jump table 0x856480).
void end_view_type()
{
    const uint8_t vt = g_CurViewType::get();
    if (vt == 0x0c) {
        return;  // no view type active
    }
    switch (vt) {
    case 7:
        Dx9_SetRenderTarget(0, *reinterpret_cast<IDirect3DSurface9 **>(
                                   reinterpret_cast<uint8_t *>(g_RenderShellPtr::get()) + kRenderShellRT0Surface));
        Dx9_SetRenderTarget(1, nullptr);
        Dx9_SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
        [[fallthrough]];
    case 1:
    case 2:
    case 3:
        Dx9_SetRenderState(D3DRS_DEPTHBIAS, 0);
        break;
    case 8:
        if (g_HalfResPassActive::get() != 0) {
            RenderShell_EndHalfResPass();
            g_HalfResPassActive::set(0);
        }
        break;
    default:
        break;
    }
    g_CurRTWriteMask::set(4);
    g_CurViewType::set(0x0c);
    g_LastMaterialIdx::set(0xffff);
    g_LastPass::set(nullptr);
    g_LastRecViewFlags::set(0xff);
    DAT_00ce7d24::set(0xff);
    g_CurPassObject::set(0);
}

// Returns false when the frame is skipped (the original's early exits).
bool begin_pass()  // 0x8556a7-0x855754
{
    if (g_NameHash_PgPrimitiveSubmitToGPU::get() == 0) {
        g_NameHash_PgPrimitiveSubmitToGPU::set(Lti_LazyNameHash(s_PgPrimitive_SubmitToGPU::ptr()));
    }
    if (g_PgPrimitiveScratchRecord::get() == 0) {
        return false;
    }
    if (g_PrecacheRequested::get() != 0) {
        if (DAT_0117560c::get() == 0) {
            return false;
        }
        g_PrecacheStarted::set(1);
        g_PrecacheRequested::set(0);
        if (RenderShell_PrecacheLoadStep()) {
            Lti_DebugPrintf_1(s_FinishedPrecacheRender::ptr(), g_PrecacheDisplayIndex::get());
            g_PrecacheDisplayIndex::set(g_PrecacheDisplayIndex::get() + 1);
            g_SuppressPresent::set(1);
        } else {
            Lti_DebugPrintf(s_PrecacheRenderingComplete::ptr());
            g_PrecacheComplete::set(1);
            g_PrecacheDisplayIndex::set(0);
            g_PrecacheRequested::set(0);
            RenderShell_PrecacheFinish();
        }
    }
    return reimpl::LtiRenderer_BeginSubmit(render_shell());  // ours; the original is a vtable call (slot 15)
}

void submit_record(PgPrimitive *rec)
{
    // ---- lookups 0x855887-0x855931 ----
    ScreenView *const screen = g_ScreenViewTable::get() + rec->screenIdx;
    uint32_t screen_fx = screen->effectPassOffset;
    if (g_ScreenEffectsEnabled::get() == 0) {
        screen_fx = 0;
    }
    const ViewEntry *const view = g_ViewTable::get() + rec->viewIdx;
    ViewContextRecord *const vctx = g_ViewContextTable::get() + rec->viewContextIdx;
    const uint8_t *const env = g_EnvTable40::get() + rec->envIdx * 0x40;
    PgMaterial *const mat = g_MaterialTable::get()[rec->materialIdx];
    EffectPass *pass = g_EffectPassTable::data()[(int16_t)(uint16_t)(mat->effectPass + (uint16_t)screen_fx)];
    Technique *const tech = rec->techniqueIdx < 0x100 ? g_TechniqueTable::data()[rec->techniqueIdx] : nullptr;
    // VR: this record's shader, so its explicit camera matrices get the
    // per-eye correction wherever in the record they are uploaded.
    view_rewrite::on_record_shader(tech != nullptr ? rec->vertexShader : nullptr);

    // ---- view change 0x855931-0x855977 ----
    if (g_LastViewId::get() != rec->viewId) {
        g_LastViewId::set(rec->viewId);
        g_LastMaterialIdx::set(0xffff);
        g_LastPass::set(nullptr);
        g_CurRTWriteMask::set(4);
        g_LastRecViewFlags::set(0xff);
        DAT_00ce7d24::set(0xff);
        g_CurPassObject::set(0);
    }
    RenderShell_SetViewType(rec->viewType);

    // ---- dirty mask 0x855981-0x8559fc (after SetViewType: it resets caches) ----
    uint32_t dirty = 0;
    if (g_LastViewContextIdx::get() != (uint16_t)rec->viewContextIdx) dirty |= 0x04;
    if (g_CurStencilMode::get() != rec->stencilMode) dirty |= 0x40;
    if (g_LastViewIdx::get() != rec->viewIdx) dirty |= 0x20;
    if (g_LastEnvIdx::get() != rec->envIdx) dirty |= 0x10;
    if (g_LastScreenIdx::get() != rec->screenIdx) dirty |= 0x02;
    if (g_LastMaterialIdx::get() != rec->materialIdx) dirty |= 0x08;
    uint32_t vs_mask = dirty;
    uint32_t ps_mask = dirty;

    IDirect3DDevice9 *dev;

    // ---- vertex side 0x8559fe-0x855bf6 ----
    if (tech != nullptr) {
        if (g_LastTechnique::get() != tech) {
            vs_mask = 0x7f;
        }
        if (g_LastVertexShader::get() != rec->vertexShader) {
            dev = lti_device();
            Device_SetVertexShader(dev, rec->vertexShader);
            g_LastVertexShader::set(rec->vertexShader);
        }
        if ((vs_mask & 0x01) && tech->screenConsts.count) {
            Dx9_SetVertexShaderConstantF(&tech->screenConsts, g_ScreenConstsVS::ptr());
        }
        if (vs_mask & 0x04) {
            view_rewrite::on_upload_gate((uintptr_t)tech, (uintptr_t)vctx);  // VR: was the 0x855a78 MidHook
            if (tech->viewContextData.count) Dx9_SetVertexShaderConstantF(&tech->viewContextData, vctx);
            if (tech->atmosphere.count) Dx9_SetVertexShaderConstantF(&tech->atmosphere, vctx->atmosphereData);
            if (tech->globalLight.count) Dx9_SetVertexShaderConstantF(&tech->globalLight, vctx->globalLightData);
        }
        if ((vs_mask & 0x10) && tech->envConsts.count) {
            Dx9_SetVertexShaderConstantF(&tech->envConsts, env);
        }
        if (vs_mask & 0x08) {
            if (tech->materialConsts.count) Dx9_SetVertexShaderConstantF(&tech->materialConsts, mat->vsConsts);
            if (mat->vsConst170 != nullptr && tech->materialExtra.count) {
                Dx9_SetVertexShaderConstantF(&tech->materialExtra, mat->vsConst170);
            }
        }
        if ((vs_mask & 0x0c) && tech->atmosphereVec.count) {
            alignas(16) float v[4];
            std::memcpy(v, vctx->atmosphereData + 0x60, sizeof(v));
            if (mat->flagsA0 & 0x0e) {
                v[1] = 0.0f;
            }
            dev = lti_device();
            Device_SetVertexShaderConstantF(dev, tech->atmosphereVec.start_reg, v, tech->atmosphereVec.count);
        }
        if (rec->vsConstData != nullptr && tech->recordConsts.count) {
            dev = lti_device();
            Device_SetVertexShaderConstantF(dev, tech->recordConsts.start_reg, rec->vsConstData,
                                            rec->vsConstCount * 3);
        }
    }

    // ---- pass pick 0x855bf8-0x855c75 (on the raw mask: screen | material) ----
    if (dirty & 0x0a) {
        const uint8_t vt = g_CurViewType::get();
        if (is_view_type_1_2_3_7(vt)) {
            pass = RenderShell_PickPass_1_3_7(rec->viewType, mat);
        } else if (vt == 0 || vt == 4) {
            pass = RenderShell_PickPass_0_4(mat);
        } else {
            pass = RenderShell_PickPass_Material(mat, screen_fx);
        }
        if (g_LastPass::get() != pass) {
            ps_mask = 0x7f;
        }
        if (pass != nullptr && pass->viewContextData.count) {
            Dx9_SetPixelShaderConstantF(&pass->viewContextData, vctx);
        }
    }

    // ---- RT write mask 0x855c7a-0x855ce1 ----
    if (!is_view_type_1_2_3_7(g_CurViewType::get())) {
        const uint8_t m = rec->rtWriteMask;
        const uint8_t c = g_CurRTWriteMask::get();
        if (m != c) {
            const uint32_t changed = (uint32_t)m ^ (uint32_t)c;
            if (changed & 5) Dx9_SetRenderState(D3DRS_COLORWRITEENABLE, (m & 1) ? 0 : 0xf);
            if (changed & 6) Dx9_SetRenderState(D3DRS_ZWRITEENABLE, (~(uint32_t)(rec->rtWriteMask >> 1)) & 1);
            g_CurRTWriteMask::set(rec->rtWriteMask);
        }
    }

    // ---- stencil 0x855ce6-0x855d75 ----
    if (rec->stencilMode != g_CurStencilMode::get()) {
        g_CurStencilMode::set(rec->stencilMode);
        if (rec->stencilMode == 1) {
            Dx9_SetRenderState(D3DRS_STENCILENABLE, 1);
            Dx9_SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
            Dx9_SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_REPLACE);
            Dx9_SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            Dx9_SetRenderState(D3DRS_STENCILREF, g_CurStencilMode::get());
            Dx9_SetRenderState(D3DRS_STENCILWRITEMASK, g_CurStencilMode::get());
        } else {
            Dx9_SetRenderState(D3DRS_STENCILENABLE, 0);
            Dx9_SetRenderState(D3DRS_STENCILFUNC, D3DCMP_ALWAYS);
            Dx9_SetRenderState(D3DRS_STENCILPASS, D3DSTENCILOP_KEEP);
            Dx9_SetRenderState(D3DRS_STENCILZFAIL, D3DSTENCILOP_KEEP);
            Dx9_SetRenderState(D3DRS_STENCILWRITEMASK, 0xff);
        }
    }

    // ---- pixel side 0x855d78-0x8562af ----
    if (pass != nullptr) {
        if ((ps_mask & 0x40) && pass->stencilFlag.count) {
            alignas(16) float c[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            if (rec->stencilMode != 2 && DAT_00dfb363::get() != 0) {
                c[0] = 1.0f;
            }
            dev = lti_device();
            Device_SetPixelShaderConstantF(dev, pass->stencilFlag.start_reg, c, pass->stencilFlag.count);
        }
        if (ps_mask & 0x28) {
            if (rec->viewIdx == 1) {
                if (pass->materialMatrix.count) Dx9_SetPixelShaderConstantF(&pass->materialMatrix, mat);
            } else {
                alignas(16) float m[6][4];
                build_material_matrix(m, mat, view);
                if (pass->materialMatrix.count) Dx9_SetPixelShaderConstantF(&pass->materialMatrix, m);
            }
            if (mat->psConst178 != nullptr && pass->materialExtra.count) {
                Dx9_SetPixelShaderConstantF(&pass->materialExtra, mat->psConst178);
            }
        }
        if (ps_mask & 0x02) {
            // The B uploads are not count-gated; only their F partner is.
            if (pass->screenConstsA.count) {
                Dx9_SetPixelShaderConstantF(&pass->screenConstsA, screen->constsA);
                Dx9_SetPixelShaderConstantB(&pass->screenBoolsA, screen->boolsA);
            }
            if (pass->screenConstsB.count) {
                Dx9_SetPixelShaderConstantF(&pass->screenConstsB, screen->constsB);
                Dx9_SetPixelShaderConstantB(&pass->screenBoolsB, screen->boolsB);
            }
        }
        if (ps_mask & 0x04) {
            if (pass->globalLight.count) Dx9_SetPixelShaderConstantF(&pass->globalLight, vctx->globalLightData);
            if (pass->cameraPos.count) Dx9_SetPixelShaderConstantF(&pass->cameraPos, vctx->cameraPos);
        }
        if ((ps_mask & 0x01) && pass->screenConsts.count) {
            Dx9_SetPixelShaderConstantF(&pass->screenConsts, g_ScreenConstsPS::ptr());
        }
    }

    // ---- command stream + draw 0x8562b4-0x856354 ----
    reimpl::RenderCmd_ExecuteStream(rec->cmdStream, tech, pass);
    // VR: a world marker's quad placed for this eye (vr/world_markers.hpp).
    const bool marker = world_markers::before_draw((uint32_t)(rec - g_PrimitiveBase::get()), rec, tech);
    if (tech != nullptr && camera_census::on_draw(rec, tech)) {  // debug tap (true unless a debug skip)
        RenderShell_BindRecordStreams(rec);
        if (g_LastGeometry::get() != rec->geometryObj) {
            if (rec->geometryObj != nullptr) {
                IDirect3DIndexBuffer9 *const indices = rec->geometryObj->indices;
                dev = lti_device();
                Device_SetIndices(dev, indices);
            }
            g_LastGeometry::set(rec->geometryObj);
        }
        if (g_CurViewType::get() == 4) {
            // Occlusion pass: drawn only inside a query, and not at all when
            // queries are off. (No NULL check on g_LtiRenderer, as in the original.)
            const uint32_t flags = *reinterpret_cast<const uint32_t *>(g_LtiRenderer::get() + kLtiRendererFlags);
            if ((flags >> 4) & 1) {
                IDirect3DQuery9 *const q = g_OcclusionQueryArray::get()[rec->objIdx];
                Query_Issue(q, D3DISSUE_BEGIN);
                RenderShell_DrawRecordPrimitives(rec);
                Query_Issue(q, D3DISSUE_END);
            }
        } else {
            RenderShell_DrawRecordPrimitives(rec);
        }
    }

    if (marker) {
        world_markers::after_draw(rec, tech);
    }

    // ---- cache update 0x856359-0x8563a9 (the original's store order) ----
    g_LastScreenIdx::set(rec->screenIdx);
    g_LastViewContextIdx::set(rec->viewContextIdx);
    g_LastMaterialIdx::set(rec->materialIdx);
    g_LastEnvIdx::set(rec->envIdx);
    g_LastViewIdx::set(rec->viewIdx);
    g_LastTechnique::set(tech);
    g_LastPass::set(pass);
}

} // namespace

namespace {

// The HUD records of pass 1, drawn after the walk into the HUD layer.
constexpr uint32_t kMaxHudRecords = 1024;
uint16_t g_hud_records[kMaxHudRecords];

// One submission pass: the original function's body. VR: on headset frames
// the HUD records (vr/hud.hpp) leave the walk; pass 1 draws them once into the
// HUD layer afterwards, pass 2 skips them; each eye then gets the layer
// composited.
void submit_pass(uint32_t pass)
{
    g_InRenderFrame::set(1);
    if (!begin_pass()) {
        g_InRenderFrame::set(0);
        return;
    }

    // ---- frame start 0x85575a-0x8557ab ----
    uint16_t idx = g_PrimitiveHead::get();
    g_SuppressPresent::set(0);
    RenderShell_ResetDrawStateCache(reinterpret_cast<void *>(kDrawStateCacheBlock));
    g_LastRecViewFlags::set(0xff);
    DAT_00ce7d24::set(0xff);
    g_CurPassObject::set(0);
    DAT_01163764::set(0);
    DAT_011697c8::set(0);
    DAT_0116376c::set(0);
    DAT_011697c0::set(0);
    DAT_00ff36fc::set(0);
    bind_named_textures();

    // ---- the sorted record list 0x855874-0x8563af ----
    const bool hud_layer = hud::active();
    uint32_t hud_records = 0;
    while (idx != kPrimListEnd) {
        PgPrimitive *const rec = g_PrimitiveBase::get() + idx;
        gui_census::on_record(idx, rec);  // debug tap
        if (hud_layer && hud::is_hud_record(idx)) {
            if (pass == 1 && hud_records < kMaxHudRecords) g_hud_records[hud_records++] = idx;
        } else {
            submit_record(rec);
        }
        idx = g_PrimitiveSortKeys::at(idx).next;
    }
    if (hud_records != 0) {
        hud::begin_draw();
        for (uint32_t i = 0; i < hud_records; i++) {
            submit_record(g_PrimitiveBase::get() + g_hud_records[i]);
        }
        hud::end_draw();
    }
    view_rewrite::on_record_shader(nullptr);  // no record after the walk

    LtiRenderer_EndSubmit(render_shell());
    end_view_type();
    g_InRenderFrame::set(0);
    hud::composite(pass);  // after EndSubmit: menus StretchRect main -> front there
}

} // namespace

// VR: one frame is two complete passes over the same records, LEFT then
// RIGHT; each pass resets its own draw-state caches, so the second
// re-submits the identical frame for the other eye.
void PgPrimitive_SubmitToGPU()
{
    adaptation::on_frame_begin();  // exposure restore after a render-size switch
    eye_replay::begin_left_pass();
    hud::begin_frame();  // after the pass-1 pose sample
    world_markers::begin_frame();
    submit_pass(1);
    eye_replay::begin_right_pass();
    submit_pass(2);
    eye_replay::end_frame();
}

} // namespace mc2vr::reimpl

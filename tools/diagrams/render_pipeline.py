#!/usr/bin/env python3
"""Generates docs/render_pipeline.svg: the rendering call stack of Mercenaries 2
from the main loop to the headset, and how the world becomes draw commands.

Two views of one frame:
  - top: the data's forms, world -> HMD, each step labelled with the function
    that performs it (numbered);
  - below: the call trees per thread root; a node's number links it to the
    data step it performs. MC2VR's parts are inline dashed tags.

Sources: docs/ and docs/reverse_engineering/ (per-address facts: Ghidra plates).
Edit STEPS / TREES and re-run:  python3 tools/diagrams/render_pipeline.py
"""
import html
import os

OUT = os.path.join(os.path.dirname(__file__), '..', '..', 'docs', 'render_pipeline.svg')

# Palette: the dataviz reference palette; categorical slots 1-3 validated
# (all pairs pass CVD / normal-vision; aqua relies on its visible labels).
PAGE, SURFACE, CARD = '#f9f9f7', '#fcfcfb', '#ffffff'
INK, INK2, INK3, GRID, RULE = '#0b0b0b', '#52514e', '#8a8984', '#e4e3df', '#c9c8c2'
THREAD = {'render': '#eb6834', 'update': '#2a78d6', 'host': '#1baf7a'}

SANS = "Inter,'Segoe UI','DejaVu Sans',Helvetica,Arial,sans-serif"
MONO = "'JetBrains Mono','DejaVu Sans Mono',Consolas,monospace"
CW_SANS, CW_MONO = 6.3, 7.85  # 13 px mono glyph = 0.602 em

# ---------------------------------------------------------------- data view
# (title, what it is, converted to the next by)
STEPS = [
    ('World state', 'entities, physics, AI, camera entity (quat + pos)',
     'RenderQueue_SubmitWorldPackets · CameraTable_FillFromPose'),
    ('Views + camera', 'g_ViewTable entries (pose, cull bounds, LOD) · g_CameraTable 3×4 entries',
     'ViewContext_BuildCameraConstants'),
    ('View contexts', 'per view: VP = inverse(camera)·projection, frusta, cascades → g_ViewContextTable',
     'Scene_EmitViewPrimitives → ~50 emitters → PgPrimitiveScratchRecord_Submit'),
    ('Draw records', '~3k PgPrimitive (0x58 B): technique, material, view ctx, geometry, command stream, vsConst, sort key',
     'PgPrimitive_PublishFrame (sort, ping-pong)'),
    ('Sorted list', 'g_PrimitiveHead → next … in sort-key order, tables repointed at this buffer',
     'PgPrimitive_SubmitToGPU, per record'),
    ('D3D9 calls ×2', 'shaders, constants, states, command-stream ops, DrawIndexedPrimitive — once per eye',
     'eye capture at the pass boundaries'),
    ('Eye images', 'LEFT + RIGHT render targets, shared with the host (+ HUD panel composited)',
     'host blit → xrEndFrame'),
    ('HMD', 'OpenXR swapchains, submitted with the frame\'s own pose / FOV', ''),
]
STEP_MOD = {  # MC2VR's hand in a step
    1: 'HMD union pose in the camera table',
    2: 'cull FOV widened to the HMD',
    3: 'HUD / marker record ranges',
    5: 'two passes; camera constants per eye',
    6: 'HUD layer; capture + share',
}

# ---------------------------------------------------------------- call trees
# Rows: (depth, name, role, step or 0, mod text or '', flags)
# flags: 'R' = MC2VR reimplementation, '?' = uncertain in the notes.
RENDER = [
    (0, 'TaskWorker_RunLoop', 'the render worker: runs the frame\'s task nodes 0 → 3 → 4 → 2', 0, '', ''),
    (1, 'node 0  RenderTask_BeginFrame  0x0046a270', 'scene := shell+0x2b94; shell→vtbl[1]', 0, '', ''),
    (1, 'node 3  RenderTask_BuildScene  0x0046a2c0', '', 0, '', ''),
    (2, 'RenderShell_BuildSceneRecords  0x0085ac50', 'per sub-view (records come from view 1), ~2–4.4 ms', 0, '', ''),
    (3, 'ViewContext_BuildCameraConstants  0x008591ac', 'camera entry from g_CameraTable → view = inverse, LH projection, VP', 2,
     'cull frustum widened to the HMD FOV (+5°)', ''),
    (4, 'Matrix_Inverse4x4 · Frustum_Build · ViewContext_BuildCascadeFrusta', 'view matrix, view frustum, 4 shadow cascades', 2, '', ''),
    (3, 'Scene_EmitViewPrimitives  0x00466da0', 'fills the view-context records, then every renderable kind:', 3, '', ''),
    (4, 'object emitters (vtables 0x00bab1c4…) · Terrain_QuadtreeWalk · PgClouds_EmitRecords', 'the bulk of the records', 3, '', ''),
    (4, 'shadow / reflection / offscreen passes', 'each with its own view context', 3, '', ''),
    (4, 'ViewObjects_DrawAndEmitOcclusionBoxes  0x00468ea0', 'occlusion-query boxes', 3, 'calls NOPed: occlusion culling off', ''),
    (3, 'RenderShell_EndSubView  0x0062ee20  (phase 2)', 'end of the sub-view', 3, 'brackets the world-marker and HUD record ranges', 'R'),
    (4, 'PostFx_ChainAndComposite → PgLtiBloom_EmitPasses', 'bloom, eye adaptation, composite into the front', 3, '', ''),
    (4, 'Hud_DrawWorldMarkers → Hud_DrawQuad', 'depth-tested world icons', 3, 'quads noted; placed per eye at draw time', ''),
    (4, 'Hud_DrawOffscreenIndicators · Bink_DrawOverlays', 'HUD steps 4–5', 3, 'indicators on the 1920×1080 HUD screen', ''),
    (4, 'GuiMgr_DrawView → GuiMgr_DrawViewWidgets', 'HUD step 6: each widget\'s Draw', 3, '', ''),
    (5, 'Gui_DrawTexturedRect · TextWidget_DrawGlyphRuns · MinimapWidget_Draw', 'engine-2D quads, text, minimap', 3, '', ''),
    (5, 'FlashWidget_Draw → GFxMovieRoot_Display → GuiRenderer_*', 'Scaleform movies → GUI records', 3, 'display rect = HUD screen', ''),
    (3, 'PgPrimitiveScratchRecord_Submit  0x00854910', 'every emitter ends here: one 0x58-byte record each', 3, '', ''),
    (1, 'node 4  RenderTask_PublishRecords  0x0046a300', '', 0, '', ''),
    (2, 'LtiRenderer_PublishIfNewFrame → PgPrimitive_PublishFrame', 'new frame id: repoint the tables at this buffer', 4,
     'publishes the HUD / marker ranges with the list', ''),
    (3, 'PgPrimitive_SortList · PgPrimitive_Reset', 'sort → g_PrimitiveHead; flip the ping-pong buffer', 4, '', ''),
    (1, 'node 2  RenderTask_RenderFrame  0x0046a290', '→ RenderShell_RenderFrameTimed', 0, '', ''),
    (2, 'PgPrimitive_SubmitToGPU  0x00855690', 'the frame, as two complete passes over the same records', 5,
     'stereo: pass 1 LEFT, pass 2 RIGHT; one pose sample + lockstep gate', 'R'),
    (3, 'LtiRenderer_BeginSubmit  0x0074aaa0', 'Present (previous frame) · BeginScene · GPU drain · bind + clear targets', 5,
     'Present only in pass 2; drain skipped in pass 2; MSAA', 'R'),
    (3, 'RenderShell_ResetDrawStateCache', 'per pass: makes pass 2 a full re-submit', 5, '', ''),
    (3, 'for each record:', '', 5, '', ''),
    (4, 'RenderShell_SetViewType · dirty mask', 'what changed since the last record', 5, '', ''),
    (4, 'shader + constant uploads', 'VS: screen, viewContextData, light, env, material, vsConst · PS: camera, light', 5,
     'viewContextData rewritten per eye (upload gate)', ''),
    (4, 'RenderShell_PickPass_*', 'effect pass for the material', 5, '', ''),
    (4, 'RenderCmd_ExecuteStream  0x008569d0', 'the record\'s command stream: 27 ops (states, constants, targets, clears)', 5,
     'camera matrices in op 2 corrected per eye', 'R'),
    (4, 'RenderShell_BindRecordStreams → RenderShell_DrawRecordPrimitives', 'DrawIndexedPrimitive', 5,
     'world markers moved per eye, sized to 3°', ''),
    (3, 'HUD records', 'held out of the walk', 6, 'pass 1: drawn once into the 1920×1080 HUD target', ''),
    (3, 'LtiRenderer_EndSubmit  0x0074ac30', 'EndScene', 5, '', ''),
    (3, 'after each pass', '', 6, 'HUD panel composited 1.5 m ahead · eye image captured and shared', ''),
]
UPDATE = [
    (0, 'GameShell_FrameTick  0x00630e10', 'one simulation frame (GameShell_Run\'s loop)', 0, 'dt = the HMD\'s display period', ''),
    (1, 'InGameShellState_FramePipeline  0x004c96f0', '~114 subsystem updates; no D3D', 1, '', ''),
    (2, 'RenderQueue_SubmitWorldPackets  0x0048e620', 'active views → 96-byte packets → g_RenderQueue', 1, '', ''),
    (2, 'ViewManager_UpdateViewPose  0x0048f5c0', 'staged pose → ViewEntry (output only)', 1, '', ''),
    (2, 'ViewEntry_DeriveCullTask  0x00876a90', 'cull bounds + LOD per view (task)', 1, '', '?'),
    (2, 'CameraTable_FillLoop → CameraTable_FillFromPose', 'camera entity → g_CameraTable', 1,
     'entry := HMD union pose (camera, culling, LOD)', '?'),
    (2, 'GuiMgr_Update  0x00609580', 'widget updates, re-layout, minimap border', 0, 'GUI sees a 16:9 1920×1080 screen', ''),
    (1, 'PostUpdateHook call site  0x00630ee7', 'slot 5 (no-op)', 0, 'host events drained; HUD mode switch', ''),
    (1, 'wait for render tasks', 'not the current frame\'s: update runs ~1 frame ahead', 0, '', ''),
]
HOST = [
    (0, 'host frame loop', 'mc2vr_host.exe, D3D11 on DXVK', 0, '', ''),
    (1, 'xrWaitFrame → xrBeginFrame → locate views', 'both eyes at the predicted display time', 0, '', ''),
    (1, 'publish IPC state', 'pose + FOV, IPD → read by the camera table and the lockstep gate', 0, '', ''),
    (1, 'wait for the eye pair', 'poseId = hostFrame + 1', 7, '', ''),
    (1, 'blit shared eye textures → swapchains → xrEndFrame', 'with the pair\'s own pose / FOV', 8, '', ''),
]
TREES = [
    ('RENDER WORKER', 'the build and draw of one frame — runs one frame behind the update thread', 'render', RENDER),
    ('UPDATE THREAD', 'simulation and render producers', 'update', UPDATE),
    ('OPENXR HOST  (separate x64 process)', 'pose in, eye images out', 'host', HOST),
]


def esc(s):
    return html.escape(s)


def t(x, y, s, size=12, color=INK2, weight='normal', fam=SANS, anchor='start', style=''):
    return (f'<text x="{x:.1f}" y="{y:.1f}" font-family="{fam}" font-size="{size}" fill="{color}" '
            f'font-weight="{weight}" text-anchor="{anchor}" {style}>{esc(s)}</text>')


def wrap(s, width, cw):
    out, line = [], ''
    for w in s.split(' '):
        c = (line + ' ' + w).strip()
        if len(c) * cw > width and line:
            out.append(line)
            line = w
        else:
            line = c
    return out + ([line] if line else [])


def badge(x, y, n):
    return (f'<circle cx="{x}" cy="{y}" r="9" fill="{CARD}" stroke="{INK}" stroke-width="1.3"/>'
            + t(x, y + 4, str(n), 11, INK, 'bold', SANS, 'middle'))


def mod_tag(x, y, text, max_w):
    """Dashed MC2VR tag; returns (svg, height)."""
    lines = wrap(text, max_w - 70, CW_SANS)
    h = 8 + 16 * len(lines)
    w = min(max_w, 70 + max(len(l) for l in lines) * CW_SANS + 12)
    s = [f'<rect x="{x}" y="{y}" width="{w:.0f}" height="{h}" rx="4" fill="{SURFACE}" stroke="{INK}" '
         f'stroke-width="1" stroke-dasharray="4 3"/>',
         t(x + 8, y + 16, 'MC2VR', 10.5, INK, 'bold', MONO)]
    for i, l in enumerate(lines):
        s.append(t(x + 60, y + 16 + 16 * i, l, 12, INK))
    return ''.join(s), h


def tree(x0, y0, width, title, sub, thread, rows):
    """Indented call tree; returns (svg, height)."""
    color = THREAD[thread]
    out = [f'<rect x="{x0}" y="{y0}" width="5" height="40" rx="2" fill="{color}"/>',
           t(x0 + 16, y0 + 17, title, 15, INK, 'bold'),
           t(x0 + 16, y0 + 35, sub, 12, INK2)]
    y = y0 + 64
    IND = 26
    stack = []  # (depth, connector x, row center y)
    pending = {}
    for depth, name, role, step, mod, flags in rows:
        x = x0 + 10 + depth * IND
        cy = y + 6
        # connectors: from the nearest shallower row
        while stack and stack[-1][0] >= depth:
            stack.pop()
        if stack:
            px, py = stack[-1][1], stack[-1][2]
            out.append(f'<path d="M{px},{py} V{cy} H{x - 6}" fill="none" stroke="{RULE}" stroke-width="1.2"/>')
        name_w = width - (x - x0) - 40
        nl = wrap(name + ('   (?)' if '?' in flags else ''), name_w, CW_MONO)
        for i, l in enumerate(nl):
            out.append(t(x, cy + 5 + i * 17, l, 13, INK, 'bold', MONO))
        if 'R' in flags:
            rx = x + min(len(nl[-1]) * CW_MONO, name_w) + 10
            out.append(f'<rect x="{rx:.0f}" y="{cy + 5 + (len(nl)-1)*17 - 12}" width="108" height="16" rx="8" fill="{INK}"/>')
            out.append(t(rx + 54, cy + 5 + (len(nl)-1)*17, 'reimplemented', 10.5, CARD, 'bold', SANS, 'middle'))
        yy = cy + 5 + (len(nl) - 1) * 17
        if role:
            for l in wrap(role, name_w, CW_SANS):
                yy += 16
                out.append(t(x, yy, l, 12, INK2))
        if mod:
            svg, h = mod_tag(x, yy + 7, mod, name_w)
            out.append(svg)
            yy += 7 + h
        if step:
            out.append(badge(x0 + width - 12, cy, step))
            out.append(f'<line x1="{x + min(len(nl[0]) * CW_MONO, name_w) + (122 if "R" in flags else 8):.0f}" y1="{cy}" '
                       f'x2="{x0 + width - 24}" y2="{cy}" stroke="{GRID}" stroke-width="1" stroke-dasharray="1 3"/>')
        stack.append((depth, x + 4, yy + 6))  # children connect below this whole row
        y = yy + 18
    return ''.join(out), y - y0


def render():
    W = 2000
    M = 40
    out = []
    # ---- header
    out.append(t(M, 52, 'Mercenaries 2 — one frame, from the world to the headset', 26, INK, 'bold'))
    out.append(t(M, 80, 'Top: the forms the frame\'s data takes, each step performed by the function under its arrow. '
                 'Below: the call trees that do it, per thread; a node\'s number is the step it performs.', 13, INK2))
    out.append(t(M, 99, 'MC2VR\'s parts are dashed tags. Per-address facts are on the Ghidra plates, the design in docs/. '
                 'Generated by tools/diagrams/render_pipeline.py.', 13, INK2))

    # ---- data chain
    y = 140
    out.append(t(M, y, 'HOW THE WORLD BECOMES DRAW COMMANDS', 13, INK, 'bold', style='letter-spacing="1"'))
    n = len(STEPS)
    gap = 34
    cw = (W - 2 * M - gap * (n - 1)) / n
    cy = y + 22
    ch = 268
    for i, (title, what, by) in enumerate(STEPS):
        x = M + i * (cw + gap)
        out.append(f'<rect x="{x:.1f}" y="{cy}" width="{cw:.1f}" height="{ch}" rx="8" fill="{CARD}" stroke="{GRID}" stroke-width="1.2"/>')
        out.append(badge(x + 20, cy + 22, i + 1))
        out.append(t(x + 36, cy + 27, title, 14, INK, 'bold'))
        yy = cy + 50
        for l in wrap(what, cw - 24, CW_SANS):
            out.append(t(x + 12, yy, l, 12, INK2))
            yy += 16
        if i + 1 in STEP_MOD:
            svg, h = mod_tag(x + 8, cy + 132, STEP_MOD[i + 1], cw - 16)
            out.append(svg)
        if by:
            fy = cy + ch - 80
            out.append(f'<line x1="{x + 12:.1f}" y1="{fy}" x2="{x + cw - 12:.1f}" y2="{fy}" stroke="{GRID}" stroke-width="1"/>')
            out.append(t(x + 12, fy + 16, 'next, by', 11, INK3))
            for j, l in enumerate(wrap(by, cw - 24, CW_MONO * 0.86)):
                out.append(t(x + 12, fy + 32 + j * 15, l, 11, INK, 'normal', MONO))
            ax = x + cw
            out.append(f'<line x1="{ax + 3:.1f}" y1="{cy + 22}" x2="{ax + gap - 4:.1f}" y2="{cy + 22}" stroke="{INK2}" '
                       f'stroke-width="1.6" marker-end="url(#a)"/>')
    yb = cy + ch
    y = yb + 36

    # ---- call trees
    out.append(f'<line x1="{M}" y1="{y}" x2="{W - M}" y2="{y}" stroke="{GRID}" stroke-width="1"/>')
    y += 34
    out.append(t(M, y, 'THE CALL STACK', 13, INK, 'bold', style='letter-spacing="1"'))
    y += 24
    colw_l = 1060
    colw_r = W - 2 * M - colw_l - 60
    svg, hl = tree(M, y, colw_l, *TREES[0][:3], TREES[0][3])
    out.append(svg)
    svg, hu = tree(M + colw_l + 60, y, colw_r, *TREES[1][:3], TREES[1][3])
    out.append(svg)
    svg, hh = tree(M + colw_l + 60, y + hu + 40, colw_r, *TREES[2][:3], TREES[2][3])
    out.append(svg)
    # device note
    ry = y + hu + 40 + hh + 40
    rx = M + colw_l + 60
    out.append(f'<rect x="{rx}" y="{ry}" width="5" height="40" rx="2" fill="{INK3}"/>')
    out.append(t(rx + 16, ry + 17, 'D3D9 DEVICE  (DXVK → Vulkan)', 15, INK, 'bold'))
    out.append(t(rx + 16, ry + 35, 'below every draw-side call', 12, INK2))
    yy = ry + 64
    for l in wrap('All engine calls go through the Dx9_* caller-side caches. The carrier owns the front / depth targets '
                  'and renders at the HMD\'s per-eye size.', colw_r - 20, CW_SANS):
        out.append(t(rx + 10, yy, l, 12, INK2))
        yy += 16
    svg, h = mod_tag(rx + 10, yy + 4, 'device hooks: SetVertexShaderConstantF (camera channel) · SetRenderTarget '
                     '(pass gate, MSAA) · SetDepthStencilSurface · SetTexture · StretchRect · Present · Reset · '
                     'TestCooperativeLevel (render-size switch)', colw_r - 20)
    out.append(svg)
    yy += 4 + h
    # cross-thread hand-offs
    hy = yy + 40
    out.append(t(rx, hy, 'HAND-OFFS BETWEEN THE TREES', 13, INK, 'bold', style='letter-spacing="1"'))
    hand = [
        ('update → render', 'g_CameraTable, g_ViewTable, g_RenderQueue (its consumer is unlocated)'),
        ('render → render', 'node 4 publishes the sorted records that node 2 draws'),
        ('host → carrier', 'IPC pose: camera-table fill + pass-1 sample / lockstep gate; events at PostUpdateHook'),
        ('carrier → host', 'shared eye textures + FRAME_READY per eye'),
    ]
    yy = hy + 24
    for k, v in hand:
        out.append(t(rx, yy, k, 12, INK, 'bold', MONO))
        for l in wrap(v, colw_r - 150, CW_SANS):
            out.append(t(rx + 140, yy, l, 12, INK2))
            yy += 16
        yy += 6
    H = max(y + hl, yy) + 50

    svg = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{W}" height="{H:.0f}" viewBox="0 0 {W} {H:.0f}">',
           f'<defs><marker id="a" viewBox="0 0 10 10" refX="9" refY="5" markerWidth="7" markerHeight="7" orient="auto">'
           f'<path d="M0,0 L10,5 L0,10 z" fill="{INK2}"/></marker></defs>',
           f'<rect width="{W}" height="{H:.0f}" fill="{PAGE}"/>'] + out + ['</svg>']
    with open(OUT, 'w') as f:
        f.write('\n'.join(svg))
    print('wrote', os.path.normpath(OUT), W, 'x', int(H))


if __name__ == '__main__':
    render()

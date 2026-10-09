# Render data model (consume side)

All plaintext; layouts on Ghidra plates. Engine naming: [engine_naming.md](engine_naming.md). Frame chain: [frame_chain.md](frame_chain.md).

- **`PgPrimitive`** (0x58): per-draw submit record — draw params, VS, technique, stencil, and table INDICES (`materialIdx/viewIdx/envIdx/viewContextIdx/screenIdx`). Singly-linked submit list (base/head/next-table in the `g_PrimitiveBase` plate; next entries are {6-byte sort key, ushort next}, `0xffff` terminates). Lifecycle: Reset → VM'd build/AssignKeys → `PgPrimitive_SortList` → SubmitToGPU (`0x00855690`, formerly mislabeled RenderShell_RenderFrame).
- **`PgMaterial`** (0x190): NAMED material ("OcclusionMaterial", "PgPrimitiveSubmitToGPU") — texture-projection transform `rows[6][4]` × view scale/offset goes to **pixel**-shader constants (texgen). Rule of thumb: PS-constant transform + textures + blend flags = material, not camera.
- Shaders: `.sho` files via `PgShader_Register(name, file, variant)`; `_pl/_sl/_pl_sl` screen-effect variants + `_li` fallbacks, gated by `g_ScreenEffectsEnabled`.
- Per-draw state is dirty-check cached with `0xffff/0xff` sentinel invalidation; caches are caller-side (Dx9_* layer — [dx9_state_wrapper.md](dx9_state_wrapper.md)), not inside the device wrapper.

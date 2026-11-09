# Lua bindings (script → engine)

How the game's script modules map to engine functions. Every binding is named in Ghidra as `Lua<Module>_<Entry>` with tag `name:anchored` ([ghidra-reva.md](ghidra-reva.md) § Naming).

- **Module registry** `0x00dfc478`: `{const char *name, luaL_Reg *table, const char *initScript}` × 12 bytes, 31 modules, e.g. `Sys`, `Pg`, `Object`, `Player`, `Gui`, `_GuiInternal`, `Graphics`, `Camera`, `Sound`, `Movie`, `LTILibName`. The init script aliases globals, e.g. `ASSERT = Debug.Assert`, `_GuiInternal.nVersion = 2`.
- **Tables:** `luaL_Reg {name, fn}` arrays in `.rdata`, ending at `{0, 0}`. In total 1008 entries over 933 functions.
  - The leading `_` of module and entry names is dropped in the Ghidra names, e.g. `_GuiInternal.SetMinimapBorder` → `LuaGuiInternal_SetMinimapBorder`.
  - A function behind several entries is named after the first; its plate lists all of them.
- `LuaBinding_ReturnNothing` `0x006aef90` (`xor eax,eax; ret`) backs about 60 stubbed-out entries, e.g. most of `Ai` and `Junk`, and `Debug.Printf`.
- Not renamed: 4 entries whose function is a thunk (3 to SecuROM, plus `_GuiInternal.MinimapSetPlayerLocation` → `MinimapSetFocusLocation`).
- **The modules relevant to the HUD:**
  - `_GuiInternal`: widgets, image/text/sprite/flash/movie widgets, minimap, markers, PDA ([gui_widgets.md](gui_widgets.md));
  - `Gui`: objectives, `FindGuiLocation`, `GetReticlePosition`, the marker API;
  - `Graphics`: `SetScreenRatio` / `GetScreenRatio` (aspect mode `+0x2bc4`), `SetFovParams`, near/far, atmosphere.
- Separate from the registry: the Lua standard libraries and Scaleform's ActionScript classes have their own `luaL_Reg`-shaped tables in `.rdata`, which aren't named.

# Mercenaries 2 VR Project

Adding full-featured VR support to the game "Mercenaries 2: World in Flames", through reverse
engineering.

> This project is almost entirely implemented by AI agents, and in its current state extremely
> fragile. I don't recommend even attempting to use it just yet. Write-up, coming soon!

## Features

Options are set in `mc2vr.conf` (deployed to `<game dir>/mc2vr/`; see `conf/mc2vr.conf` for every
key and its default).

**VR**

- Stereo rendering in the headset through OpenXR (SteamVR), with head tracking. Each eye gets its own
  view, rendered at the headset's recommended resolution (`render_size`).
- Culling and level of detail cover the headset's full field of view, so nothing pops in at the edges.
- The HUD is a head-locked panel (`hud_distance`, `hud_size`). World markers such as vehicle entry,
  pickups and objectives sit at their real depth in each eye (`marker_size`).
- First-person on foot: the camera sits at the character's eyes (`first_person`,
  `first_person_eye_height`, `first_person_eye_forward`). Vehicles keep their cameras.
- VSync, motion blur, and depth of field are off in VR.

**Graphics**

- Working MSAA for the 3D scene (`msaa`, off/2/4/8). The game's own MSAA option has no effect in the
  PC port.
- Stable sun shadows: the shadow map's texel grid is fixed in the world, so shadow edges don't crawl
  or flicker with head movement.
- Filtered shadows on grass, bushes, trees, decals and water, which the game draws with single-sample,
  unfiltered shadows.
- Smooth transitions between shadow cascades on every surface, instead of a hard line where shadow
  detail drops.
- Higher shadow resolution (`shadow_resolution`, 2048 by default; the game's is 1024).
- Optional modern material shading (`shading=modern`): physically based lighting with GGX specular,
  Fresnel, Burley diffuse and image-based reflections, keeping the game's own reflective materials.
  It applies to buildings, vehicles, props, terrain, vegetation and decals; water keeps the game's
  lighting.

## Setup

For VR support, the launch script in this project is set up to launch the game under Proton, with
SteamVR as the OpenXR runtime.

1. Copy "launch.conf.example" to "launch.conf", and fill in the missing details: the location of
   your game install, and the location of Steam.
2. Register SteamVR's 32-bit OpenXR runtime, once. The game is 32-bit, and SteamVR on Linux only
   registers its 64-bit runtime:

   ```sh
   ln -s ~/.local/share/Steam/steamapps/common/SteamVR/steamxr_linux32.json \
         ~/.config/openxr/1/active_runtime.i686.json
   ```

   Adjust the SteamVR path if your Steam library is elsewhere. If this is missing, the launcher
   log (`mc2vr_launcher.log`) prints the exact command for your system.
3. Install Microsoft's HLSL compiler into the project's own Wine prefix, once. The mod's shaders
   are compiled at build time with `d3dcompiler_43` (June 2010 DirectX redistributable):

   ```sh
   WINEPREFIX="$PWD/.wine" winetricks -q d3dcompiler_43
   ```
4. Run `./launch.sh`. It builds what's missing, deploys to `<game dir>/mc2vr/` and starts the
   game. Logs land next to the deployed files.

To play without VR, run `./launch_novr.sh`: the same Proton setup, without the mod.

## Ghidra

The majority of the reverse engineering effort in this project is done through ghidra.
Ghidra files are impractical to check in to git, so instead we provide symbols in CSV format, and
data types in C header format.

Note: These are not yet available.

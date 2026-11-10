# Mercenaries 2 VR Project

Adding full-featured VR support to the game "Mercenaries 2: World in Flames", through reverse
engineering.

> This project is almost entirely implemented by AI agents, and in its current state extremely
> fragile. I don't recommend even attempting to use it just yet. Write-up, coming soon!

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
3. Run `./launch.sh`. It builds what's missing, deploys to `<game dir>/mc2vr/` and starts the
   game. Logs land next to the deployed files.

## Ghidra

The majority of the reverse engineering effort in this project is done through ghidra.
Ghidra files are impractical to check in to git, so instead we provide symbols in CSV format, and
data types in C header format.

Note: These are not yet available.

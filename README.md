# Mercenaries 2 VR Project

Adding full-featured VR support to the game "Mercenaries 2: World in Flames", through reverse
engineering.

## Setup

For VR support, the launch script in this project is set up to launch the game under Proton.
You need to configure the location of your game install, and the location of Steam.

Copy "launch.conf.example" to "launch.conf", and fill in the missing details.

## Ghidra

The majority of the reverse engineering effort in this project is done through ghidra.
Ghidra files are impractical to check in to git, so instead we provide symbols in CSV format, and
data types in C header format.

Note: These are not yet available.

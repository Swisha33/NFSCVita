# Testing on the Vita

The VPK is built from your own NFSC.exe: keep it to yourself, never share it (it contains code generated from EA's
executable).

## Install
1. Build: `tools/build.ps1 -Game "Need for Speed - Carbon"` -> `build-release-clang/NFSCVita.vpk`.
2. Copy the VPK to the Vita (VitaShell FTP or USB) and install it.
3. Copy **everything inside** the game folder (`Need for Speed - Carbon/`, about 5.2 GB) to `ux0:data/NFSCVITA/`.
   `NFSC.exe` must end up at `ux0:data/NFSCVITA/NFSC.exe`. The memory card needs the space.
4. Overclock to 444/222/222 or higher with PSVshell before starting.
5. Start "NFS Carbon" from the home screen.

## What to expect at this stage
Nothing has run on hardware yet. The likely first stops, in order:
- a crash or a stop in start-up code (log ends early),
- `d3dx9_30: effect IDI_..._FX not supported yet` (the shaders are the next milestone),
- `msvcr71: C++ exception thrown ...` or `call to unknown target ...` lines.

## What to send back
`ux0:data/NFSCVITA/nfscvita.log` after each run (it is written while the game runs, so it holds the last lines
before a crash). The registry values the game writes are kept in `ux0:data/NFSCVITA/registry.txt`.

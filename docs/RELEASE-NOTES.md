# Release notes

## v0.1.0 (pre-release)

First public build of the Need for Speed Carbon (PC v1.4) port to the PS Vita. **Early and rough**: it boots,
races, free roam and the career start work, but it runs at about 20 fps and has known issues. Please report bugs
with `ux0:data/NFSCVITA/nfscvita.log` attached.

**You need your own copy of the PC game, version 1.4** (the `NFSC.exe` with SHA-256
`baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3`). No game data is included: copy your game folder
to `ux0:data/NFSCVITA/`. See the [README](../README.md#install).

### What works
- Boot, front end, profiles and saves (`ux0:data/NFSCVITA/save/`)
- Races and free roam with sound, HUD, minimap, traffic, car reflections
- Career start (intro race and the first city section)
- Vita control layout (NFSUG2_VITA's), changeable on the Controls screen; rear touch for look back / camera

### Performance
- About 20 fps in the city (CPU-bound). Recommended: [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker)
  (fourth core) and [PSVshell](https://github.com/Electry/PSVshell) at 444 or 500 MHz. The port no longer sets the
  CPU clock itself (setting `cpu_clock`, default `0`).
- Recent gains: the EA audio thread runs on its own core next to the Vita's audio mixer (+8 %), small scenery is
  culled earlier (`scenery_scale`, +7 %), five helper threads the game raises to high priority (network, file and EA thread library) stay at normal
  priority (+7 %).

### Known issues
- Movies are skipped (`skip_movies`): the VP6 video decoder is too slow on the Vita in this version.
- Memory is tight; the track streaming pool is smaller than on PC (`stream_track_kb=46000`), so the world can fill in
  late after a fast move into a new area. Later parts of the career are untested.
- No online play.

### Install
1. Install `NFSCVita.vpk` with VitaShell.
2. Copy everything in your game folder (with `NFSC.exe`) to `ux0:data/NFSCVITA/` (the `MOVIES` folder can be left
   out).
3. Start the game.

The VPK's icon and LiveArea show the game's own logo and art. A VPK you build yourself gets them from your copy
(`tools/vita/livearea_art.py`); without that step it has plain images with the game's name as text.

Unofficial, non-commercial fan port, not affiliated with or endorsed by Electronic Arts.

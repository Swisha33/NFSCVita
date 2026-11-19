# Release notes

## v0.1.2 (pre-release)

Widescreen and headlights. Install the new `NFSCVita.vpk` over the old one; your game files and saves stay.

### New
- **Widescreen** ([#2](https://github.com/Swisha33/NFSCVita/issues/2)): the picture is drawn for the Vita's 960x544
  screen instead of a stretched 4:3 one. Cars and the world keep their shape, the race view shows more to the
  sides, and the HUD sits at the screen edges; menus are centred. It costs no speed (the game still draws 640x480).
  Values from ThirteenAG's NFSCarbon.WidescreenFix, as the macOS port uses them. Setting `widescreen_fov=0/1/2`
  picks the field-of-view variant (default 1).
- **Headlights** ([#2](https://github.com/Swisha33/NFSCVita/issues/2)): the headlight beam lights the road and walls
  at night again. `headlights=off` turns it off.

### Fixed
- A crash at the career start when video memory ran out (render target creation); the GPU heaps now use spare
  video and main memory and give empty chunks back.

## v0.1.1 (pre-release)

Bug-fix update for v0.1.0. Install the new `NFSCVita.vpk` over the old one; your game files and saves stay.

### Fixed
- **Crash when the free roam starts after the career prologue** ("guest memory exhausted"). The city's 74 MB
  section is loaded again at that point and no longer finds its memory taken: the freed range is kept for it, and
  under memory pressure the port now frees its own copies of static vertex/index buffers (they come back when the
  game needs them).
- **Road and building textures sliding when the camera view changes.** Some world shaders were given a random
  texture offset; it is now zero as on PC.
- **Missing characters in cutscenes**: bodies and faces are drawn (skinned meshes and the facial animation).

### Changes
- About 6 MB more memory for the game (smaller host heap).
- New diagnostic settings for bug reports: `pool_log=N` (the game's memory pools every N seconds),
  `debug_texoffset=on`.

## v0.1.0 (pre-release)

First public build of the Need for Speed Carbon (PC v1.4) port to the PS Vita. **Early and rough**: it boots,
races, free roam and the career start work, but it runs at about 20 fps and has known issues. Please report bugs
with `ux0:data/NFSCVITA/nfscvita.log` attached.

This port is vibe-coded: written almost entirely by an AI coding assistant (Claude Code), directed and play-tested
on a real Vita by the maintainer.

**You need your own copy of the PC game, version 1.4** (the `NFSC.exe` with SHA-256
`baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3`). No game data is included: copy your game folder
to `ux0:data/NFSCVITA/`. See the [README](../README.md#install).

### What works
- Boot, front end, profiles and saves (`ux0:data/NFSCVITA/save/`)
- Races and free roam with sound, HUD, minimap, traffic, car reflections
- Career start (intro race and the first city section)
- Vita control layout (NFSUG2_VITA's), changeable on the Controls screen; rear touch for look back / camera

### Performance
- About 20 fps in the city (CPU-bound; measured at 500 MHz with CapUnlocker). Recommended:
  [CapUnlocker](https://github.com/GrapheneCt/CapUnlocker) (fourth core) and [PSVshell](https://github.com/Electry/PSVshell)
  at 500 MHz. The port no longer sets the
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

# Release notes

## v0.1.7 (pre-release)

Faster again, especially where the frame rate used to drop. Install the new `NFSCVita.vpk` over the old one; your
game files and saves stay.

### New
- **About 14 % faster.** Free roam at 500 MHz: about 23.4 fps instead of about 20.5; the slowest stretches about
  17.5 instead of 15.5 fps.
  - **Profile-guided optimization (PGO):** the build is optimized with profiles recorded on a real Vita (free roam,
    a race with full traffic, the menus), as [NFSUG2_VITA](https://github.com/danos-x5/NFSUG2_VITA) does. About
    +10 %.
  - **Smaller vertex uploads:** the game's per-frame buffers for particles, light glows and similar quads were copied
    to the GPU whole (180 KB) on every frame; now only the part the game wrote (usually ~25 KB). About +3 %.
  - **Cheaper clock reads:** the game reads the clock about 1000 times a frame, most of them while new city
    sections load.

### Known issues
- Rare short freezes (up to about a second) when the game's sound thread holds its lock for a long time; being
  investigated.
- Spoken audio follows your game files: a copy installed in another language speaks that language.

## v0.1.6 (pre-release)

Faster, a working world map, and the keyboard opens by itself. Install the new `NFSCVita.vpk` over the old one; your
game files and saves stay.

### New
- **About 6 % faster.** The recompiler now keeps the game's local variables in CPU registers instead of memory where
  no other code can see them (the stack-slot optimization from [NFSUG2_VITA](https://github.com/danos-x5/NFSUG2_VITA)).
  Free roam at 500 MHz: about 20 fps instead of about 18.8.
- **World map and mouse menus work.** Carbon's PC map uses a mouse cursor, which the Vita did not have. The rear
  touch is now the cursor: the whole rear panel stands for the screen, the cursor goes where your finger is, and a
  short tap clicks (selects a race). The right stick moves it too. With the finger at the panel's edge (or the right
  stick pushing against it) the map scrolls that way. Only in menus and maps; while driving the rear touch keeps its
  look-back and camera functions.
- **The keyboard opens by itself** on text fields such as the profile name, and its title says what you are
  editing. Select + Triangle still opens it by hand.

### Fixed
- The game's log of the previous run is kept as `ux0:data/NFSCVITA/nfscvita.prev.log`, so a crash log survives a
  restart (please attach it to bug reports after a crash).

### Known issues
- Spoken audio follows your game files: a copy installed in another language speaks that language.

## v0.1.5 (pre-release)

The quick race freeze with a teammate is fixed. Install the new `NFSCVita.vpk` over the old one; your game files and
saves stay.

### Fixed
- **A quick race froze while loading when the teammate's car had finished loading in the menu**
  ([#1](https://github.com/Swisha33/NFSCVita/issues/1)). At the race start the game waits in a loop for the cars to
  load, while one of EA's worker threads does the reads. On the Vita only one thread runs game code at a time, and
  that loop never let the worker in: a deadlock. The loop now gives way for 1 ms per pass (`race_start_yield=off`
  restores the old behaviour). Loading that race can still take a while.

## v0.1.4 (pre-release)

Quick races with top-tier cars load again. Install the new `NFSCVita.vpk` over the old one; your game files and saves
stay.

### Fixed
- **Quick race (and other races) with top-tier cars such as the Lamborghinis froze while loading**
  ([#1](https://github.com/Swisha33/NFSCVita/issues/1)). The game's car memory was cut to 24 MB to make room for the
  city; a race full of exotic cars needs over 33 MB. It is now 46 MB (10 MB more memory for the game overall), and
  the city's 74 MB track section has its memory range kept free from the start, so both fit.

### Known issue
- A quick race can still freeze while loading if you pick a **teammate** and wait until **his car has finished
  loading** in the menu before starting. Start the race (X) before his car appears to avoid it. Being worked on.

## v0.1.3 (pre-release)

No more long freeze at the end of a race, and new artwork. Install the new `NFSCVita.vpk` over the old one; your game
files and saves stay.

### Fixed
- **The game froze for 20 seconds to over 2 minutes** (sound kept playing) when a race ended or a big track section
  was dropped. The game's chunk unloader searched the whole section again for every 64 pieces it removed; that took
  no time on a PC but minutes on the Vita. A native version does it in one pass. `fast_chunk_unload=off` runs the
  game's own code (for comparison).

### New
- **Icon and LiveArea artwork** by Reddit user u/Either-Version-9577 (also in the updated v0.1.2 VPK).

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

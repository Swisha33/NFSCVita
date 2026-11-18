# Clues from the other ports (read 2026-11-18)

Sources: refs/nfs-carbon-mac (CLAUDE.md, kit/CHANGELOG.md), refs/nfscarbon-android (README, docs/, launcher and
runtime source), refs/NFSUG2_VITA (README, src/lib/registry.cpp), refs/UG1VITA. Only what applies to our open
problems is listed; addresses are for our NFSC.exe v1.4 (the Mac and Android ports use the same exe).

## Performance
- **Graphics settings are the biggest lever and cost no code.** Our registry.txt runs Carbon at
  g_PerformanceLevel 1 (Medium) with g_CarEnvironmentMapEnable 2, g_RoadReflectionEnable 1, g_RainEnable 1,
  g_ParticleSystemEnable 1, g_WorldLodLevel 1, g_CarLodLevel 1, g_ShaderDetailLevel 2, g_VisualTreatment 1.
  - Android "low" (Redmi Note 8 / Snapdragon 665, ~10 fps with 8 cars): performance level 5 (Custom, so the
    single values apply), world 0, vehicle 0, shader 0, shadows 0, reflection rate 0, road reflection 0, texture
    filtering 0, particles off, weather off, post-processing off, visual treatment off ("ultra low" also caps
    the render height at 360).
  - NFSUG2_VITA's "vita" preset (UG2, same engine family): road reflections off, world LOD 0, car env map on,
    particles on, performance level 2. Their README: "Above the Medium detail setting the Vita will definitely
    struggle."
  - Mac: levels 0 Low, 1 Medium, 2 High, 4 Maximum, 5 Custom ([0x00ab0ae8]); 007115d0 applies a level. If
    FirstTime = 1, 0071d0a0 re-detects the hardware from the adapter name every start and overwrites the
    settings (the game opens the key with KEY_READ and still writes FirstTime: writes through a read-only handle
    must fail, as on Windows).
  - The PC capture (work/pctrace) at its default level drew reflection pass + scene + downscale/blur chain;
    each reflection / cube face is a whole extra scene pass (run 88's dump: several 256x256 cube targets per
    frame).
- **Audio thread without the lock** (Mac kit, "mixer on its own core", same idea as our run 83-86): their
  lock-free import set, measured: Enter/Leave/TryEnterCriticalSection ~4000/s, Sleep 100/s, timeGetTime, QPC,
  Interlocked*, ReleaseSemaphore, **IDirectSoundBuffer GetCurrentPosition / Lock / Unlock ~200/s** (all DirectSound
  shims under one mutex). We still take the lock for the DirectSound calls and ReleaseSemaphore. Their lesson:
  "the mixer must not be delayed" (main-thread-first scheduling made sound cut out); and the remaining stutter was
  the main thread's own heavy frames. Their next idea: native versions of the hot DSP loops (008ad12e, 008ad305,
  008ab884, 008b70b0, 008abbb2, 008ac988) and translated bank routines.
- Mac's other target: the kit's work under the game's draw code (put_matrices, apply_pass, queue_draw), the
  same as our run 94-96 effect-emulation work.
- NFSUG2_VITA: 30 fps lock, PGO from Vita runs (~3 %), CapUnlocker 4th core, scenery cull settings
  (world_lod_pixels / world_min_pixels: small scenery uses the simple model or is skipped).
- Android: indirect calls go through a direct-mapped cache before the hash table (we have a 2-way slot table).

## Missing cars (traffic / opponents)
- Mac: bMalloc 00476b20's out-of-memory path **writes to address 0, which does not crash in a port**: a failed
  allocation is silent. Our pools are shrunk (stream_track_kb etc.), so cars failing to load is plausible.
  We do not detect writes to guest address 0 yet (STATUS: "should be fatal with a log line").
- Traffic density is pushed at 007a9e3f / 007a9e43 / 007a9e47 (Extra Options' traffic setting); opponents
  004ac87c / 004ac88f / 004ac8dd / 004ac8e6 (Quick Race, fn 004ac840).
- Track streaming pool: 006b6e00 -> 007a42e0, 0x445c000 / 0x441c000 + 65000 KB + extras, about 133 MB on PC
  (MOV at 006b6ef2 / 006b6ef9).

## Rendering
- Texture flicker: NFSUG2_VITA lists "World objects sometimes show the wrong texture for a while. A fix is in
  testing." The same family of bug probably (texture cache / reuse), not solved there yet.
- Android: car and traffic lights drew as long lines because their translator's fsincos left sin/cos swapped.
  **Ours is correct** (st1 = sin, st0 = cos, checked tools/recompiler/disasm/codegen/fpu.py).
- Mac kit change 27: Autosculpt locks vertex buffers (0070ed70 / 0070eeb0 / 0070eda0) and never unlocks them;
  works on Windows because the pointer is the buffer. Matters for Autosculpt (and any still-locked buffer).
- Mac kit changes 7-11, 28: D3DXFillVolumeTexture (LUT), shared-parameter texture references, cube/volume DDS,
  D3DXLoadSurfaceFromSurface with DXT1 alpha: all done here (runs 2x-95).
- Mac: car vinyl skin pipeline 007dba00 -> 007d7580 -> 00732ca0 (D3DXLoadSurfaceFromSurface); crew logos and
  menu previews use the same rasterizer 00752070.
- Mac HD-reflection addresses (sizes of the reflection targets): road 0071a950, car cube 0070de00 (AND at
  0070de38), mirror 0070dab0, PiP 0xa63b00/04, road blur 007130e0, sky 007afca1. Useful the other way round:
  making these targets SMALLER on the Vita.

## Gameplay / stability
- Judder: the main loop's frame time [0x00a99a5c] is quantized to 1/3600 s; the sim service 00761150 steps
  fixed 1/60 s with an accumulator [0x00b4d7ec], so frames flip between 0 and 2 steps. Mac fix (smooth_sim):
  pull the remainder 10 % (max 0.25 ms) towards half a step before each call.
- Netbios / network thread: guest thread start 0094c740 (DirtySock idle thread) is missing from analyses; a
  network-thread stray write at profile load caused "Start Career" instead of "Resume Career" on the Mac.
- MoviePlaybackFix: 00888908 waits for the audio reader; Mac waits up to 5 s alertably only for movies with
  audio chunks.
- Safehouse: 00730840 unmasks FPU exceptions (_controlfp(3, 0x8001f)) before CreateDevice; D3D9 masks them again
  unless D3DCREATE_FPU_PRESERVE.

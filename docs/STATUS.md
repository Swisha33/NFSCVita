# Status and roadmap

Last update: 2026-11-18 (evening).

## Decision: path 1, fork UG1VITA

Of the three paths in the original plan, path 1 (UG1VITA's tooling applied to NFSC.exe) is taken:

- UG1VITA is the only existing **Vita** runtime: GXM renderer, Vita memory arena, audio, input, IME, VPK
  packaging. The other two would need all of that written from scratch.
- It builds on Windows with PowerShell + Python and fetches VitaSDK/LLVM itself, which matches this machine.
- Its runtime is barely coupled to Underground (about 30 references, mostly build config), so the game-specific
  part is `tools/recompiler/generate.py` tables and `src/nfsu/`.
- The Carbon knowledge (entry points, patches, D3DX gaps) from the Mac port is data that can be carried over by hand.
- 32-bit ARM is proven for Carbon: the Android port also ships an ARMv7 build.

## Spike result (2026-11-15, UG1VITA discovery over NFSC.exe, 31 min on an i5-6300U, ~4 GB RAM)

| | NFSC.exe |
|---|---|
| functions found | 42,753 (Mac port via Ghidra: ~50,000) |
| x86 instructions | 1,450,756 (4.49 MB of the 6.03 MB .text = 74.5 %) |
| jump tables | 659 |
| bytes not attributed to code | 1.24 MB in 8,956 ranges (data in .text, padding, and missed functions) |
| MMX/SSE | present (movq, pmaddwd, mulps, ...): VP6 video decoder and math paths; must be gated like UG1VITA's SIMD_GATED |

### First full generation (2026-11-15, `tools/recompiler/generate.py`, 54 min, peaks at ~9 GB incl. swap)
491 bucket files + registration, 239 MB of C++. Problems the report listed, and what was done:

| Problem | Count | Fix |
|---|---|---|
| x87 `fucompp` / `fcompi` / `fucompi` / `fldlg2` not in the code generator | 679 / 17 / 1 / 3 | added (codegen/fpu.py, fpustack.py) |
| `lock cmpxchg` | 1 (0x8dae47) | added (codegen/integer.py) |
| MMX `punpckhdq` / `movntq` / `paddusb` | 24 / 20 / 1 | added with NEON versions (codegen/mmx.py, include/mmx.h) |
| SSE/SSE2 outside gated ranges | 2,450 in 0x886f4e-0x8d81d6 | gated: EA VP6 video + audio middleware, CPUID-dispatched |
| "flag convention" readers | 11 | entries guessed from data (0x6e6f4d = "Mon"): NOT_INDIRECT |

Second generation (same day, 66 min): **0 problems**. Left in the report, all expected: 11 `int3` padding traps, 119
traps after calls that never return, 1 undecodable byte run, and 2,450 SSE/SSE2 sites in the gated ranges.

Note: the writer's handling of mangled C++ imports and data imports (`_iob`, `_adjust_fdiv`) was added during that run,
so `src/nfsc/generated/nfsc.cpp` from it still has the old registration. The next generation fixes it.

### Code size, measured
Every 25th bucket compiled with the clang game-code flags (`tools/measure_codesize.ps1`): 0.45 MB of functions for
8.5 MB of C++, i.e. **about 13 MB of ARM code for the whole game** (x86 code expands about 2.7x). The earlier guess
of 83-166 MB was far off: code size is not a memory problem. The guest heap and Carbon's pools are.

## What Carbon needs beyond Underground

| Area | Underground (UG1VITA has it) | Carbon needs |
|---|---|---|
| C runtime | linked into Speed.exe, translated with it | imported from msvcr71.dll (134 functions) and msvcp71.dll (3): translate the rebased DLLs or write native shims (95 not yet in UG1VITA) |
| D3DX | linked in, native overrides | imported from d3dx9_30.dll (19 functions, all new: effect pool, effect from resource, cube/volume DDS, LoadSurfaceFromSurface, FillVolumeTexture, math) |
| Effects | 20 Cg ports of Underground's effects | 30 fx_2_0 binaries in NFSC.exe resources (IDI_*_FX, 379 KB), precompiled, so no HLSL compiler; each needs a Cg port + GXP |
| Win32 | | +15 kernel32, +6 user32, +7 advapi32, +2 rpcrt4, +14 ws2_32 (stubs for offline play) |
| Memory | guest heap 96-192 MB | track streaming pool alone ~130 MB on PC (006b6e00); must be shrunk |
| Audio banks | x86 routines in .abk translated (bankcode.py) | same mechanism (Android port does it for Carbon) |
| Videos | VP6 | VP6 too; Mac port needed MoviePlaybackFix (00888908 audio wait) |

## Milestones

0. **Setup** (done 2026-11-15): game checked (v1.4 hash OK), references downloaded to `refs/`, UG1VITA imported
   as the base, discovery spike written (`tools/recompiler/spike_nfsc.py`).
1. **Generator for NFSC.exe** (done 2026-11-15): `tools/recompiler/generate.py` now holds NFSC tables; translates
   with 0 problems. Toolchain (VitaSDK, LLVM 22.1.8) in `.deps/`; CMake/build scripts point at `src/nfsc/`.
   **Import layer** (done 2026-11-15, docs/IMPORTS.md): shims for all 348 imports; the runtime and the import
   registration compile. Generator now also handles Carbon's setjmp (SETJMP 0x87ef70) and the x87 stack effect of
   CRT imports (IMPORT_FPU_DELTAS).
   **First VPK** (2026-11-15): `tools/build.ps1` regenerates (0 problems, 47 min) and links
   `build-release-clang/NFSCVita.vpk` (12 MB; ELF .text 19.6 MB incl. runtime). Registry defaults for Carbon's key
   (Language "English US", Install Dir "C:\"). **Next: first run on hardware** (docs/TESTING.md), then fix what the
   log shows on the way to the EA logo.
   **Hardware runs** (2026-11-15, logs pulled with `curl.exe -s -o work\vita_runN.log
   "ftp://<vita-ip>:1337/ux0:/data/NFSCVITA/nfscvita.log"`; VitaShell FTP on the user's Vita):
   1. SSE `movss` at 0x008d7f29 in EA's audio library (used without a CPUID check) -> codegen/sse.py translates SSE
      on cpu.xmm (full regeneration).
   2. RaiseException 0x406d1388 (MSVC thread naming) -> returns, logs the name (kernel32.cpp).
   3. IDirect3DDevice9::CreateQuery -> Query object: event = finished, occlusion = 4096 pixels (d3d9.cpp).
   4. D3DXCreateEffectFromResourceA failed, game called address 0 -> d3dx9_fx.cpp (all 30 effects load; list in
      run 5's log: IDI_WORLD_FX 43 params/4 techniques ... IDI_HDR_FX 15 techniques).
   5. Guest memory exhausted: streaming pool 139,059,376 bytes (006b6e00 -> 007a42e0 at 0xb70650) = 68.4 MB fixed
      (MOV EBX 0x445c000 at 006b6ef2 / 0x441c000 at 006b6ef9) + [0xa62c48] KB (65000; sub-pool 0xb77018 carved by
      007e4910; other mode uses [0xa62c4c] = 1) + extras. Fix: main.cpp writes [0xa62c48] = cfg stream_subpool_kb
      (default 24000) -> pool ~93 MB. Also DXT5 textures (FormatBC3, UBC3) added.
   6. `IDirect3DDevice9::CreateVolumeTexture is not implemented`, after `D3DXCreateVolumeTextureFromFileInMemory`
      and `D3DXCreateCubeTextureFromFileInMemory not supported yet`. Mac port notes (refs/nfs-carbon-mac CLAUDE.md,
      kit changes 7-10): 0073c1a0 creates an N^3 A8R8G8B8 volume (colour-grading LUT) filled by
      D3DXFillVolumeTexture with callback 0073c100; GLOBALB.BUN handlers 0070f6c0 (five 256^2 DXT1 DDS cube maps
      into 0xab09d0[]) and 0070f670 (32^3 A8R8G8B8 DDS volume, 6 mips, into 0xab09e8). Fix (built 2026-11-15, not
      yet run): `src/lib/winapi/d3d9_volume.cpp` (VolumeTexture, KindVolumeTexture, 22-slot vtable; levels packed
      in one guest MemMap, LockBox hands them out; A8R8G8B8/X8R8G8B8/A8/L8; binding accepted, samples nothing
      yet), DDS cube + volume loaders and D3DXFillVolumeTexture (guest callback per texel via crt::callGuest) in
      d3dx9_30.cpp. Also fixed: IDirect3DQuery9 and the volume texture had no D3D9_UNPACKER, so Query methods
      read the guest COM block as the host object (m_type/m_device garbage).
      Expected log lines: "volume DDS loaded: 32x32x32", "D3DXFillVolumeTexture filled NxNxN", "cube DDS loaded".
      Later (rendering): the LUT as a 2D atlas (N x N^2) for IDI_VISUALTREATMENT_FX; the Mac port was black in
      3D without it. Mind the Mac port's kit changes 8/9: shared effect-pool params and pass sampler slots must
      take their own reference to the LUT, or it is freed at race load.
   Free user RAM after the guest arena: 48 MB; at the stop in run 5 only 30 MB was left.
   Runs 7-18 are automated: `tools/vita.ps1 run` (vitacompanion: FTP 1337, commands 1338) uploads the new
   eboot.bin, closes and starts NFSCVITA0 and fetches the log; no VPK install for code-only changes.
   7. Volume DDS, cube DDS and the LUT fill work. StretchRect -> logged no-op (d3d9_device.cpp).
   8. x87 overflow: the analysis inferred a float result at the indirect calls 0067a398/0067a3c5 of memory-mode
      function 0067a340 and RC_FPU_CALL pushed 0.0 each call. Codegen now emits RC_FPU_CALL_LOG (log only) in
      functions whose x87 stack is in cpu.fpu (full regeneration; confirmed in run 9: "kept (stack in cpu.fpu)").
   9-17. Log went silent ~25 s in while the game ran on (the user saw a grey screen with stuttering movie audio).
      The open-per-line log lost every line from then on; the log is now one sceIo descriptor kept open
      (platform/log.cpp). Diagnostics added: Present heartbeat every 10 s (frames, draws), infinite waits logged
      after 10 s with the guest caller (kernel32.cpp), a stall watchdog thread (winapp.cpp: logs the lock holder's
      stack words after 8 s without a frame; "alive" line every 10 s with the open guest files), abort/exit/terminate
      hooks (main.cpp). Movies: ~8 fps (VP6 decoded by guest code), grey (no shader translation yet).
   18. Movies skipped: main.cpp sets the game's skip-movies switch [0xa9e6d8] = 1 (setting skip_movies, default
      on; 005baea0 posts 0xc3960eb9 at once). Front end then runs at ~16 fps, ~570 draws/frame. Open:
      - `sceGxmExecuteCommandList failed: 0x805b001f` (2627 times) once draws reach ~66k per 10 s.
      - Stop: `dynamic_call: no method registered at guest address 0x01afd564` (heap address: very likely the
        x86 routines of an audio bank, SOUND/**/*.abk). Port UG1VITA's bank code (tools/recompiler/bankcode.py,
        src/nfsu/bankcode.cpp) with Carbon's loader / library table / bank list addresses.
      - D3DXLoadSurfaceFromSurface still unimplemented (00732ca0 at boot; vinyls later).
   19. The 0x01afd564 stop did not come back (5+ min in the front end at ~17 fps). A missing method now logs the
      bytes at the target, the nearest "ABKC" header below it and the caller's stack words (winapp.cpp), so the
      next occurrence tells whether it is bank code. Carbon's exe has no "ABKC" compare: its bank list and loader
      are still to be found if the port is needed.
   20-21. The rejected lists were small filter scenes (80x60, 160x120, 128x128; 2 draws each). Scenes under
      JobMinimumDraws (32) now draw on the immediate context; a rejected list is drawn again immediately and
      logged once per shape. Run 21: no rejections. The log is one open descriptor, so FTP saw a stale size:
      logSync() every 10 s from the watchdog.
   22. Stable front end (~17 fps, ~570 draws/frame). The user's photo: title screen, but every textured draw is a
      flat quad (logo = white rectangle, text = solid blocks): effect passes applied nothing to the device.
   23. d3dx9_fx.cpp applyPass (BeginPass, CommitChanges): render / texture stage states of the pass (Wine's state
      table), the textures + sampler states of the pixel shader's samplers (CTAB register set 3 -> sampler param ->
      its Texture state -> texture param), and direct Texture states. Preshader-computed states are skipped. ~15 fps.
      Still blocks on screen (user).
   24-28. debug_frame_dump showed the textures bound and the uploads real (DXT data, upload fingerprints in
      d3d9_resources.cpp, declarations logged), but stage 0 colour/alpha op = DISABLE in the game's shader draws, so
      the fixed-function stand-in drew the vertex colour only. applyPass now sets stage 0 to texture x diffuse.
   29. Title screen with readable "click to continue" (user). The "freezes" of runs 27/29 were crashes whose last
      lines reached the log only later: a call into bank code (ABKC header 0x01afd4c0, target +0xa4).
   30. src/nfsc/bankcode.cpp: UG1VITA's bank code interpreter (finds the bank by scanning down to its ABKC header,
      offsets relative or absolute, adds sub r32,r/m32 and imm32 group 1; survey work/abk_survey.py: 389 routines,
      207 distinct, ~15 instruction forms). Press start: menu for a second, then a call to 0x008a2dda (sound player
      table 0x00a03ec0, never discovered). generate.py EXTRA_ENTRIES += 0x008a2dda and five other data-pointed
      functions after padding (work/missing_entries.py); full regeneration.
   31. Main menu stays up (user: ~9 fps; log 12-15 fps, 400-600 draws/frame). "Start Career" -> call to
      0x00668a10: registered by `push 0x668a10` at 00669d83, rejected by discovery ("code pointer / runs into an
      entry"). The analysis JSON's `rejected` list (1052) holds no other function-like candidate, so this should be
      the last miss that the image itself references. EXTRA_ENTRIES += 0x00668a10; full regeneration (run 32).
      Also: waits on a thread handle now end when the thread returns (kernel32.cpp, Thread::finished); before, they
      failed with "not a waitable handle".
      For the porting guide: after each generation, list `rejected` entries whose first bytes are a prologue after
      padding and add them as trusted entries before the first hardware run.
   32. (2026-11-16 00:17, build with 0x00668a10 as an entry; automated run, nobody at the Vita.) Boot unchanged:
      all 30 effects, volume/cube DDS, LUT fills, ~2 fps during the first 10 s (loading). The log stopped growing
      at 14,880 bytes after the first watchdog line (~20 s in) and a later `vita.ps1 log` gave the same size:
      hang or crash, its last lines may only show after a Vita restart (`vita.ps1 log` then). "Start Career" not
      tested yet. Next: restart the Vita, fetch the log, rerun, press Start Career. Project moved to a stronger PC
      after this run (docs/HANDOFF.md).
      Correction (2026-11-16, log fetched again from the new PC, 17,038 bytes): it was not a hang. The front end ran
      on for ~50 s (12 fps, 400-650 draws/frame; first bank routine interpreted) and stopped with
      `dynamic_call: no method registered at guest address 0x00530030` (bytes 8b 44 24 04 83 e8 00 74 11 48 74 06,
      caller 004a777c). Cause: vtable slot 009cde88 holds 0x00530030 right after 0x00530020, and discovery's text
      filter (discover.py, "UTF-16 characters after UTF-16 characters") took both dwords for a string, so the
      function was never an entry. `work/nfsc_look.py missing` (unregistered code pointers in .rdata/.data that
      start after padding and decode as code) lists six such functions: 0x00530030, 0x00530060 (same vtable),
      0x00670060 (009e1784, after 0x00670050), 0x0050fe30 (vtable 009cb6b4, after a float constant), 0x00500040
      and 0x00736f70 (referenced only from real text; added anyway, cost nothing). All in EXTRA_ENTRIES; full
      regeneration for run 33 (first build on the new PC, .deps fetched again).
      For the porting guide: a function at an address 0x00XX00YY with printable XX/YY looks like UTF-16 in a vtable;
      run the `missing` scan after every generation, it does not rely on the analysis JSON's `rejected` list.
   33. (2026-11-16, first build on the new PC: toolchain fetch + regeneration + compile 48 min, of which
      nfsc.cpp alone ~22 min.) The six entries are registered; the front end runs past run 32's stop. "Start Career"
      (user) loads and stops: `guest memory exhausted (requested 74368560 bytes)`, arena 192 MB, 25 MB free.
   34-35. Who asks: MemMap::addReport hooks now log the lock holder's guest stack, registers and raw stack words
      (winapp.cpp), and heap.cpp logs every large block >= 8 MB. Lesson: direct calls leave no return address on
      the guest stack (only indirect ones do), so the "code addresses on the stack" are mostly SEH handlers; the
      frame was identified from saved registers and arguments instead. Large blocks at boot: 33,554,596 at
      0x01afd000 (holds the sound banks), 9,381,972, 97,075,364 (pool 7 = streaming pool), 14,278,200.
      The request: 00476560 (the game's pool allocator: a pool without room grows by malloc of
      max(size + align + 0x10, [0xa4e29c])) for bMalloc 00476b20(size 0x046ec580 = 74,368,384, flags 0x2000 =
      main pool, align 0x80), called from the track streamer at 0079ebf4: a section whose size ([ebp+0x18])
      exceeds the largest free block of pool 7 (004750f0(7)) is, if it is section 2500 (0x9c4), allocated from the
      main heap instead (other sections are counted as pending). The career intro canyon (STREAML5RB.BUN, 82.8 MB)
      has a 74.4 MB section 2500; pool 7's track part is 0x441c000 + 0x80000 = 71.9 MB (00699a60 / 006b6ef2;
      [0xa9e668] set selects 0x441c000 and takes 0x40000 off the car sub-pool; + 0xc8000 from 00557f70).
      Related: 006bbe70 re-carves the car sub-pool at race load, with [0xa62c4c] = 1 KB when 006999c0() == 1;
      whether that ran before the section load is not known yet.
   36. generate.py PATCHES: track part 0x4800000 / 0x4840000 (+0x80000 = 76.0 MB), so the section fits pool 7
      (+4.1 MB of arena). main.cpp logs pool 7's free list and the mode switches on exhaustion. Full regeneration.
      Result: same stop, same request. The premise was wrong twice: (a) the pool table slot [0xa84c20 + 7*4] is 0
      at that moment (pool 7 is not registered while the track loads), so section 2500 goes to the main heap
      whatever the pool size, on PC as well; (b) the section is not the canyon's: L5RB.BUN's section 2500 is
      0x23520 bytes, the 0x046ec580 one is in L5RA.BUN (the city; entry at file offset 0x3d148: number 0x9c4,
      offset 0x00dbd000 in STREAML5RA.BUN, size twice). Switches at that point: [0xa9e668] 0, [0xb69c10] 0,
      [0xa631b8] 1, [0xa9e620] 0. The 33,554,596-byte block is a bMalloc of exactly 0x2000000 (+0xa4 pool
      overhead); requesters to check: 005ad902 (0059dd90) and 005e693e (005e4590).
      PC budget: 74.4 MB permanent section + 139 MB pool. Vita arena: 192 MB, of which ~72 MB are other things.
   37. The pool pays: PATCHES now replace both `mov r, 0x445c000 / je / mov r, 0x441c000` sequences (006b6ef2,
      00699a68) with a read of the slot 0x00c2b400 (zero padding after .rsrc), filled by main.cpp from the new
      setting stream_track_kb (default 20000; PC 70000); stream_subpool_kb default 24000 -> 16000. Pool ~37 MB,
      which should leave ~86 MB free before the 74.4 MB section. Tuning needs no regeneration from here on.
      Open: whether 20 MB of track sections and a 16 MB car pool are enough to show the intro.
      Result (user pressed Start Career after ~7 min in the menu at 15 fps): the section loads ("heap: large
      block of 74368548 bytes"), loading goes on for ~20 s, then `guest memory exhausted (requested 235872
      bytes)`: 888 KB free of 192 MB. Heap 153 MB (72.6 section + 36.8 pool + 32.8 + ~11), vertex/index buffer
      shadows 20.7 MB in 2004 blocks (10 MB in the menu), thread stacks 10.3 MB, chunks 8 MB. Host side then:
      user 16 MB, cdram 40 MB, phycont 26.6 MB free (phycont unused).
   38. More arena: the vertex/index GpuHeap takes its chunks from phycont memory (main memory as fallback,
      gxm_render.cpp), so s_reserveMemory 64 -> 48 MB and s_maxMemory 192 -> 208 MB (memmap.cpp). Boot log:
      212992 KB for blocks, user 31.7 MB free after it. heap::setLargeHook: every large block >= 8 MB logs its
      requester. The 33,554,596-byte block: 0051f780, size = [0xa631b8] ? 0x2000000 : 0x4000, pointer
      [0xa8c2b8], size [0xa8c2c0]: the audio pool (sound banks, ABKC at +0x4c0). The 0x2000000 "pushes" at
      005ad902 / 005e693e are flags, not sizes. The 14,278,200-byte block: bMalloc(0xd9dd94, 0x2000), freed later.
   39. PATCHES: 0051f780 reads the slot 0x00c2b404 (setting audio_pool_kb, default 32768 = PC). The exhaustion
      report logs the newlib heap use and how many pages of the audio pool were ever written. Full regeneration.
      Run 39 (pool 20 + 16 MB, arena 208 MB): the section loads, ~20 s later no frames, main thread holds the
      execution lock in 0055fbe0 (called from the car loader 007e4bb0). 0055fbe0 places resource files in game
      pools (004750f0(pool) = largest free block) and ends with `mov dword ptr [0], 0` when bytes are left over:
      the PC game's out-of-room assert. Guest address 0 is writable here, so the game runs on broken: a freeze,
      not a stop. To do: make writes to guest address 0 fatal with a log line (they mark asserts all over the exe).
   40. Car sub-pool back to 24000 KB; the watchdog logs the memory report at a stall. Start Career: exhausted
      again at 208 MB (4 MB texture lock buffer, 1.5 MB free): heap 161 MB, buffer shadows 22.8 MB, texlock
      5.6 MB. newlib heap: 19.4 MB in use of the 64 MB reserved. Audio pool: every page written (it is filled
      at creation), so that scan says nothing about its use.
   41. _newlib_heap_size_user 64 -> 36 MB, s_maxMemory 208 -> 236 MB. Start Career: 37 MB free, and a second
      request for 74,368,384 bytes (same streamer site, reached from the front-end state change 0066e300 /
      005b0120) while the first block is live.
   42. Logged: file reads >= 4 MB (kernel32.cpp ReadFile) and releases of large heap blocks. The section is read
      twice into the same buffer (6.9 s each, two file handles), no large block is released, then the third
      allocation fails. Streamer section states ([section+0xc]): 1 buffer allocated (0079ebf4 / 0079eac7),
      2 read issued (007a43f0, callback 007a2a80), 3 loaded, 4 activated (0079e670), 0 unloaded (0079e700:
      bFree of [section+0x4c]). Activation compacts: 006adbc0 allocates the compacted size with flags 0x2087,
      copies, and bFrees the 74 MB buffer. bFree returns it to the game's pool 0, which never gives a grown
      chunk back to malloc; the next load takes the same free block (same address), and after later small
      allocations have carved it up, the load after that grows the pool by another 74 MB.
   43. PATCHES retarget four calls: the oversized-section allocation 0079ebfa -> malloc thunk 006a1560; the
      section buffer frees 0079e75a, 0079a2b5, 006adc6c -> free thunk 006a1590. crt_free forwards pointers that
      are not guest heap blocks to bFree 00476d70 (crt::setForeignFree), because those sites also free pool
      blocks. Expected: "large block ... released" after each activation. Full regeneration.
      For the porting guide: a game allocator that grows by malloc and never shrinks hides freed memory from
      the port; log large blocks and their releases early.
      Run 43 (after two build slips: the compaction call is at 006adc6d, not 6c; setForeignFree lives in
      win32::msvcr71): works, "large block of 74368384 bytes ... released" after each of the first two loads.
      The third load failed with 110 MB free, largest range 65 MB: small blocks had landed in the vacated space.
   44. heap.cpp: large blocks >= 16 MB are placed at the top of the arena (MemMap::High). **The memory wall is
      passed**: three loads and releases of the section, 97 MB free afterwards, the loading screen ends (user:
      audio only, then closed). Log: 2 fps with ~70 draws/frame, then `d3d9: object 0x01801200 was released`
      and `dynamic_call: no method registered at guest address 0x00000000` (a released object's block has
      vtable 0, so a method call through it calls 0). Suspect from the Mac port (kit changes 8/9): the
      colour-grading LUT freed at race load through missing references in effect parameters / sampler slots.
   45. d3d9.cpp names the kind an object block held last and logs the destruction of the rarer kinds.
      Built and uploaded 2026-11-17 (build-release-clang/NFSCVita.self is this build); the run was started and
      sat in the menu, Start Career not pressed yet. **Next:** `tools\vita.ps1 run -Seconds 45 -Out
      work\vita_run45.log`, the user presses Start Career, then `vita.ps1 log`: read the "destroyed" and
      "was released (the block last held a ...)" lines, fix the missing reference, and look at why the world is
      dark and at ~2 fps (~70 draws/frame) after the loading screen.
      Result (log fetched 2026-11-17 as work/vita_run45b.log, Start Career had been pressed): same stop, and the
      new line reads `object 0x01801200 was released (the block last held a texture)`. So it is a plain 2D
      texture, not the volume LUT or a cube map (no "destroyed" line of a rarer kind in the log): the Mac-port
      LUT suspicion is not confirmed. The call through it comes from the main thread (esp 0x00f3e8d0), about
      10 s after the loading screen ends. **Next:** log AddRef/Release with the guest caller for that block
      (or for every texture whose count reaches 0 while it is still bound to a stage or held by an effect
      parameter in d3dx9_fx.cpp: SetTexture on an effect must take its own reference), then rerun.
      The effect side already holds its own references (FxEffect::setTexture, GetTexture AddRefs), as in
      NFSUG2_VITA, so the gap is elsewhere.
   46. Reference history for textures (d3d9.cpp, Object::noteRef): creation and the last 10 AddRef/Release
      (including binding references), each with the guest return address and Direct3D method that was running
      (g_methodCaller / g_methodSlot, set in `drained`). The "was released" line now names the current method and
      caller and prints that history. Note: D3DX imports do not set the caller, so a texture made by D3DX shows
      the last Direct3D method as its creator.
      Result: the dead object is a texture made by CreateTexture; history c(1) +GetSurfaceLevel(2) b(3) -(2)
      u(1) b(2) -(1) u(0), then a third surface Release -> crash. The "caller" words are not return addresses
      (the recompiled call does not store one at [esp]), and the slot is global, so b/u events carry the name of
      whatever drained method ran them. So the game releases once more than it acquired through us; no method
      of ours hands out a pointer without AddRef (checked: GetBackBuffer, Get/SetRenderTarget, depth, GetTexture,
      GetContainer, GetSurfaceLevel, CreateRenderTarget, effect GetTexture/GetPool).
   47. Split binding events (b/u) from AddRef/Release (+/-) in the history; result above.
   48. History also records every Direct3D method called on the texture or one of its surfaces (op m, with
      the object it ran on) and the texture's size/format/render-target flag. Not yet run through Start Career.
      **Direct boot** (from NFSUG2_VITA's boot_race): settings boot_race / boot_track / boot_opponents set the
      game's own SkipFE debug switch [0xa9e620] and its track [0xa6313c] / opponents [0xa63154]. The block of
      debug variables is serialised by 006bf080 (SkipFE 0xa9e620, track 0xa6313c = 5000, -1 -> 2000 at
      006bf060; laps 0xa63150 = 1; opponents 0xa63154 = 2; player car presets 0xa63184.. "M3GTRCAREERSTART";
      skip movies 0xa9e6d8; audio 0xa631b8) and copied into the race parameters 0xa7a1a8.. by 007a9b90.
      Run 48 with boot_race=on: no menu, the 74 MB city section loads and is released, then a steady ~6.5 fps
      at ~620 draws/frame for 70+ s without the texture crash. User: black screen, music, pause menu works, map
      slow; HUD at the start, gone after the pause menu.
   49. Start Career with the method history: the 256x128 A8R8G8B8 render target gets a second GetSurfaceLevel
      that adds no reference (op m without +): it failed with INVALIDCALL because CreateTexture forced render
      targets to 1 level, and the game released the stale pointer from the first call.
   50. **Career crash fixed**: render targets report the level count the game asked for (Texture::levelCount);
      each level below the first is a render target of its own, made on first GetSurfaceLevel and released with
      the texture ("render target 256x128 level 1 is a 128x64 target of its own"). The career ran on past the
      old stop. GetSurfaceLevel refusals are logged.
   51. **Shader method from NFSUG2_VITA**: src/platform/shadercomp.cpp (their compile mode, MIT) runs when
      ux0:data/NFSCVITA/shaders/compile.txt exists, compiles the listed Cg programs with the Vita's
      libshacccg.suprx (SDK 3.0.0) and exits; tools/shadercomp.ps1 uploads shaders/*.cg + the manifest
      (tools/shadercomp_list.cmake over cmake/shaders.cmake), starts the app, waits for compile.log and fetches
      the .gxp into shaders/gxp (-Compare: into work/gxp_compare). Recompiled UG programs differ in bytes from
      the shipped ones (other compiler build); not a problem.
      Carbon's effects carry a "shader" annotation per technique (3/2/1, 0 = fixed function; work/fx_dump.py
      lists all 30, work/sm_dis.py disassembles the blobs it writes to work/fx/). The Vita preset follows the
      game's own "lowlod" technique: IDI_WORLD_FX lowlod = vs_1_1 (pos x WorldViewProj; colour =
      lit-pow(colour, cvVertexPowerBrightness.x) * .y, clamped; uv + cvTextureOffset) + ps_2_0 (tex x colour);
      the same shaders in worldnormalmap / glassreflect lowlod; worldprelit = colour, mul_x2; worldreflect's
      road shader reduced to pow(colour, 1.3) * 2.
   52. cworld_v / cworld_f(_atest) (shaders/, compiled on the Vita): every technique of the world family
      (IDI_WORLD_FX, WORLDNORMALMAP, GLASSREFLECTSHADER, WORLDREFLECT, WORLDPRELIT) draws with them
      (d3dx9_fx.cpp FxFamily, setting carbon_shaders). Plumbing: FxEffect is an EffectPass; its values go into
      an Underground-style value block (d3dx9.cpp carbonBlock*, slots at UG parameter offsets, mirrored by
      CmdValues); kind EffectKindCarbonWorld = 0x40; blend from the device states the pass set
      (EffectBinding::deviceStates); textures from the device stages applyPass set. Run 52 (boot_race): no
      errors, race at ~1.5-3 fps. User photo: HUD (speedometer) and minimap drawn, the minimap as an unmasked
      white square (IDI_FE_MASK_FX), the 3D world black.
   53. debug_frame_dump now dumps 3 frames from frame 450 (debug_frame_dump_from) and, per effect draw, the
      effect, technique, WorldViewProj and the first vertex in clip space; every draw names the Carbon pass it is
      in (g_fxPassName). Finding: the scene is drawn into a **640x480 render texture** (the back buffer is 640x480
      too) and IDI_VISUALTREATMENT_FX puts it on the back buffer with a clip-space quad (vs_1_1 oPos = v0); that
      quad went through the fixed-function stand-in (device transforms) and landed off screen: the black world.
      The race frame: car 76 draws (IDI_CAR_FX technique 2), sky 11, FE 39, FE mask 8, screen effects 5,
      particles 6, world 6; clip coordinates of the world draws are sane.
   54. Screen family: cscreen_v / cscreen_f (clip-space position, texture shown as is) for IDI_VISUALTREATMENT_FX
      (Vita preset: no bloom, LUT or depth of field).
   55. Car family: ccar_v / ccar_f (IDI_CAR_FX technique 3 reduced: clamp(harmonic(n)) x (cvDiffuseMin + s x
      cvDiffuseRange), s from pow(n.v, cvPowers.x) and cvClampAndScales; paint = lit colour, decals where the
      diffuse map (stage 1) has alpha, x cvVinylScales.x; no specular / env map). IDI_SKY_FX joins the world
      family with alpha 0.5 (uPowerBrightness.zw). Race now ~5.6 fps (56 frames / 10 s).
      Result: in the race ~45 s, then a hang (user: "crashed"): no frames for minutes, the execution lock held by
      EA's audio thread (start 008a800c -> loop 008a7de6). Timer mode: every ~10 ms (timeGetTime) 008a7c75
      returns what is due = (timeGetTime - [obj+0x90]) * rate([[obj+4]+0xd4]) * 0.001 - mixed([obj+0x88],
      double), and the loop mixes it in 256-sample chunks without letting go of the lock. After a long stall
      (here the frame-dump frame) seconds were due; recompiled mixing is slower than real time, so it never caught
      up. Runs 53/54 already had 9 s stalls with the same holder. The user's photo of a message screen (T-Mobile
      SMS) shows the FE renders cleanly.
   56. winmm timeGetTime is paced for that thread (winmm::setPacedThreadStart(0x008a800c, 50), registered at
      CreateThread): it advances at most 50 ms per call, so a stall never becomes a mixing backlog (audio falls
      behind real time by the stall instead).
      Run 56: no "paced" line: Carbon's threads start through the EA thread trampoline 008daf40 / 008d6b40
      (entry and argument at +0x10/+0x14 of its block), and src/lib/gamethreads.cpp was still Speed.exe's table.
   57. gamethreads.cpp: Carbon's trampoline (setStartThunk 0x008DAF40, 0x10, 0x14); CreateThread reports the
      resolved entry to winmm. 
   58. FE mask family: cmask_v / cmask_f (IDI_FE_MASK_FX: rgb = tex x saturate(colour x 2), alpha = tex.a x
      mask.a (FEMaskMap, stage 1, second uv set) x colour.a). Run 58: paced clock active ("thread 9 (start
      0x008a800c) gets a paced timeGetTime"), mask active, 120 s in the race without a hang, but ~2 fps
      (~170 draws/frame): performance is the next big topic.
      Movies (NFSUG2_VITA movies.cpp: replaceMethod hooks on the VP6 codec's vtable, frames from an .mp4 beside
      each .vp6 through the Vita's AVC decoder, platform/video.cpp + mp4.cpp): Carbon has no 'MV0K'/'MV0F'
      immediates, so its codec addresses must be found another way; not started.
      User photo: the minimap is round now (mask works); HUD comes back after the pause menu; 3D still black.
   59-60. Screenshots: setting screenshot_every=N writes every Nth frame (up to 6) to ux0:data/NFSCVITA/
      shot_<frame>.bmp (gxm_device.cpp: the display callback copies the pixels, a thread of its own writes the
      file; writing from the callback crashed run 59, small system stack). work/bmp2png.py converts them.
      Setting debug_carbon_colors: world draws solid green, the screen copy a dark red floor, Carbon draws without
      depth test / culling. Run 60/61: red everywhere, no green: nothing in the shown 640x480 texture.
   62. **StretchRect** was a no-op since run 7: Carbon renders the scene into one 640x480 target and StretchRects
      it into the texture visual treatment shows. Now a GPU copy: a scene on the destination's target with one
      quad (cscreen programs) sampling the source level (d3d9_device.cpp). Run 62 (debug colours still on): the
      3D scene appears (green world, car, horizon).
   63. Real colours (work/shots63): the car (blue Viper, "CARBON" plate) behind the camera, a dark road and
      horizon, a tyre-pattern billboard, HUD and round minimap. The world is very dark (night; bloom, LUT,
      headlights and specular not drawn yet). Race ~1.5-2 fps.
      The game shows a career SMS (Free Roam) at the start of the boot race; the user presses Circle.
   64. (Timing run lost: the cfg upload did not take. Upload the cfg, then read it back over FTP before the run;
      `vita.ps1 close` first.)
   65. **Profiling**: src/platform/sampler.cpp (NFSUG2_VITA's sampling profiler, MIT; setting sampler=on, needs
      CapUnlocker; writes ux0:data/NFSCVITA/samples.bin) and tools/samples.py (names the samples with llvm-nm on
      build-release-clang/NFSCVita.elf; `python -I tools/samples.py samples.bin build-release-clang/NFSCVita.elf
      --top 40 --callers`). timing_log=on works (perf: lines every few seconds).
      **Race at 3.9 fps: frame ~260 ms = game thread ~35 ms of work + ~220 ms blocked**: "game blocked 227 ms ...
      preempt 220 | lock held: helper 221". GPU 11 ms (not the problem), 191 draws, 15.8k triangles. The helper
      holding the execution lock is almost certainly EA's audio thread 008a800c (mixes in recompiled x86 under the
      lock, preempts the game ~110x/s) -- what NFSUG2_VITA fixed with audio_mixer.cpp (EA SND mixer loop outside
      the execution lock, native x87-identical kernels, audit that retakes the lock) + audio_defer.cpp.
      Game-thread profile (70% waiting): guest 006a83a0 43% of the running samples (walks a whole chunk buffer:
      id, size, next = +size+8, counts the chunks into a 64-slot ring, then calls 0069a170 / 0046cec0 per chunk),
      FxEffect::shareParam 7% (fixed in 66), sub_69c5d0, sub_4758e0, sub_476560 (pool allocator), sub_87eac0
      (ftol), D3DXMatrixMultiply.
   66. shareParam caches each shared parameter's peers (FxParam::peers, FxEffect::s_generation). Setting
      sampler_thread=audio: the paced audio thread retargets the sampler to itself (winmm.cpp). Run 66 was started
      (boot race, timing_log, sampler, sampler_thread=audio); its results: work/vita_run66.log and
      work/samples66.bin, or fetch again: `tools\vita.ps1 log -Out work\vita_run66.log` and
      `curl.exe -o work\samples66.bin ftp://<vita-ip>:1337/ux0:/data/NFSCVITA/samples.bin`.
      **Next**: read the audio thread's profile; then take NFSUG2_VITA's approach (refs/NFSUG2_VITA/src/ug2/
      audio_mixer.cpp, audio_kernels.h, audio_defer.cpp): run Carbon's mixer outside the execution lock and/or
      native mixing kernels for the hot guest functions; or lower the audio thread's scheduler role so it stops
      preempting the game (src/lib/gamethreads.cpp setThreadRole, Carbon entries; UG's table was removed).
      Then 006a83a0 (who calls it each frame), then UG2's other speed-ups (native hot functions from the profile,
      30 fps lock, PGO profile from Vita runs).
      User's goal (2026-11-17): everything loads and renders properly and runs optimized; take whatever
      NFSUG2_VITA does that we miss. Movies via .mp4 still to do (Carbon's VP6 codec addresses unknown).
      Run 66 result (audio thread, 119 s, 70% running): **bank code interpreter 25.6% + memcmp from it 17.7%**,
      then EA mixer guest code (sub_8ad305 12%, sub_89f363 6%, sub_8ad12e 3%, sub_8d3750 3%, ...). Race 0.7-3.4 fps,
      the audio thread ("other") holding the lock 240-1300 ms per frame.
   67. bankcode.cpp: the per-instruction cache check is a masked 12-byte compare (as in NFSUG2_VITA) instead of a
      memcmp call. Result: memcmp gone, runRoutine itself 25.6 -> 45.2%: no gain, the check was not the cost.
      tools/samples.py --pcs NAME lists hot ELF addresses inside a function: spread over fetch/check/dispatch.
   68. Counters: the perf "bank code" fields are filled (runs, ms, instructions, decodes) and every 50,000 runs the
      log names the five routines with the most interpreted instructions. Race: ~1.7 M interpreted instructions/s
      in ~1,250 runs/s, 8 routines cached, decodes 0 after warm-up. One routine dominates: **0x...e8 (bank +0xe8),
      11,088 bytes, 3,203 instructions per run (straight-line), ~385 runs/s = 70% of all interpreted
      instructions**; then 1,445 / 1,346 / 352 / 335 instructions per run. Likely the engine sound synthesis.
      Fix: build-time translation as NFSUG2_VITA does (tools/recompiler/bankcode.py -> <module>_bank.cpp, run-time
      match by relocated content). Carbon's layout (Android port, refs/nfscarbon-android .../scripts/abk_module.py):
      code range header +0x84..+0x88, call relocations table at +0x30 (count, then offsets of e8 operands), each
      operand an index into the library function table 0xA7D6C0, loader 0x898A82; 285 banks.
      Hot routines by bank file: SKIDS/skid_big_mb.abk / skid_sml_mb.abk (11,088 bytes, 576 library calls),
      ENGINE/car_*_eng_mb_ee.abk (5,008) / car_*_exh_mb_ee.abk (4,424), IG_GLOBAL/roadnoise_00_mb.abk (1,516),
      IG_GLOBAL/wind_00_mb.abk (2,348).
   69. **Bank routines translated at build time** (NFSUG2_VITA's method). tools/recompiler/bankcode.py is now UG2's
      (+ Carbon's sub r32,r/m32 and group-1 /5 sub imm); generate.py BANK_LIBRARY_TABLE 0xA7D6C0, BANK_DIR SOUND,
      writes nfsc_bank.cpp in the full run, and `python tools\recompiler\generate.py --binaries "Need for Speed -
      Carbon" --out src\nfsc\generated --bank-only` rewrites only that file (seconds). Result: 116 distinct routines,
      all 389 bank entries, 102,635 instructions, none left to the interpreter; 16 MB of C++ (old UG1 translator kept
      as work/bankcode_ug1_old.py). Runtime: src/nfsc/banknative.h (UG2's, namespace nfsc); bankcode.cpp finds the
      translation by relocating its bytes for the routine's address/bank and comparing the spans on **every run**
      (no loader hook yet, so no generation counter), runs it call to call through dynamic_call, interprets
      otherwise; setting bank_native (default on); logs "runs translated (<file>:<entry>)" once per routine and
      "has no translation" with an FNV hash. The 50,000-run report now ranks by runs and shows native runs.
      Run 69 (work/vita_run69.log, samples69.bin): skid, engine, exhaust, wind entry 0 run translated; race 12-22 fps
      (run 68: 0.6-5). Still ~3,000 interpreted runs/10 s: roadnoise_00_mb.abk never native, wind native in half its
      runs.
   70. Logged where routines are entered when not at their start: roadnoise +0x1f0, wind +0x70c, swtn_car_01 +0x195,
      each exactly the start of the bank's **entry 1**. Carbon's entry ranges overlap (roadnoise: entry 0 0xe4-0x6d0,
      entry 1 0x1f0-0x86c), and findRoutine took the first range containing the address, so entry 1 calls ran as
      entry 0 entered in the middle (always interpreted).
   71. findRoutine prefers the entry that starts at the address (bankcode.cpp). **All bank code native**: 0
      interpreted instructions, ~6,500 runs per 10 s in ~8-33 ms. Race 14-25 fps, dips to 6-9 fps where "preempted"
      reaches 170-260 ms per frame (the audio thread holding the execution lock: EA mixer guest code).
      For the porting guide: bank entry ranges can overlap; match the entry start before range containment.
      **Next:** profile the audio thread again (samples71.bin; `python -I tools/samples.py work/samples71.bin
      build-release-clang/NFSCVita.elf --top 40 --callers`), then the EA mixer outside the execution lock (UG2
      audio_mixer.cpp / audio_defer.cpp), the next big lever. Later: hook the loader 0x898A82 (and the unload) for a
      generation counter as UG2 does (sub_7212e5/sub_7211ab) instead of comparing spans every run; shadow compare
      (Recording is generated already). Sound by ear with all routines native: not checked yet.
      Audio thread profile (samples71, 50k running samples in 102 s): EA mixer 0x8AD305 15.5 %, 0x89F363 9.2 %,
      bank code ~12 % (skid routine 5.3 %, runRoutine 4.1 %, LazyFlags 2.5 %), memcmp of the span check 3.9 %,
      0x8AD12E 3.8 %, 0x89F4F6 3.8 %, 0x8B70B0 3.5 %, 0x8A25B8 2.9 %.
      Audio thread structure (008a7de6): per iteration EA's SND lock [0xbbf600] (89b05a / 89b060), then per
      256-frame chunk: tick 8993fa (under its own lock 899352/899369: object updates through vtable +4 of the list
      [esi+0x54] with dt [esi+0xd0]; the command buffer [esi+0x18] posted by the game, each `call [ebx]` returning
      its size; event lists 89e6e5), buffer 8a745d, render 8a74dd (43 functions, no imports, no indirect calls), submit
      8a74ab; then Sleep. The hot functions are reached only through function pointers stored into module objects
      (code at 0089ecb6, 008adfee, 008a2131, 00540701) and through the bank library table 0xA7D6C0. Tools:
      work/nfsc_callgraph.py (static call graph below a function, recursive descent with analysis2.json bounds),
      work/nfsc_callers.py (caller chains up to pointer-only entries).
   72. Setting audio_max_rate (dsound GetCaps dwMaxSecondarySampleRate; default 48000): Carbon mixes at the reported
      maximum. 24000: output 24000 Hz, audio thread running samples 50.3k -> 33.9k (mixer functions halve; bank code
      is paced by time, unchanged at ~6,500 runs/10 s), race 23-30 fps, "preempted" 21-40 ms/frame (run 71: 30-85).
      Sound at 24 kHz not judged by ear yet; default stays 48000 until it is. (0x8A69AA's 48000 is a stream-header
      default, not the mix rate.)
   73. NATIVE_OVERRIDES (first ones): EA audio kernels in src/nfsc/audio_kernels.cpp: mix 0x8AD305 (dst += src x g),
      scale 0x8AD12E (dst = src x g), both NEON float (= their SSE path); filter 0x89F363 (y = in - x a - b buf[i+1]
      to out1, (c buf[i] + buf[i+1]) d to/onto out2, last y returned in st0), double like the x87 code. Setting
      audio_kernel_check=N compares the first N calls of each with the translated body and logs differences.
      (Build slip: new src/nfsc/*.cpp files must be listed in cmake/sources.cmake NFSU_GAME_PORT_SOURCES.)
      Run 73 (48 kHz, audio_kernel_check=200): all 200 checks identical; sound fine by ear (user). Audio thread
      samples: mix 7,800 -> 2,259, scale 1,927 -> 817, filter 4,605 -> 3,762 (a serial recurrence: little to gain);
      running 493/s -> 454/s (-8 %). Race 19-25 fps with dips to 8-12 where "preempted" is 100-210 ms/frame.
      The user saw a "crash" while driving: my script closes the app after 120 s, which looks the same (back to
      LiveArea; every close leaves a psp2core ... .tmp in ux0:data). The user's own later session (73b) ended without
      an error or a crash dump; its last ~10 s were never synced to the card.
   74. The watchdog syncs the log every 2 s (was 10), and error/fatal lines are synced at once (log.cpp).
      audio_kernel_check / audio_max_rate added to the known settings. Deployed with cfg boot_race + timing_log.
      Run 74 (user drove freely): C2-12828-1 with a core dump. work/vita_core.py / work/vita_threads.py read
      psp2dmp files (gzip ELF core; THREAD_INFO 0xC8-byte entries, THREAD_REG_INFO r0-r15 + cpsr, MODULE_INFO has the
      eboot segment base: bias = base of module_start - ELF _start, here 0x2c000; disassemble with
      .deps/vitasdk/bin/arm-vita-eabi-objdump). Main thread pc in nfsc::sub_560580 +0x1e6: `ldr [guest base +
      0xb8d52b05 + 0xc]`, i.e. guest 0x560617 `cmp [esi + 0xc], eax` walking the texture-pack pending list.
      Texture packs: container chunk 0xB3300000 (0xB3310000 header, 0xB3320000 data); handlers registered by
      0x4735D0 (load 0x560580, unload 0x560A90). Data chunks (nested: 0x5605BB, top-level: 0x55CAA0) are linked at
      chunk + 0x10 into the circular list 0xA94210 (+0 next, +4 prev, +0xC pack hash) until a header with that hash
      ([header + 0x60]) resolves them (0x560617, one per call; 0x55CA70 finds by hash). The unloader never unlinks
      pending data (0x560AD4 just returns 1 for a top-level data chunk), so unloaded packs leave dangling nodes;
      on PC freed pool memory is not reused at once, so the walk reads intact old nodes; our heap reuses it.
      Before the crash the game had reset its device (all 30 effects destroyed and recreated, threads suspended):
      a load transition.
   75-76. src/nfsc/texturepacks.cpp (replaceMethod on both handlers, which are only reached through the handler
      table): the unloader first unlinks pending nodes inside the chunk being unloaded (containers and top-level
      data); the loader first drops nodes whose chunk id (node - 0x10) is not 0xB3320000 and rebuilds the list from
      both ends (safety net, logged as an error). Run 75 (containers only): 6 safety-net repairs in a few minutes,
      the first node 0x0a5fc510. Run 76 (+ top-level data): "unloading pack data chunk 0x0a5fc500-... unlinked 1"
      (the same node), 6 unlinks in 90 s, no repairs.
      For the porting guide: game code that tolerates use-after-free on PC (pool memory stays intact after a free)
      breaks with a heap that reuses memory; a list walk into garbage is the symptom.
      Run 76 (user drove ~10 min): no crash, no safety-net repairs, 19 unlinks; but 16 freezes of 8-9 s ("no frame,
      execution lock held", the audio thread holding it at the sample), the world filling in afterwards.
   77. Audio duty limit (winmm pacedSleep/pacedWoke, kernel32 SleepEx): the paced audio thread sleeps at least
      audio_duty % (default 100) of the time it worked since its last Sleep; clock step per call 20 ms
      (audio_paced_step). The freezes stayed: not the mixer.
   78. stream_track_kb 20000 -> 50000 (cfg): **no long freezes**, the city streams in (user photo: Fortuna Heights,
      buildings, road). The 8-9 s freezes were the game waiting for track sections the 20 MB pool had not
      prefetched. Default is now 50000 (main.cpp); stream_* / audio_pool_kb registered as known settings.
      User: "audio still messed up a bit": the duty limit stretches ~4 s of sleeps per 10 s in the city, i.e.
      the audio thread wants more than half the lock there and the limit skips audio.
   79-80. audio_max_rate=24000 did not bring it under budget in the city (7-9 fps, game blocked ~60 ms/frame).
      Lock-holder slots now carry the thread id (thread.cpp): the "timer callbacks" bucket is guest thread #1 =
      the game's main thread itself (a host thread raised to time-critical; no timeSetEvent timers exist, now
      logged), ~60-68 ms/frame of its own work; the audio thread #9 holds the lock ~50-53 ms/frame. City frame
      ~128 ms = ~68 ms game + ~58 ms waiting for audio. **Next:** EA mixer outside the execution lock (UG2
      audio_mixer.cpp design; Carbon's render chunk 8a74dd has no imports and no indirect calls, the tick 8993fa
      runs object updates and the game's command queue through function pointers); then the game thread's own
      68 ms (profile with sampler=on).
      User on run 80 (24 kHz + duty limit): sound "way better", stutters only when the game stutters; one short
      freeze (no stall over 8 s in the log; section reads of 8.6 MB took 1.3-2 s, 4-12 MB/s from the card).
   81. audio_duty=0 at 24 kHz: the user judged the sound worse than run 80. Defaults now: audio_max_rate 24000,
      audio_duty 100, stream_track_kb 50000.
   82-83. **EA audio thread without the execution lock** (setting audio_unlocked, default on).
      Why not UG2's design: NFSUG2_VITA (audio_mixer.cpp) runs only UG2's render loop 0x725C8E unlocked, auditing
      every voice's module chain against a table of known render functions and retaking the lock for anything else.
      Carbon's audio profile (run 73, classified by static call graph, work/nfsc_callgraph.py): render 0x8A74DD
      ~15 %, bank code ~16 %, tick sound objects reached through function pointers ~40 %, runtime ~20 %, so a
      render-only unlock would leave most of the cost under the lock.
      Instead the whole thread runs unlocked, as it does on PC in parallel with the game:
      - its synchronisation is EA's: 0x8D8640 (the tick lock 0x899352 and the thread library's mutexes) calls the
        EnterCriticalSection / WaitForSingleObject imports; the game's allocator bMalloc 0x476B20 takes a critical
        section (0x88C858). (0x89B05A / 0x89B060 around each loop iteration only record rdtsc, they are no lock.)
      - the generator drops no lock prefix any more: the 23 lock-prefixed instructions (all in EA's thread library
        0x8DAE30-0x8DBF00: lock xadd / lock cmpxchg) and every xchg with memory run inside x86::AtomicSection, one
        global host spinlock (include/x86.h). Full regeneration.
      - WinApplication::setUnlockedThread / setGuestText: the unlocked thread's dynamic_call to anything outside
        .text 0x401000-0x9C1000 (imports, COM / Direct3D methods, foreign code = bank routines) runs under the
        execution lock (lockedCall); recompiled code calls nothing else of the runtime directly (checked: only
        dynamic_call, allowPreemption and pure rc:: / x86:: helpers).
      - src/nfsc/audio_thread.cpp: replaceMethod on the thread entry 0x008A800C (EA's trampoline 0x8DAF40 calls it
        with `call edi`), which drops the lock, empties the thread's teb::HostLock slot and runs the loop.
      - the duty limit (winmm pacedSleep) does not apply to the unlocked thread.
      Known gaps: bank routines (~16 %) still run under the lock (their caches in bankcode.cpp assume it);
      SuspendThread of the audio thread (the game suspends its threads around a device reset) takes effect only at
      the thread's next runtime call; a method-table grow while the audio thread looks up a method would race
      (only foreign-code registration grows it after start).
      Run 83 (2026-11-18): "audio: the EA audio thread (0x008a800c) runs without the execution lock" logged, no
      crash, sound "the same or a little better" (user). But no faster (city 6-9.5 fps vs run 80's 6.7-12.7): the
      thread now took the lock ~5,000 times a second for its runtime calls, and each grab preempted the game
      (preempt ~4,500/s, relock 36-60 ms per frame).
   84-85. Counted the unlocked thread's locked calls by target (perf line "unlocked thread's locked calls", target
      names logged once: "locked call target 0x... = name"). Run 85: LeaveCriticalSection / EnterCriticalSection
      ~1,300/s each, floor, timeGetTime, QueryPerformanceCounter, GetTickCount, memmove, Interlocked*, Sleep;
      bank code (0x0d......, foreign) ~1,200/s holding it ~20 ms per frame; IDirectSoundBuffer::GetCurrentPosition
      200/s, Lock/Unlock ~100/s.
   86. **Lock-free runtime calls for the unlocked thread** (WinApplication::lockedCall, list lockFreeName in
      src/lib/winapp.cpp; setting audio_lock_free, default on): critical sections (host mutexes; EnterCriticalSection
      and SleepEx release the execution lock only when the caller holds it: WinApplication::holdsContext), the
      Interlocked* imports, clock reads, Sleep, floor/ceil/memmove/memcpy run without taking the lock. The
      Interlocked* imports now run in x86::AtomicSection like the generated lock-prefixed instructions
      (InterlockedCompareExchange / ExchangeAdd were plain read-modify-writes, racy once a thread runs unlocked).
      Run 86: lock grabs ~1,700/s (from ~5,000), relock 21-27 ms per frame (from 36-60), city 10-12.5 fps (run 83:
      7-9). Left under the lock: bank code ~1,200/s (~12-17 ms per frame), GetCurrentPosition, buffer Lock/Unlock.
      **Next:** bank code unlocked (thread-safe caches in bankcode.cpp; it is ~16 % of the audio thread), then
      GetCurrentPosition / Lock / Unlock of IDirectSoundBuffer if their state allows; then the game thread's own
      ~68 ms per frame (sampler=on profile).
   87-92. **Graphics: the grey "tyre tread" block and particles** (2026-11-18; the user: sound good, asked for
      graphics before bank code). Screenshots (screenshot_every, now also screenshot_from / screenshot_count) showed
      a tread-textured block fixed mid-screen and the moon as a 2x2 atlas of phases.
      - New debug setting debug_skip_cycle=a,b,... (+ debug_skip_from): every 150 frames skips the next group of
        draws (an effect name, "fixed" = draws without a family program into a texture target, "fixedbb" = to the
        back buffer), timed so screenshot_every=150 shows each group missing. Config values may now be 255 chars
        (MaxValue was 64: run 89's list was dropped silently).
      - Run 90: skipping IDI_SKY_FX removes the moon; skipping "fixed" removes the block.
      - The block was IDI_CARNORMALMAP_FX (the tyres) through the fixed-function stand-in: model-space positions
        land as clip coordinates. Now in the car family (same inputs as IDI_CAR_FX, normal map unused). The car
        family also read the diffuse map from stage 1 always; IDI_CAR_FX technique 2 (the one used) has
        DIFFUSEMAP_SAMPLER on s0 (stage 1 holds the environment map): the stage now comes from the pass's sampler
        table (FxEffect::m_diffuseStage -> EffectDraw::pass).
      - IDI_PARTICLES_FX gets its own programs (shaders/cparticle_v.cg / cparticle_f.cg, setting carbon_particles):
        vs_1_1 billboards (centre + corner offsets aTex1.xy along cmWorldView's right/up, rotated about the view
        axis by aTex1.z; particles keep the corner within 0.65 NDC of the centre, colour x2, ambient_colour;
        flares: no limit, colour x1). Per-light shading and the flares' soft depth fade are left out;
        streak_flares (previous-frame matrix) still use the stand-in.
      Run 92: block gone, tyres on the car, tyre smoke renders, street lights glow; fps unchanged (11-12 without
      timing_log). The moon looks like one moon in run 92's shot (not checked closely).
      **Next (graphics):** IDI_SKY_FX has its own vs_3_0/ps_3_0 (procedural clouds scrolled by cfTimeTicker,
      MISCMAP1) and still draws with cworld; the user reports the sky moving too fast when driving. Then
      streak_flares, IDI_WATERSHADER_FX, screen effects (IDI_SCREENEFFECT_FX downscale/blur still run through the
      stand-in for nothing: skip them for speed).
   PC reference capture (2026-11-18): apitrace 14.0 win32 (.deps/apitrace, unpacked with py7zr; RenderDoc has no
      D3D9) traced NFSC.exe on this PC through the menus into the career prologue race (night canyon, opponent
      car): work/pctrace/nfsc.trace, 6.3 GB, 35,440 frames (race ~34,000-35,300). Tools: `apitrace dump --grep=` /
      `--calls=a-b` (a scan of the whole trace takes ~10 min); work/pctrace/frame34801.txt is one race frame;
      work/pctrace/shadermap.txt maps the trace's shader handles to work/fx/<effect>_t<T>p<P> by bytecode (all 194
      matched). Frame 34801: 484 draws; reflection pass, then the scene with sky t0 (vs_3_0/ps_3_0, the procedural
      one), world t3/t1, worldnormalmap t3, worldreflect t6/t2, car t3/t1, carnormalmap t1, worldprelit t0,
      particles t2/t4/t6, then screeneffect downscale/blur (t1, t2, t5) and visualtreatment t0 to the back buffer.
      Sky PS constants there: c0 fog colour (0.216 0.31 0.341 1), c1 (2 0.6 1 0.4), c2 eye, c3.x time 0.215.
   93. **Sky, technique 0** (the one the PC uses per the capture): shaders/csky_v.cg / csky_f.cg translate
      sky_t0p0 vs_3_0/ps_3_0 instruction by instruction (height gradient, moon from MISCMAP1 on a billboard around
      the fixed point (-50000, 60000, 30000) with glow, clouds from DIFFUSEMAP at uv*2.77 scrolled by
      cfTimeTicker*(-0.14,-0.28), horizon haze, fog to cvFogColour, * cvVertexPowerBrightness.y/2, alpha 1).
      FxEffect::m_skyProcedural -> EffectKindCarbonSky (clouds stage 0 wrap, moon stage 1 clamp); technique 1 stays
      in the world family; setting carbon_sky. Run 93: cloudy night sky in every shot, no stray textures; the moon
      was not in view (not checked yet).
   94-95. **Car paint** (user photo: blue/black swirl over the Viper). The car family read IDI_CAR_FX technique 2's
      DIFFUSEMAP from stage 0 since run 92, but the car's skin was garbage: D3DXLoadSurfaceFromSurface was a stub.
      Carbon paints the vinyl skin on the CPU into a 1024x1024 A8R8G8B8 texture and copies it so into the car's DXT1
      skin (Mac port notes: 007d7580 -> 00732ca0). Now implemented (d3dx9_30.cpp): whole surfaces of texture levels,
      decode (A8R8G8B8, X8R8G8B8, A8, L8, DXT1/3/5) -> nearest scale -> colour key -> encode (A8R8G8B8, X8R8G8B8,
      DXT1 with 3-colour blocks where texels are transparent). Run 95: "1024x1024 0x15 -> 1024x1024 DXT1 (58968
      blocks with transparent texels)", the blue Viper with white stripes as on PC. Also ccar now follows technique
      2's decal colour: texture * lerp(oD1.w, lit colour, cvVinylScales.x) (oD1.w = luminance of the harmonic light
      * (s * cvFlakes.w + cvFlakes.z); slot CarbonTextureOffset holds cvFlakes for the car family).
   96. **Effect emulation cost** (run 94 profile, game thread: applyPass 5.9 %, shareParam 2.5 %, floatsOf 2.1 %,
      memcmp 2.8 %, and ~11 % in a libc stub (named sceNetSocketClose by the symbolizer) called from them).
      Shared parameters now live in one SharedSlot per (pool, name) that every effect of the pool reads and writes
      (FxParam::v() / tex()); shareParam copied each set into ~28 effects. floatsOf caches literal names
      (literalIndex), namedParam and the pass sampler table cache parameter indices.
      Run 96 vs 94 (both timing_log + sampler): **10-16 fps vs 7-9**, frame 63-100 ms vs 111-150; game thread holds
      the lock 45 ms per frame (was 67-75), audio thread 18, relock 21. Remaining top: yieldContext 7.2 % (spins
      while handing the lock over), the libc stub 6 %, applyPass 3.6 %, setMatrices + D3DXMatrixMultiply 4.6 %.
      **Next (performance):** yieldContext spin; CommitChanges re-applying only parameter-dependent states (as
      D3DX does); audio bank code without the lock (18 ms + hand-overs).
   97-100. **Graphics preset** (clue from the Android and NFSUG2_VITA ports, docs/REFERENCE-CLUES.md). Lowering
      the g_* values in registry.txt did nothing: Carbon's detection 0071d0a0 runs at every start (00729d56) and
      applies a Level-of-Detail preset over them, chosen from the adapter's device id (High list 0x9e8078, Medium
      list 0x9e8000, else Low), then writes them back (RegSetValueExA from 00711bd0; now logged, "RegSetValueExA:
      g_... = N"). We reported a Radeon 9700 (Medium list). New setting gfx_detail (d3d9.cpp GetAdapterIdentifier):
      low (default, Radeon 9200 id 0x5961), medium (0x4E44), high (0x7100). Low = road reflections 0, rain 0, world
      LOD 0, car environment map 1, shader detail 1, particles and visual treatment on.
      Run 100 (timing_log + sampler, as 96): **12-19 fps vs 10-16**, triangles 16-26k vs 28-38k, GPU 22-26 ms vs
      28-31; looks fine in the screenshots (car, stripes, lights, smoke).
   101-125 (2026-11-18 night). **Unattended test runs**:
      - auto_input (src/lib/controls.cpp): a timed script of pad inputs added to the real pad, e.g.
        "30-90:circle:tap;95-400:triangle;100-400:sleft:weave" (segments FROM-TO:inputs[:hold|tap|weave]; sticks
        sleft/sright/sup/sdown, rleft/rright/rup/rdown). In Carbon's layout for our pad: **Triangle = throttle**
        (run 125: 38-84 mph), Circle taps close the career SMS, R = pause (run 103), Cross / Square / sticks do not
        drive. work/sheet.py makes a contact sheet of the screenshots (no PIL).
      - **Freeze ~20 s after start in file I/O** (runs 106-123, any cfg, survived a Vita restart): the Vita went into
        its idle/power-save state once nobody pressed buttons (auto_input is inside the app, the system does not see
        it). Fixed: the stall watchdog calls sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT) every 2 s (run 124+).
        Big reads are now named in the log before they start ("file X: reading N bytes at OFFSET").
      - vitacompanion's port 1338 takes "reboot" (FTP back after ~15-20 s); the log's first line now says how long
        the Vita has been up.
      - IDirectSoundBuffer GetCurrentPosition / Lock / Unlock run without the execution lock for the unlocked audio
        thread (AudioBuffer takes the device mutex; the Mac port does the same); not measured yet.
   126-131. **Missing cars**: not memory (new check: the watchdog logs nonzero words in guest page 0, where
      bMalloc's out-of-memory path writes; none in run 126). The direct boot's SkipFE setup 0075e446 sets the
      traffic density (attribute 0xc64bc341, 0..100) from [0xa9e640] * 100 unless [0xa9e644] is set; both are
      zero-initialised, so boot_race had **no traffic**. New setting boot_traffic (0..1, default 0.3) writes
      [0xa9e640]. Run 131 (boot_traffic=1.0): a rival crew car (green) drives up, "RIVAL CREW CHALLENGE!". Ordinary
      traffic not confirmed from the night screenshots yet (the user will see it). The normal start through the
      menus needs Start (title), then Circle = accept; "Start Career" asks for a new alias (default No) - loading the
      existing save "aname" needs a d-pad move to LOAD first (not scripted yet).
   132-133 (2026-11-18 morning). The Vita answered again; the first launch left no log ("game did not start"),
      a "reboot" on port 1338 fixed it. Run 132 = driving baseline of build 129 (work/nfscvita132.cfg: boot_race,
      timing_log, sampler, auto_input driving, default boot_traffic 0.3): 24-29 fps before driving, **7-18 fps
      (mean ~12) while driving**, preempt 1300-2600/s, game thread preempted 25-110 ms per frame; profile:
      libc stub ("sceNetSocketClose") 6.9 %, yieldContext 5.8 %, applyPass 3.9 %, guest 0071b630 3.0 %.
      Run 133: yieldContext's hand-over spin now has its own YieldSpinUs = 100 (was PreemptSpinUs = 500; the
      acquire spin stays 500): **30 fps before driving, 12-25 fps (mean ~17) while driving**, yieldContext 3.9 %,
      audio thread's lock wait 2.9 ms vs 5.7, timer callbacks' lock grabs 1295/s vs 1873/s. Sound not judged yet
      (the user should listen for crackle).
   134-137. **No traffic, again** (user, build 133: only the opponents). New setting debug_watch (winapp.cpp
      watchdog: guest pointer chains like "a98284>bc9c,a61290:b", logged every 10 s when they change). Run 135: the
      race status [0xa98284] exists but its race parameters [+0x6a1c] stay null in a SkipFE boot and the race's
      density [+0xbc9c] stays 0. The spawner's density 00422cc0 has its own SkipFE branch: clamp([0xa9e640] * 0.01,
      0, 1) (0x9c2938 = 0.01; normal races: type 1 -> [+0xbc9c] * 0.01, free roam -> 1.0; * 0.75 when [0xa83e9c]).
      So [0xa9e640] is on a 0..100 scale and runs 126-133 gave density 0.003. main.cpp now writes boot_traffic * 100.
      Run 136 (auto_input): 20-24 fps before driving (was 30) and 6-12 while driving (was 12-25) at the same draw
      counts: more simulation, so traffic most likely spawns; the two screenshots show the car stuck at a wall.
      The boot "race" is free roam (minimap, heat meter), not a race. Run 137: same build without auto_input for
      the user to look. Also: the driving fps comparison of 132/133 had no traffic; with traffic the baseline is lower.
   138-141. User: still no traffic with the *100 fix. Watches: [0xa9e640] = 30.0; the other early-outs of 00422cc0
      are clear ([0xa83a4c] 0, [0xb4d964] 0); [0xa9e874] (object made at 0044ae56, vtable 0x9c3b30) has vfunc +0x18
      = 0042d840, which returns true only with entries in its lists +0x14 / +0xf4 or [+0x64] != 0: all empty. So the
      density getter should give 0.3. Its only caller is the spawner 00445080 (a vtable callback via 00448b20):
      density -> 0042ce70(&x) must succeed -> accumulators [this+0x130/0x134] -> 00444d90 / 0042c950 spawn. Not
      traced further yet. Run 141: normal start through the menus (no boot_race). **User: traffic is there** (a
      little, not always; they had missed it before). So the *100 fix is right and traffic spawns; boot_traffic
      0.3 = 30 % of free roam's density. Traffic costs frame rate (run 136 vs 133: 20-24 vs 30 fps standing,
      6-12 vs 12-25 driving): performance baselines from now on are with traffic. debug_watch stays for such checks.
   142-150. **Speed work with traffic; a repeatable benchmark.**
      - The profile's "sceNetSocketClose" is not ours: samples.py prints addresses minus the load bias, so its
        0xdff9..-0xdffb.. are really 0xe0009800.. = SceLibKernel (new: the sampler logs every module's segments at
        start). About 2/3 of it is the body of sceClibMemcpy (stub -> 0xe00097d1), the rest the kernel mutex calls.
      - Clue from NFSUG2_VITA (include/platform/smallcopy.h, src/platform/memfuncs.cpp): sceClibMemcpy costs ~190 ns
        before copying. Ported: NEON copies/fills up to 1 KB, memcpy/memmove/memset linked with --wrap
        (CMakeLists.txt). The system share fell only 6.1 % -> 5.6 %: the rest is big copies.
      - Copy profile (cmake -DNFSU_COPY_PROFILE=ON + sampler_copies=on; turn it off again, it costs ~8 %): the top
        copier by far is d3d9::Buffer::Unlock's locked-range copy into GPU storage, ~52 KB per call, ~100/s
        (509 MB in 100 s), i.e. the dynamic vertex data the game writes; then floatsOf / applyPass /
        carbonBlockStore (1.4 M, 1 M, 0.6 M small copies, now NEON). UG2 has the same Unlock (plus a frame-memory
        path for small DISCARD locks of busy buffers, which would save our whole-buffer copies: 930 x 22 KB).
      - **ai_drive=on** (clue: the Mac port's auto_drive, Game_ForceAIControl 0065c2c0(1) while [0xa99bbc] == 6):
        the game's AI drives the player's car (run 146: 146 mph in 5th through the city, no input). Called from a
        new per-frame hook (d3d9::bridge::setPresentHook). Benchmark cfg: boot_race=on, ai_drive=on,
        timing_log=on, auto_input=30-90:circle:tap; 170 s run; **tools/perfsum.py LOG** gives the mean fps of
        the drive. Repeatable: runs 148 / 150 = 11.7 / 11.7 fps (mean frame 85.6 ms).
      - YieldSpinUs 30 instead of 100: 11.3 fps (run 149), no gain; stays 100.
   151-156. **Effect path and hot x87 leaves.** (The route still varies a little between runs, e.g. run 152's AI hit
      a wall: compare two runs, or the sampler shares.)
      - Clues: NFSUG2_VITA has no effect framework (fixed-function), but its src/ug2/native.cpp replaces _ftol2 and
        its hot-function conversions keep the guest frame; the Mac/Android ports have nothing on these addresses.
      - D3DXMatrixMultiply: NEON (the scalar VFP code was full of core<->VFP moves), straight from/to guest memory.
        2.6 % -> 0.8 % of the game thread.
      - setMatrices: float4x4 parameters copy 64 bytes instead of 16 putFloat calls. 2.2 % -> 0.7 %.
      - CommitChanges (called per draw) no longer re-applies the pass's constant render/stage/sampler states, as
        D3DX: only the sampler textures and the family constants (setting fx_commit_light=off restores the old
        path). applyPass + applyState 5.0 % -> 2.4 %. applyFamily: the effect-name compares and the carbon_sky
        config read are cached. param(): name handles cached by guest address.
      - Benchmark: runs 154 / 155 = 12.7 / 12.3 fps (was 11.7 / 11.7). Screens look the same (run 152 shot).
      - Native overrides (src/nfsc/native_math.cpp, generate.py NATIVE_OVERRIDES; needs -Regenerate): _ftol2
        0087eac0 (1.4 %), frustum box test 0071b630 (1.9 %), box transform 0070f0b0 (0.7 %), square overlap
        0046fcc0 (0.7 %). Written to be bit-identical to the translated x87 (double, float rounding at each
        fstp dword); setting native_check=N compares the first N calls of each and logs "native ... differs".
      - Career crash (user, 2026-11-18; work/vita_career_crash1.log): after the intro race and the car choice,
        "guest memory exhausted": the city's permanent section (74368384 bytes, streaml5ra.bun) is released and
        requested again, and 79 MB are free but the largest range is 72324 KB (300 KB short) -- the run 43 problem
        again: smaller blocks landed in the vacated range (0x035ae000; the top of the arena holds the stream pool
        and the audio pool). Stopgap: cfg stream_track_kb=46000 -- confirmed by the user (2026-11-18 afternoon):
        the career now loads past the car choice into the next race. Fix for the career work: keep a range for this
        section (park the released block for the next request of that size, or keep small blocks out of it).
   157-165. **Native overrides checked; a stack bug in them; descriptor limit; LiveArea art.** (2026-11-18)
      - Run 157 (build 156, native_check=200): _ftol2 identical, 12.4 fps. The check budget was shared and _ftol2
        used it up: each override now has its own (run 158: overlap / frustum differ only in edx, which every
        caller overwrites or drops: work/edx_after.py over all call sites, REGS=ecx,... for other registers).
      - Runs 158-161 crashed ~70 s into the drive, four times the same dump: stream library 0x8d14a0 (EA, 8 slots,
        called by the game's stream manager 0x773829 with slot 2) passed edi = 0x0eda1d60 (a heap pointer) to
        0x8d250f, which indexed its slot table with it. The guest stack below esp still held the callee's saved
        edi = 2 next to 0x0eda1d60: a callee returned with esp 4 bytes off. Cause: address-taken guest functions
        write cpu.esp on return (callers through dynamic_call take esp from it), the native overrides did not.
        Fix: native_math.cpp popReturn (esp += 4, frustum test 0x71b630 += 16 for ret 0xc) and the same step in
        audio_kernels.cpp; native_check compares esp for _ftol2. native_math=off runs the translated bodies
        (run 162: clean). Runs 163-165: no crash; benchmark 12.8 / 12.9 fps (baseline 12.5).
        For the porting guide: a native override must leave cpu.esp exactly as the guest ret does.
      - Descriptor limit: the game keeps every bundle, bank and traffic car file open (60+ guest files while
        driving); the Vita process has ~64 descriptors and open() failed with errno 24 (runs 157, 160; engine
        .abk/.gin of traffic cars not loaded). src/lib/file.cpp now parks the least recently used idle read-only
        file (closes its descriptor, opens it again at its position on next use); the watchdog line counts parked
        files (run 162: 66 guest files, 6 parked, no failures).
      - LiveArea: tools/vita/livearea_art.py (from NFSUG2_VITA's, Carbon TPK headers: 0x58-byte header + length-
        prefixed name, 24-byte format entries) makes icon0/startup/bg from NFS07_LOGO (GLOBAL/UniqueBootTextures.bin,
        A8R8G8B8) and SPLASH_SCREEN_WS_3 (FRONTEND/DEMOFRONTB1.BUN) into sce_sys/game/ (ignored, EA art); CMake
        uses it with an a1 gate template (template_gate.xml.in); build.ps1 makes it when missing.
      - Profile run 166 (sampler): yieldContext 7 % (lock handovers), applyPass 2.4 %, 71e660 / 727230 / 79fa60
        ~1.3 % each. The unlocked audio thread took the execution lock ~1,800 times/s for bank routines
        (~30 ms/s), and the game thread lost 50-70 ms per frame to preemption.
      - Bank code without the lock (setting bank_unlocked, default on): WinApplication::setLockFreeMethod lets the
        unlocked thread run runRoutine without the lock; bankcode.cpp guards its cache and counters with its own
        mutex (never held while guest code runs), routines are pinned while running, the resolve handoff is
        thread_local, and interpreted runs take the lock (takeContext). Runs 167 / 168: **16.6 / 16.4 fps**
        (was 12.8); game thread preempted 2-11 ms per frame instead of 50-70; audio levels and bank run rate
        unchanged. Sound not yet checked by ear.
      - Startup gate: logo only, transparent (user's request).
      - Controls: NFSUG2_VITA's layout (user's request). src/nfsc/pad.cpp writes it into Carbon's bindings table
        (0xa98700, rows and format from the Mac port's mods/core/nfsc/pad.h) whenever a port's rows 0/1/4/5/6 are
        the game's own defaults (remembered at the first frame with the device; game defaults: gas button 0 =
        Triangle, nitrous button 6 = L), so Controls-screen remaps are kept. R gas, L brake, left stick steer,
        Cross handbrake, Circle nitrous, Triangle / Square and rear upper corners shift, rear lower left / right look
        back / camera, Select reset, Start pause, d-pad down Speedbreaker, d-pad left crew, d-pad up engage event;
        menus d-pad / stick, Cross accept, Circle back. Rear touch is now four quadrants (buttons 4 5 10 11) as UG2.
        Setting ug2_layout (default on). Run 169: applied (3x during boot: the profile load resets the rows).
        User test: fine, but d-pad left/right steered: the d-pad also posted the Left / Right arrow keys (meant
        for text fields), which Carbon's keyboard columns bind to steering; gamepad.cpp now posts them only while
        a text field is active. Rear-touch shifting removed (user: Triangle / Square are enough; a profile saved
        with it loses exactly those two bindings). Run 170: layout applied.
        auto_input scripts written for the old layout (Triangle throttle) no longer drive.
   171-195 (2026-11-18 evening). **Port review, front-end mapping, save loading fixed.**
      - Work plan from comparing the other ports: Claude Docs "NFSCVita work plan" (performance, movies, career,
        graphics, comfort, release). Run 171 = AI-drive benchmark of build 170: **17.3 fps** (was 16.5).
      - Front end, timed with the new screenshot log line ("gxm: screenshot ... at N s"): the title ("CLICK to
        continue") is ready 57-88 s after start (varies a lot: time-based auto_input scripts are fragile). Start ->
        profile auto-load (one profile) or the ALIAS list (several) -> Cross -> main menu with "Cannot connect to EA
        Nation! OK" -> Cross -> Career (START CAREER / LOAD / SAVE). Start Career with a profile asks "You must
        create a new alias" (default No). work/menurun.sh N "AUTO_INPUT" FROM EVERY COUNT SECONDS does one run
        (cfg, run, log, screenshots, contact sheet); work/shots.sh clear|fetch.
      - **The career save never loads.** At Start the game shows "Unable to load aname." The file is there
        (save/nfs carbon/aname/aname, 286,764 bytes) but the load opens `C:\save\NFS Carbon/Aaname/Aaname`.
        Saving is right: Career -> SAVE wrote aname/aname (run 189). New CreateFileA failure line (path, caller)
        and setting debug_find=TEXT (CRT strncpy/strncat/_mbscpy/_mbscat/sprintf family/wcscpy/vswprintf log when
        their output contains TEXT, with the arguments) show: the profile probe reads each save with the right
        path (strncpy from the static 0xbbc3d4, sprintf "%s/%s/%s" at 0x894c26); after the choice, guest code
        builds "A" + name in a fresh heap block (0x03120110; the same object holds the plain name at +0x48), and
        EA's memory-card request constructor 0x89624f (called via 0x896303 / 0x896363 from 0x896f46 / 0x896fa7)
        copies it (strncpy n=0x40) into the request that 0x894bd1 / 0x894c26 turn into the path. Not a CRT shim:
        the "A" is written by translated code. Selecting "aaname" opens "Aaaname" (so: "A" + name, or the first
        letter upper-cased and kept). With a copy at aaname/aaname the profile loads (SAVE appears in Career).
        Also: of several profile folders the second one in readdir order is never listed (aname + zname -> only
        aname; aname + bname + aaname -> aname, aaname). Windows lists "." and ".." first; emulating them (setting
        find_dots, default off) changed neither problem. debug_scan=TEXT (watchdog search of guest memory every
        10 s) works but slows the game. Not found yet: which function writes the "A" (no `mov byte [..], 0x41`
        in the exe, no lone "A" string used there): next, find the constructor of the object at 0x03120110 (it is
        allocated with the game's 0xee fill) or compare with a PC run's file accesses.
        **Cause and fix (runs 194-195, user's hint "A = slot A?").** The game lists profiles with
        "C:\save\NFS Carbon/A*" and names each profile folder and file "A" + alias: "A" is the English string
        0x193548ec (type 0, the save; type 1 is "SHADOW", the backup; memcard class vtable 0x9e8ca8, +0x68 returns
        them). The MemcardEntry builder 0x58e480 strips the prefix only when 0x580940 finds it with a case-sensitive
        compare. Our file layer lower-cased every guest path, so the game's folder "Aname" (alias "name") was stored
        as "aname": on the next start "aname" did not match "A", nothing was stripped, the alias became "aname" and
        the load asked for "Aaname". The skipped profiles are the same thing: "bname" and "zname" do not match "A*".
        Fix: src/lib/file.cpp keeps the game's case for paths under C:\save (the memory card is case-insensitive,
        so lookups still match). Run 195: the save placed at Aname/Aname loads at start ("Load complete."). Saves
        made by older builds sit in lower-case folders: rename "aname" to "Aname" (folder and file) to keep them.
        The user's original save is backed up in work/savebackup/aname (restored on the Vita after run 189's
        test save; the re-saved file is work/savebackup/aname_resaved_run189, put back as Aname/Aname in run 195).
        Front end: Cross on the title works from ~30-35 s after start (user).
   196-215 (2026-11-18 night, user away). **Performance: reflection pass, thread priorities, the SND lock.**
      Benchmark noise on this route is about +-1 fps: compare pairs (work/bench.sh N [cfg lines]).
      - Frame dump of a race frame (run 196): one 256x256 pass (49-87 draws, IDI_WORLD*_FX "lowlod": the car
        environment map), the scene at 640x480 (~500 draws, 200 of them IDI_WORLDNORMALMAP_FX), a 160x120
        downscale (2 draws: skipping screen effects would save nothing) and the HUD (50-100 draws). The game is
        CPU-bound (GPU ~25 ms of a ~57 ms frame), so render-target sizes do not matter; draw counts do.
      - Settings table (00711c60): g_CarEnvironmentMapEnable [0xa65340], g_RoadReflectionEnable a65358,
        g_ParticleSystemEnable a6536c, g_WorldLodLevel a65370, g_CarLodLevel a65378, g_RainEnable a65380,
        g_VisualTreatment a65394, g_ShadowDetail a65398, g_ShaderDetailLevel a63e60. 0072ecc8 checks a65340 every
        frame before the environment map pass. New dev setting debug_poke=ADDR=VAL,... (dwords at every Present).
      - **car_reflections** (default off): writes 0 to a65340 at every Present. 17.1 / 17.2 fps vs 16.3 / 14.4
        (runs 198, 199, 202, 203); the pass is gone in the dump (run 201). Particles off: no clear gain (run 205).
      - Build 197 failed in vita-elf-create (heap corruption 0xc0000374, deterministic, also with -v): the link
        lacked NFSUG2_VITA's -Wl,--defsym=__sce_headroom=0x2000 (8 KB after the code segment for the SCE data;
        our linker script already reads __sce_headroom). Added in cmake/vita-clang.cmake.
      - Lock hand-overs: "preempted" (~9 ms per frame) is yieldContext handing the execution lock to a waiting
        thread of a higher level. New timing_log line "yield: by waiting level": 200-900 yields per 10 s to
        THREAD_PRIORITY_HIGHEST threads, 0.7-2.9 s of every 10 s. debug_no_yield=on (never yield) hangs the boot
        (run 206). SetThreadPriority / CreateThread are now logged: priority 2 = threads at 0094c740 (DirtySock idle),
        009763e0 (network), 0088c86e and two through EA's trampoline 008d6b40; the RWAudioCore Dac thread is 15.
        **thread_normal** (default "94c740,9763e0,88c86e,8d6b40", empty = the game's priorities) keeps those at
        normal priority: 18.0 / 18.1 fps vs 16.8 / 16.9 (runs 211-213), preempted 0; network threads alone: 16.9
        (run 210). Streaming looks normal in run 212's screenshots. Sound not judged by ear yet (user).
        Run 214 (new defaults): **18.3 fps**.
      - Critical sections: new timing_log line "critsec: contended (10 s)" (guest address, waits, total ms, and since
        build 215 the owners: CRITICAL_SECTION RecursionCount / OwningThread are kept as Windows keeps them). One
        section takes it all: 0x00bc9a50, EA's sound (SND) library lock (wrappers 0x8cf74b lock / 0x8cf75d unlock),
        ~400 contended waits per 10 s, 1.2-1.6 s of every 10 s (12-15 % of the game thread), ~3 ms each, while
        the audio thread itself uses ~1 ms of CPU per frame: its holder seems to stall while holding it.
      - User (2026-11-18 night): the texture flicker is gone "since many runs" and the HUD now stays after the pause
        menu: both open items resolved (not tied to one change). New report: the car's lighting from the
        surroundings looks off sometimes; being checked against car_reflections=off.
   216-222 (2026-11-18 late night). **Car lighting, the audio thread, where 30 fps has to come from.**
      - User: car lighting "looks off sometimes", only on tonight's builds; sound good; no texture flicker; HUD
        stays after pause. Runs 216 / 217 (screenshots): with car_reflections off the cars are lit bright and flat
        in dark streets: the environment pass also feeds their light from the surroundings. Since thread_normal it
        costs nothing measurable (on 18.5, off 17.7 fps), so **car_reflections now defaults to on**.
      - Sound lock 0x00bc9a50: owners while the game waits are thread 0x9 "RWAudioCore Dac" (EA's audio thread,
        entry 008a800c, on helper core 3 at Windows priority 15) or already released (0x0). Its profile
        (sampler_thread=audio, run 218): running ~63 % of the time, sleeping 30-35 %; 8ab884 (x87 sample decoder,
        ~630 bytes) 9-10 %, bank routine a7eb6b92 7 %, runRoutine 6.5 %, memcmp 5.9 %, 8ad305 (native) 6 %,
        8abbb2 5 %, 8d3750 4.7 %, filterNative 3 %, LazyFlags::materialize 3 %.
      - The memcmp was bankcode.cpp comparing every native bank routine's bytes on every run (11 KB skid routine,
        ~385 runs/s). Now the first and last 32 bytes of its first / last span per run, the full compare every
        250 ms per routine (and on a quick mismatch). Run 219 had a size check that sent every run to the search
        (2.5 fps, 27,834 log lines): fixed in build 221. Run 221: 18.3 fps; memcmp gone from the audio profile
        (run 222), but the SND lock waits stay ~1.1 s per 10 s: they follow the total mixing time.
      - ffmpeg 9.0.2 essentials (gyan.dev, SHA-256 checked) in .deps/ffmpeg for the H.264 movie path (user OK).
      - Where 30 fps has to come from (18.3 now, about +65 % needed): the game thread holds the lock ~44 ms per
        frame, 65 % of it translated game code with a flat profile (no function above 2 %); generated code is
        already -O2 + ThinLTO. Candidates, biggest first: (1) a small-object scenery cull like NFSUG2_VITA's
        world_min_pixels / world_lod_pixels (their native sub_5d6570); Carbon's per-model render path is 00729320
        (mesh loop, per-mesh frustum test 0071b630 at 00729478, whole-model test 0071ef30 when [0xab0a34], box
        transform 0070f0b0, bounds at model+0x20..0x38): needs the bounds layout, camera position and FOV;
        (2) native versions of the audio DSP loops 8ab884 / 8abbb2 / 8d3750 (shorter SND lock holds, ~5 %);
        (3) PGO; (4) more native hot guest functions (each ~1-2 %).
   223-228 (2026-11-19 night). **Scenery size scale; traffic and audio-rate costs.**
      - Carbon already sizes scenery like NFSUG2_VITA's culling: the scenery renderer 0079fa60 (called from 0079fd96 /
        0079fe32 / 0079fee5) gets an on-screen size from 0079f9c0 (radius / (distance - radius) * info+0x40, the
        view's pixel scale; 0 behind the view) and skips the instance when it is <= 1, draws its low-detail model
        (instance +0x34) below 32. 0079f9c0 is now a native override (generate.py NATIVE_OVERRIDES, regenerated):
        identical to the guest in 300 native_check calls (run 223, 18.6 fps). **scenery_scale** (default 0.5, 1 =
        the game) multiplies the size before the truncation: 0.5 19.9 fps / 603 draws (run 224), 0.35 19.5 (run
        225); no gaps in the screenshots. Run 226 (new default): 19.2.
      - Traffic: boot_traffic=0 22.4 fps vs 19.2 at 0.3 (runs 226 / 227): ~15 % (the game's own Traffic option).
      - audio_max_rate=16000: 21.0 fps but a different stretch of route (523 draws); SND lock waits barely lower
        (run 228). Not taken: the user chose 24 kHz by ear.
      - Not done: PGO (Carbon's main never starts pf::pgo; full instrumentation of the generated code may need tens
        of MB the Vita lacks: try -PgoList with the hot buckets only); the sim runs fixed 1/60 s steps (0x9ebb6c,
        0x9ec0a8, accumulator 0xb4d7ec, up to 8 per frame): shared read-only constants, left alone; movies: Carbon's
        player is a different EA library version from UG2's (header "MVhd" checked at 0x888884, no MV0K/MV0F tags):
        needs its own reverse engineering. The user offers PC runs with logging when that helps.
   229-236 (2026-11-19). **The SND lock: who holds it, and the audio thread's core.** (User chose: audio lock first,
      then native hot functions, then PGO.)
      - timing_log's critsec line now also reports hold times per owner (count, total/max ms, holds >= 2 ms; entry
        time kept in the guest CRITICAL_SECTION's SpinCount +0x14). Section 0x00bc9a50: the EA audio thread (0x9)
        holds it ~3,750 times / 10 s, **3.1-3.5 s of every 10 s**, ~94 holds/s of >= 2 ms (one per 256-frame chunk at
        24 kHz); the game thread ~6,000 holds totalling ~30 ms. The long hold is the per-chunk tick 0x8993fa: its
        lock 0x899352 goes through [obj+0x38] = 0x0089cb10 -> 0x8cf74b (set by 0x89cb1a), around the module updates
        (vtable +4 of the list [esi+0x54]), the event lists and the game's command buffer. The render 0x8a74dd runs
        outside it (0x8a70e2 / 0x8a70ea around the loop are rdtsc counters, not locks). The game's own lock users
        (~20 wrappers 0x8cfa6c..0x8cff07: lock, call an SND API function, unlock) change arbitrary SND state, so
        handing the lock to the game between modules is not provably safe: not done.
      - Guest stack words cannot show where a section was entered (recompiled calls push no return addresses). New
        setting sampler_mark_section=HEX: the sampler mark is the holder's thread id while that section is held;
        tools/samples.py --mark ID counts only those samples. Run 232 (sampler_thread=audio, bc9a50): under the lock
        bank code is about half (bank_a7eb6b92 15 %, runRoutine self 14 % -- mostly the call-out loop: each call of a
        translated bank routine into a game helper 0x8980e0-0x89819f returns to runNative, dynamic_call, re-enter at
        a resume point --, LazyFlags::materialize 6.6 %, other banks ~6 %, resolve 1.7 %, memcmp 1.5 %), the helpers
        0x898xxx ~10 %, 0x537310 4.6 %, 0x8b70b0 4.5 %, 0x8a25b8 3.4 %, 0x536c40 2.3 %.
      - **audio_thread_core** (new, default "audio"): the EA audio thread had the Helper role (core 3, with all other
        guest threads, io, a job phase worker and a GXM command list worker). Moving it to core 2 left its hold times
        unchanged but cut the game thread's relock (re-taking the execution lock after a wait) from ~4 ms to 0.3 ms
        per frame and its SND lock wait from ~5 to ~2.3-3.5 ms: a helper thread holding the execution lock was
        preempted by the priority-15 audio thread on the same core. Pairs (benchmark, core 2 vs core 3): 20.1 / 19.1
        (230 / 229), 21.5 / 19.1 (233 / 234), 19.7 / 18.3 (235 / 236): **about +8 %**. No audio underruns logged;
        sound by ear not yet judged by the user.
      **Next (speed):** (1) bank code call-outs without the round trip: tools/recompiler/bankcode.py could call
      library targets (fixed 0x89xxxx helpers) directly instead of returning to runNative (~10 % of the lock hold);
      (2) native hot guest functions (71e660, 727230, 79fa60, 7e83e0, 5fcdc0, 699420); (3) PGO; (4) check the
      other helper threads for the same inversion (relock is ~0.3 ms now, so little left there).
   237-248 (2026-11-19). **v0.1.0 published; cutscene characters (bodies and faces).**
      - Release v0.1.0 on GitHub (Swisha33/NFSCVita); cpu_clock default 0, stream_track_kb default 46000,
        CI fixes (-Werror warnings in winapp.cpp / msvcr71.cpp). The user's PSVshell holds 500 MHz: every benchmark
        so far ran at 500.
      - New log line: each Carbon effect / technique that draws through the fixed-function stand-in, once
        ("d3dx9: effect X technique N (name) draws through the fixed-function stand-in"). Races: IDI_SCREENEFFECT_FX
        techniques 1/2/5 (blur, downscale), IDI_FE_FX, IDI_WATERSHADER_FX technique 1. Cutscenes added
        IDI_WORLDBONE_FX and IDI_UCAP_FX.
      - The career start (title -> profile -> Career -> START CAREER) is reachable with auto_input
        "40-66:cross:tap": prologue NIS (scene_introcareer07ng), the pursuit, BUSTED, the arrest NIS
        (scene_arrestf06ng). work/menurun.sh 247 "40-66:cross:tap" 4000 90 36 390 captures both.
      - **Bodies** (user: "in cutscenes player models are missing"): IDI_WORLDBONE_FX (vs_1_1/vs_3_0: three blend
        weights, index * 4 into camBlendMatrices c0-c63, dp4 against the blended rows) had no family, so the stand-in
        drew bone-space positions. New family (EffectKindCarbonBone) reuses UG1VITA's worldbone_v / world_f: same
        skinning maths (bone() = the vs's dp4 rows), colour 0.5 so world_f gives the texture (technique 1's ps_1_1 is
        "mov r0, t0"). Run 245: driver's body, jacket, hands and hair right; face missing.
      - **Faces**: IDI_UCAP_FX positions are not in the mesh: x/y/z = sum over 8 float4 sets (texcoord 1-8) of
        dot(cavPcaWeights{X,Y,Z}[k], set k), then skinned like the body (vertex declaration: pos, tex0, weight float3,
        index float3, 8 x float4: exactly MaxVertexElements = 12). New vertex inputs aTex4..aTex8 (UsageTex4..8),
        program cucap_v.cg (compiled on the Vita, tools/shadercomp.ps1 -Only cucap_v), PCA weights stored after the
        bones in the block (BlendMatrices now 352 floats; carbonBlockStore slot sub-offsets). Run 247: shapes and
        animation right, but technique 0's colour is a PCA atlas (five tiles of DIFFUSEMAP, colour weights
        cavPcaWeightsR/G/B in ps_3_0): rainbow faces. Setting ucap_technique (default 2): SetTechnique on the UCAP
        effect selects technique 2 ("main_1_1", ps_1_1: HEIGHTMAP texture rgb), whose pass binds the plain face
        texture. Run 248: faces right (eyes, mouth animation). The protagonist's face stays blurred on purpose (the
        game hides it).
      - Settings: carbon_bones (default on: bodies and faces), ucap_technique.
   253-258 (2026-11-19). **Free-roam crash after the prologue: the city section's range.**
      - Run 253 (user): entering the free roam after the prologue, `guest memory exhausted (requested 74368384
        bytes)`: the city's section 2500 (streaml5ra.bun at 14405632) is requested a second time; 74480 KB free in
        417 ranges, largest 54948 KB. Its first load (career start) read it into one heap block at 0x03996000,
        compacted it and released it; in between, other blocks spread into that range.
      - 0079ebbb: `cmp si, 0x9c4 / mov bl, 1` sends section 2500 to the main heap (bMalloc flags 0x2000) whatever
        pool 7 holds, by design; run 255 with stream_track_kb=74000 confirmed: still a heap block, and the bigger
        pool then left no room for it (fatal at the first load). Other ports: nothing on this (UG2 has no such
        section; the Mac/Android ports have the PC's memory).
      - Run 254: parking the released block in heap.cpp for the next request (released only as a last-resort
        reclaim) failed: the race load needed that memory within seconds (12.5 MB texture cache flushed, then the
        parked block given back). Kept as a patch only.
      - New setting pool_log=N (seconds): free lists of the game's bMalloc pools (table 0xa84c20). Run 256 (boot
        race): audio pool (pool 2, 32 MB) 2.3 MB free -> no savings there; car sub-pool (pool 6, 24 MB) 14-17 MB
        free in a 3-car race (but run 39: 16 MB broke the career car loader).
      - **Fix: soft reservation** (memmap.cpp): the range of the last released block of 48 MB or more stays free
        for a request of at least half its size. Other blocks search outside it first, then after the reclaim (the
        texture cache) outside again, and only then inside it (logged: "soft reservation: first block placed inside
        it", and at the end how much went in). Plus newlib heap 36 -> 30 MB (peak 22976 KB taken): arena 235520 ->
        241664 KB. Run 257 (without the reclaim step): one 1 MB heap chunk went in during the race load, largest
        range 72048 KB (580 KB short). Run 258: nothing inside it in the whole boot race; the texture cache is
        flushed a few times (12.5 MB at the load, then 1-5 MB); frames per 10 s the same as run 257 (236 vs 243).
      - **To check (user)**: the career path prologue -> free roam with this build (cfg: work/nfscvita258play.cfg =
        play cfg + pool_log=30). Expect "soft reservation of 72628 KB ... ends" when the free roam loads.
   259-260 (2026-11-19). **Sliding world textures when the camera view changes: fixed.**
      - User: road and building textures slide when the camera view changes (seen before, gone, back again).
        UG2 Vita lists a similar open item ("wrong texture for a while"), no fix published.
      - New setting debug_texoffset: counts world commits with a nonzero cvTextureOffset. Run 259: 55-75 % of them,
        values like (0.95, -0.027, -0.298, -0.296), changing with the view: IDI_WORLDNORMALMAP_FX, WORLDPRELIT_FX and
        WORLDREFLECT_FX have no cvTextureOffset, FxEffect::floatsOf returned false without writing, and the world
        family's uninitialized offset[4] (stack leftovers of the camera maths) was added to every UV. It came and
        went with the code layout, which is why it looked fixed for a while.
      - Fix: floatsOf zeros its output when the parameter is missing (covers the car family's reused buffer too).
        Run 260: 0 of 2000 commits, apart from a few IDI_WORLD_FX t1 draws whose x offset cycles 0..1 (real
        scrolling textures); camera switched three times, screenshots normal.
      - auto_input knows the rear touch quadrants (rear_ul, rear_ur, rear_ll, rear_lr; rear_lr = camera).
   261-263 (2026-11-19). **Free roam still crashed; guest copies of static buffers become reclaimable.**
      - Run 261 (user, prologue -> free roam with the soft reservation): same fatal error; 80624 KB free (6 MB more
        than run 253), largest range 62080 KB. A 5996 KB block went into the reservation right after the first
        load, and between the loads the texture cache was flushed dozens of times: the arena is full for the whole
        prologue, so placement alone cannot keep 72.6 MB together. pool_log in the career: audio pool 15.4 MB free
        (but 2.3 MB in a city boot race), car sub-pool 6-18 MB free.
      - The game needs ~10 MB more free memory in that stretch. Biggest candidate: the guest copies of vertex /
        index buffers (~25 MB, 2000+ blocks). The GPU draws from its own copy; the guest copy is only what Lock
        hands out. Buffer::evictShadows (d3d9_resources.cpp) frees the guest copy of every static buffer (not
        D3DUSAGE_DYNAMIC) that is unlocked and was last unlocked 2 s or more ago; Lock copies it back from the GPU
        copy. It is the second step of the d3d9 reclaim, after the texture cache. Lock / Unlock / eviction share a
        recursive mutex. Still-locked buffers (Autosculpt, Mac kit change 27) are never touched.
      - Run 262 (boot race): no eviction needed. Run 263 with the test setting debug_evict_buffers=300 (evict every
        300 frames): 3-15 MB freed each time, screenshots normal, frames per 10 s as run 262. Stale pointers kept
        after Unlock would show as broken geometry: none seen.
      - Run 264 (user): **no crash**, prologue -> free roam. 24 MB of buffer copies freed after the first load; the
        reservation held (one 4 MB block inside it) and the section loaded three more times in the same range.
      - Release v0.1.1 (pre-release): this fix, the sliding textures, cutscene characters.
   266-269 (2026-11-19). **ai_drive in the career; GXM out of host memory.**
      - Game_ForceAIControl 0065c2c0: the argument picks the player (0 first, else last of [0xa9ff5c] / count
        [0xa9ff64]); it switches the vehicle's AI control interface on (0060cb50 with 0x4087c0, +0xc(1)) unless +0x8
        says it is on. ai_drive called it once on entering state 6; in the career the state stays 6 through scenes
        and restarts (run 250: one call, during the intro scene, then the BUSTED loop). Now every 30 frames while
        in state 6.
      - Runs 266 / 267 (career start, ai_drive): `sceGxmCreateRenderTarget(256x256) failed: 0x805b0002` (GXM out of
        memory). New log line at that failure: free user -2048 KB, cdram 39936 KB. The gxm_buffers heap had used up
        phycont and put two 8 MB chunks in main memory; GPU heaps never gave chunks back.
      - Fix (gxm_render.cpp): phycont overflow goes to video memory (CDRAM) first, main memory last; the texture
        heap (CDRAM) gets main memory as its last resort; a chunk that becomes empty is freed (not the first).
        Runs 268 / 269: no crash in 7-8 min, 30 fps; the career resumed from the save (scene intro05ng02, the
        prologue was done in run 264), then streaml5ra sections load and unload in turn (the car moves). Not yet
        seen on screen: screenshot_every stopped after two shots in run 269 (a loading screen) and wrote none in
        run 268 -- a screenshot bug to look at.
      - User test with this build (2026-11-19): free roam fine. Their crash before it: the in-game screen resolution set
        to the highest; restarted at default. GitHub issue #1 (v0.1.0, sizzle2100-svg) crashed silently after ~7 min;
        issue #2: no headlights, picture stretched. The stretch is real for everyone: the game makes a 640x480 (4:3)
        device by default ("back buffer 640x480 with depth") and the port stretches it to 960x544. Next: widescreen
        (Mac port core.nfsc stages 1-2 from ThirteenAG: 00712ac0, 0071b780 projection, HUD; UG2 Vita widescreen.cpp),
        then headlights, and what high resolutions do (crash).
   270-272 (2026-11-19). **Widescreen (issue #2): done.** Clue: the macOS port's core.nfsc (ThirteenAG's
      NFSCarbon.WidescreenFix, MIT; same exe, so its addresses apply as they are). The 640x480 back buffer stays
      (no extra pixels); everything is laid out for the 960x544 display and squeezed into it (anamorphic), so the
      Vita's stretch gives the right shape. generate.py WIDESCREEN_PATCHES: projection 0071b780 reads its aspect
      term and three constants from slots 0xc2b440..0xc2b450; the minimap's 120.0 (005d52f9/005d5356) and the menu
      width 640.0 (cursor 00731665, Autosculpt line 007c9214/007c925b) read slots 0xc2b454/0xc2b458; the six HUD
      centre immediates 320.0 and the PiP x / width get the 960x544 values; the game's widescreen HUD layout is
      forced on (SETZ 005dc507/005d52b2, JNZ 005b6bae/005b6b5b). src/nfsc/widescreen.cpp: FE floats 009e8f8c
      (x scale 2/(480*aspect)), 00a604ac / 009c778c (centre 423.5), 009d5f3c, arrest blur 009d0584; native hooks
      on 0071b780 (views 1/4 hor+: aspect 1/1, x 0.7304, half angle 0.4347, y 1.225; mirror view 3 37/40; other
      views the game's values), 005d5280 (left/right HUD groups moved out by 103.5 via FindObject/GetCenter/
      SetCenter) and 00750b30 (mirror/PiP quads narrowed by (4/3)/aspect). Setting widescreen_fov 0/1/2 (the fix's
      view scaling). No off switch: the immediates are compiled in.
      Run 271 (boot race, ai_drive): views 1/3/4 hooked, "widescreen HUD on"; screenshots: car in proportion,
      round minimap and tacho at the screen corners. Run 272 (front end): title, career menu and garage centred
      4:3 with the right shape. Not ported: rain droplets (00722cb0), loading-screen pillarboxes, water reflection
      size, movie placement.
   273-278 (2026-11-19). **Headlights (issue #2): done.** The world family drew every technique with cworld
      (texture * vertex colour). The techniques the game picks at the Vita preset (log: IDI_WORLD_FX and
      IDI_WORLDNORMALMAP_FX t3, the road's IDI_WORLDREFLECT_FX t5; all ps_1_1) add a projected headlight:
      HEADLIGHT_SAMPLER_OLD (stage 1) at the position projected by cmHeadlight1WVP (divided by w) times
      HEADLIGHT_CLIP_SAMPLER (stage 2) at (w * 0.005 + 0.5, 0) times headlight_colour_scale.x (0.5 / 0.4 in run 277).
      New programs cworldhl_v / cworldhl_f (shaders/, compiled on the Vita) for passes with HEADLIGHT_SAMPLER_OLD on
      stage 1; slots CarbonHeadlightWvp (PWorld storage) and CarbonHeadlight (PHeadlightWVP, x = colour scale);
      effectSamplerStage now passes stages 1 and 2 of world draws (run 277 drew the beam black: only stage 0 was
      forwarded). Setting headlights=off draws cworld again; debug_headlight=N logs the values every N commits.
      Tried first (runs 274-276): technique 2's per-pixel light (cavWorldHeadlightPosition/Direction, falloff and
      cone): lit walls, but the road only faintly (grazing N.L) -- not what the preset draws on PC; replaced.
      Run 278: beam on the road ahead of the car, 19-20 fps as before (timing_log on).
      - Seen in runs 254/256/257: at the end of the 1-lap boot race (~130 s) 13-40 s without frames (the
        descriptor limit is reached at the same time). Old? Check before blaming new changes.
   279-287 (2026-11-21). **The long freeze at the end of the boot race: the chunk unloader 006a83a0.**
      PR #3 (texture_zcache_mb, LZ4 store of dropped kept texture levels): 0 hits in every run (one texture
      read-back per race, 19 ms); 18.4 vs 20.5 fps in 280/281. Not merged. The user saw a freeze of 130 s in run
      281 (no zcache) after the bench stopped reading; 282 (off) 28 s, 283 (on) 27-46 s, 284 over 103 s, 285 none:
      both builds, always around frame 2530, main thread holding the execution lock, guest stack only the main
      loop's SEH frame (handler 009a5efd = main loop 006b7930; the "code addresses" are SEH handlers, not returns).
      The stall watchdog now sets sampler mark 1 while stalled (`samples.py --mark 1`): run 286's stalled 23 s are
      93 % in sub_6a83a0's own loop. 006a83a0 (cdecl; start, size) unloads a block's chunks last to first, but
      chunks link only forward: it walks from the start, rings the last 64, unloads them, and walks again up to
      the lowest one: ~n*n/128 header reads, each a cache miss in a section of several MB. Native version
      src/nfsc/chunk_unload.cpp (NATIVE_OVERRIDES): the same calls in the same order; setting
      fast_chunk_unload=off runs the translated body. Run 288 (first version, a std::vector of all chunks):
      std::bad_alloc at the unload (newlib heap 3.4 MB free). Now no heap: checkpoints every stride-th chunk
      (4096 slots, stride doubles when full), then each segment re-read into a fixed buffer and unloaded backwards
      (~2 header reads per chunk). Runs 291-293 (400 s each): no stall, no crash, ~7900 frames (before: ~3500 by
      then); the longest frames are the first load's, as before. Also new: the LiveArea logo layout
      (template_logo.xml.in).
   294-308 (2026-11-21/22). **Quick race with top-tier cars froze at the load (issue #1).** Main thread in the
      race-start callback 006bc100 -> 007e4b30 (car loader 0xb77018), waiting `while [0xb7702c] call 006b68d0`.
      (1) The car pool (stream_subpool_kb, 24000 KB; PC 65000) is too small: run 299 (36 MB) 33.7 MB taken,
      still loading. 46000 KB now; s_reserveMemory 48 -> 38 MB (arena 236 -> 246 MB; user memory free after the
      arena 20 MB at boot, 4 MB at a race load; GPU heaps overflow to CDRAM). A 40 MB pool alone broke the city
      section (run 296: 88.7 MB free, largest 68.5 MB): MemMap::reserveTop keeps the top 72.7 MB free from boot
      (city_reserve=off), ended only by a block that fits it (the 82.8 MB streaming pool ended a plain one: run
      297). Run 305: Lamborghini quick race and free roam load; run 308 (AI race, 400 s): 18.5 fps, no stall.
      (2) Still open: the freeze returns when the teammate's car has finished loading in the quick race menu
      before X (runs 302-307; X earlier: loads). Watches (debug_watch) at the freeze: [0xb7702c] 1 (loader busy)
      since the teammate pick, preview state machine 0xb76860 ([+0x34], 007dba00) back to 0, callback queue
      (0xa99358 count 0xa995dc, deferred 0xa992d8 count 0xa995e4, 16 slots) empty, loader [+0x1c] 6 requested,
      [+0x20] 0 loaded, [+0xc28] -1. Last read: murcielago/geometry.bin 11280 bytes at 1056768. Ruled out: the
      native chunk unloader (fast_chunk_unload=off froze too), pool room (car pool 12 MB free). Leads: the
      resource-file completion 0055fb41 skips the loaded callback when [ebx+3] is set; 007d7440 (preview) queues
      007e6ef0 -> 007e6e00 (waits for 0xb76860 state 0/2, then 007e4bb0 starts the next car load).
      New: file_log=on logs every open and read.
   351-353 (2026-11-23, user away: "frames drop with other cars, crashes and when the city is in sight").
      **Call-tree profile of slow vs fast frames.**
      - Other ports checked (GitHub, 2026-11-23): NFSUG2_VITA 0.8.5 (unload fences, PGO re-recorded; perf ideas:
        d3d fast layer, optimised_particles, audio_mixer_split), UG1VITA 1.0.1 (Underground-only natives: physics,
        GJK, AI, car render; half-precision shaders), Mac port (HD reflections), Android 0.1.13 (core affinity).
      - New: sampler_stack=on (src/platform/sampler.cpp) stores, per sample, the return addresses on the game thread's
        stack (words above sp right after a BL/BLX into this module); sampler_frame_marks=on marks every Present with
        the frame's length. tools/samples_stack.py keeps only verified chains (each return address's BL must call the
        function of the frame below; indirect calls and thunks accepted) and prints inclusive ms per frame for all,
        fast (<= 42 ms) and slow (>= 55 ms) frames.
      - Run 351 baseline 20.7 fps (g_WorldLodLevel 0, g_CarLodLevel 0 already: the Low preset). Run 352 (2 ms
        samples, 3300 frames): mean 47 ms; fast 36 ms, slow 68 ms. Inclusive per frame (fast / slow): frame render
        00699230 18.4 / 28.4 (main view 0072e5b0 13.6 / 22.4, mesh submit 00727230 9.2 / 16.3); simulation (message
        dispatch 005612c0 -> 00760d30 / 00767870) 5.9 / 10.8 (more fixed 1/60 s steps in a long frame, flat inside);
        streaming callbacks (006b68d0 -> track bundle 007a2a80 -> section load 006ad8f0) 0.1 / 8.5: the hitches when
        new city sections arrive. Inside the section load ~half is 0046cec0 (QueryPerformanceCounter -> ticks), called
        per resource.
      - By leaf: translated guest code 61 %, our d3d9/effect layer 23 % (applyPass 3.1, carbonBlockStore 1.9,
        Device::draw 1.7, applyFamily 1.4, lookup 1.4, D3DXMatrixMultiply 1.1, setMatrices 1.0), system libraries
        5.9 % (76 % of it sceClibMemcpy in Buffer::Unlock: vertex-buffer uploads), QueryPerformanceCounter + its
        import thunk 1.6 % (~1000 reads per frame at 678 ns: the wide process clock).
      - debug_buffer_copies=on (run 353): busy dynamic buffers re-created on unlock: all D3DLOCK_DISCARD (0x2000,
        0x2800), 180664 / 192288 / 23056 bytes, locked almost whole, 5-50 MB per 10 s.
      - Fix: pf::ticksUs reads the clock's low word (376 ns) and extends it with the latest high word.
   354-362 (2026-11-23/24). **PGO and the quad-buffer upload: 20.5 -> 23.4 fps.**
      - PGO for the whole program (tools/build.ps1 -PgoGenerate, then -PgoUse; Carbon's main already starts
        pf::pgo). The fully instrumented build (21.8 MB self, ~10 fps) runs out of guest memory at the first race load
        (run 354: 95 MB streaming pool vs 83 MB largest range); profile runs use stream_track_kb=32000. Profiles
        (work/, not in git: derived from the game): 355 free roam AI drive 420 s, 361 boot race with 3 opponents and
        boot_traffic=1.0 450 s, 362 front end (title, garage, memory card list). Instrumented build 57 min, -PgoUse
        build 22 min. Runs 357 / 358 (profile 355 only): 22.5 / 23.0 fps vs 20.2-20.7 (runs 351, 353, 356), min
        15.5 -> 17: about +10 %.
      - Quad buffers: the two 180664-byte dynamic vertex buffers at 0xaaf558 (1024 quads of 4 x 0x2c bytes, made by
        00710ba0, locked DISCARD|NOSYSLOCK each frame by 00710cc0, quads appended at the cursor [0xaaf58c + 4i] by
        00710dd0) were uploaded whole on every unlock. Buffer::setUnlockExtent (d3d9_resources.cpp) lets the game
        side report the bytes written in a DISCARD lock; src/nfsc/main.cpp quadBufferExtent reads the cursor
        (setting quad_upload_extent, default on). A busy buffer re-created for a DISCARD lock now copies only the
        locked range (the rest is undefined in D3D9). Uploads in place 74-98 MB -> 20-23 MB per 10 s (runs 356 / 359);
        the quad buffers now 22-30 KB per unlock. Runs 359 / 360: 23.1 / 23.7 fps (PGO + extent).
      - Note for traffic: boot_traffic 0.3 (benchmark default) is light; career free roam runs at 1.0 (see main.cpp).
      - Merged profile work/pgo_merged1.profdata (355 + 361 + 362): runs 363 23.5, 364 17.0 (one 3.6 s stall, below),
        365 (boot_traffic=1.0, screenshots: flares, tyre smoke, HUD right) 22.7, 366 (traffic 1.0, sampler on the
        audio thread) 23.2 fps. Release builds: -PgoUse work/pgo_merged1.profdata (output build-release-clangpgo;
        tools/vita.ps1 -Self picks a build's .self). Re-record after regenerating the guest code.
      - Open: intermittent stalls from the SND lock 0x00bc9a50 held long by the EA audio thread (thread 9): max hold
        852 ms in run 364, 239-279 ms in runs 334 / 335 / 351 (before today's changes), ~50 ms in most runs. Run 366
        (sampler_thread=audio, sampler_stack, sampler_mark_section=bc9a50): a 131 ms hold = thread 9 in 008cd648 ->
        bank runner 008cd5da, then 10 ms waiting on a mutex (0x109fd, type 0x40) and 111 ms on an LwMutex
        (0x104c1, type 0x100). Bank code ran all native (0 interpreted), so not the interpreter's execution lock.
        Next: name 0x104c1 (log the LwMutex ids at creation) and see who holds it.
   368-374 (2026-11-24). **v0.1.7 published (PGO); a lost play log; the sound-lock holds explained.**
      - The user played two races; the Vita's log was my benchmark run's: tools/vita.ps1 deletes nfscvita.log before
        a test launch (to see that the game started), and run 368 ran after the user's session. vita.ps1 now first
        saves it as work/vita_kept_<time>.log. nfscvita.prev.log does work on normal launches (checked: relaunch
        through port 1338 without the delete); log.cpp copies the file if the rename fails and logs that.
      - sampler_stack now names wait objects every ~30000 waiting samples (LwMutex::nameOf: live LwMutexes by the
        words of their work area). Run 374 (450 s, traffic 1.0, sampler on the audio thread): a 196 ms hold of the SND
        lock = thread 9 in bank code (008cd5da) calling bMalloc 00476b20 -> 00476560 through
        WinApplication::lockedCall (the audio thread runs unlocked, so a call into guest code needs the execution
        lock): 8 ms on mutex 0x109fd, 145 ms on 0x104c1 (type 0x100), then 0x1065b, i.e. waiting for the game thread
        to give up the execution lock, which it does only when it blocks (e.g. on the SND lock itself). Guest
        critical sections are LwMutexes "guest_mutex" (type 0x100000), not this.
        Ideas: let the audio thread's lockedCall get the lock at the game thread's next yield point with priority
        (it is time-critical), or run bMalloc for the audio thread without the execution lock if its pool lock
        makes that safe (check 00476560 for its critical section first).
   375-384 (2026-11-24). **Car environment map over 3 frames, CommitChanges cache, sound-lock waits.**
      - Logging: an unlocked (audio) thread's waits >= 20 ms for the execution lock (lockedCall) or a critical section
        (EnterCriticalSection) are logged; the watchdog line reports the host heap in use (issue #5 died with it
        full). Runs 375/376 (400 s, traffic 1.0): no execution-lock waits, thread 9 waits 20-50 ms for the SND lock
        held by the game thread; no long hold reproduced.
      - GitHub issues #4-#7 (v0.1.4/0.1.5, read only): crashes after "guest memory exhausted" reclaims (a call through
        a null pointer: an allocation failed) and one std::bad_alloc with the host heap full (26 MB). Memory, not speed.
      - fx_commit_cache (default on, d3dx9_fx.cpp): CommitChanges keeps per pass the (stage, texture parameter) list
        and the headlight / car diffuse stage / particle technique flags instead of map and string lookups per draw
        (CommitChanges was 5.9 % of the game thread). Runs 379-384: noise-level (22.3-23.7 with interval 1).
      - envmap_interval (src/nfsc/frame_natives.cpp, native override of 0072e460, regenerated): the car environment
        map's six cube-face views (0xb4ba10 + 0x80 * face, drawn in the loop at 0072ecea with [0xab0b98] = 1) are
        spread over n frames; default 3. Runs 381 (2) 23.6, 382 (3) 24.7, 383 (3) 24.5 vs 384 (1) 23.7 fps;
        screenshots 383/384: the car is lit and reflects as before. 0046cec0 (QueryPerformanceCounter >> [0xa8534c])
        is native too (timer_native).
   385-391 (2026-11-24). **Recompiler passes from NFSUG2_VITA, new PGO profiles: ~26 fps.**
      - generate.py: --float-align all (x87/SSE float loads and stores as plain ARM float accesses; the check build
        reported 0 misaligned sites in races and the front end), --inline-ftol2 (0087eac0 inlined), --pure-natives
        (calls to 0087eac0, 0070f0b0, 0071b630, 0046fcc0, 0079f9c0, 0046cec0 keep guest state in registers),
        --native-writes; now the default codegen in tools/generate.ps1. Run 387: host heap flat at ~23.7 MB over 20 min.
      - Regenerated, profiles re-recorded (work/pgo_falign1.profdata). Runs 388 25.8, 389 26.3 fps vs 382/383 24.6;
        screenshots 391: world, car, HUD as before. x87 depth notes at 004152fe / 00414beb as in older runs.
   392-400 (2026-11-24). **Guard barriers, audio bank calls in place.**
      - Run 392 (stack profile, traffic 1.0): slow frames are streaming (section load 006ad8f0 11 ms of 77; 0055dc20
        walks the model-reference list per solid pack, 3.5 ms); the game thread waits ~3 ms per frame on the SND lock.
      - ~2 % of the game thread sat right after `dmb` barriers: thread-safe guards of function-local
        `static const x = Config::get...()` in per-draw code, and a seq_cst read of s_dumping in Device::draw.
        New pf::LazyBool / pf::LazyInt (config.h, constant-initialized, plain load); s_dumping read relaxed. Runs
        396 / 397 with re-recorded profiles (work/pgo_lazy1.profdata): 26.1 / 25.5 fps (noise-level).
      - Audio bank routines (bankcode.py): calls into the audio library (0x89xxxx) run in place in the Direct
        variant instead of returning to runNative and dynamic_call (the round trip was 18 % of the audio thread's
        time under the SND lock, run 398). Run 399: samples under the lock 39.3 s -> 32.2 s per 170 s run,
        runRoutine 18 % -> 2 %; run 400 (traffic 1.0): the audio thread holds SND ~2.5-2.7 s per 10 s (was
        3.0-3.2), the game's SND wait 2.2 -> 1.5 ms per frame, 25.4 fps; all 116 routines still native, sound
        levels normal. Left under the lock: skid routine bank_a7eb6b92 27 %, mixer 008a25b8 13 %, 005373b0 10 %.
   401-408 (2026-11-24). **GPU heap best fit, what the game thread waits on, faster fragment programs.**
      - Bank routines that only call the library skip their resume switch in the Direct variant (run 401: no
        measurable change). GpuHeap::allocate: best fit through a (size, offset) index instead of a first-fit walk
        over every hole; runs 403 / 404 (traffic 1.0) 25.9 / 26.2 fps, slow frames 159 vs 245 in run 392.
      - The game thread waits ~13 % of the time (run 406, sampler names every 5000 waits; conditions are logged at
        creation): 0x000110a7 (type 0x100, a system object, ~2.7 ms per frame: GXM / display waits), 0x4001016d
        gxm_jobs_done (~1.7 ms), the SND guest_mutex (~1.3 ms). Suspending a waiting thread to read its stack breaks
        its wait (sceKernelWaitCond 0x80028146, run 405): not done.
      - The GPU is busy 24 ms per frame on average (perf lines, run 404), up to 29-30 in the city: near the frame
        time. Fragment programs compiled with fastprecision + fastmath (tools/shadercomp.ps1 -FragmentOptions, new;
        vertex programs and blit_f as shipped): GPU busy 24.1 -> 20.1 ms (run 408), screenshots 407 unchanged.
   409-410 (2026-11-24). **Quad batch uploads.** The quad batch 0074ebc0 (tyre smoke and similar: 96-byte quads,
      cursor at batch +0x10) locks its whole 192000-byte buffer with DISCARD ~15 times a second for a few quads; the
      unlock-extent hook (main.cpp quadBufferExtent, now given the cpu: ebp is the batch at the unlock) uploads only
      the written quads. Run 409 (debug_buffer_copies): busy-buffer copies 28-36 MB -> 2-10 MB per 10 s, in-place
      uploads 20-43 -> 9-15 MB; smoke drawn as before (screenshots). Run 410 (traffic 1.0): 26.2 fps.
      **Next:** water (IDI_WATERSHADER_FX), the screen blur passes; speed (native hot functions, PGO).
   411-414 (2026-11-24). **PGO profile re-recorded** on commit 53a9a4b (work/pgo_final2.profdata, self in
      work/self_final2.self): 27.5 / 24.8 fps default traffic, 25.9 / 26.1 fps traffic 1.0; no errors. Same level as
      runs 404/410 (the profile mostly catches up with the changed functions); use pgo_final2 from now on.
   415-420 (2026-11-24). **Bank library calls not inlined.** Run 415 (stack profile): the 3.8 % "sceNetSocketClose"
      is SceLibKernel (samples.py shows system pcs minus the load bias): sceClibMemcpy/Memset (buffer uploads, guest
      copies) and file syscalls while loading; no single fix. Run 417 (audio thread under SND): the skid routine
      bank_a7eb6b92 was 35 % of the lock time: in Direct runs ThinLTO inlined its ~576 library calls into 300 KB of
      code. bankcode.py now emits them as `NFSC_BANK_CALL` ([[clang::noinline]]): 94 KB, run 418 18.7 % (+ its
      helpers listed apart), audio thread SND hold 2068 -> 1740 ms per 10 s. d3d9_native: the capture checks skip
      their emulated-TLS reads until a capture begins (never in Carbon). Runs 419/420: 26.7 / 26.1 fps (traffic 1.0).
   421-423 (2026-11-24). **Native resampler.** The mixer 0x008a25b8's hot loop was 0x008b70b0 (linear interpolation,
      x87, a voice's +0x24 function pointer, inlined there by PGO's indirect-call promotion). audio_thread.cpp
      replaces its method natively in float (audio_resample_native, default on): mixer 13 % -> 7.9 % + 3.2 % of the
      audio thread's SND-held time; race-time holds ~2650 (417) -> ~2450 (418) -> ~2330 ms per 10 s (421), audio
      levels as before. Runs 422/423: 27.6 / 26.3 fps (traffic 1.0).
   424-428 (2026-11-24). **Streaming placement, input polling, interrupted condition waits.**
      - Run 424 (debug_poke a65358=0, road reflections): same draw count, nothing to gain.
      - -HotCallsite N (build.ps1, PGO's hot call-site inline limit for the generated code): 750 left the big
        functions about as large (727230 44 -> 41 KB): kept as an option, default off.
      - MemMap::place searched with the soft reservation masked (its free bits rewritten and restored per placement:
        ~0.44 ms per streaming frame, run 415); now it searches past the range: 0.02 ms (run 428).
      - gamepad pump(): the pad and rear panel are read at most every 2 ms (the game polls several times per frame):
        sceTouchPeek + sceCtrlPeekBufferPositive 0.53 % -> 0.14 % of the game thread.
      - CondVar waits the sampler interrupts (THREAD_SUSPENDED) could return without the mutex: the next wait failed
        with MUTEX_NOT_OWNED (fatal in sampler runs 405 and 425). The mutex is now taken again when not owned.
      - Runs 426/427: 26.3 (traffic 1.0) / 27.2 fps.
      **Earlier next list (runs 224 / 226 baseline 19.2-19.9 fps):** (1) the rest of applyPass
      (2.4 %) and carbonBlockStore (1 %); (2) more guest hot spots: 71e660, 727230, 79fa60 (bigger, with calls),
      7e83e0 (x87 leaf, 0.8 %); (3) UG2's frame-memory path for DISCARD locks of busy buffers; (4) audio bank
      code without the lock; (5) yieldContext (4-5 %): skip the yield when the waiter holds the lock only briefly.
      Other: the texture flicker; script loading the career save from the menus (d-pad to LOAD).
      Current Vita cfg (2026-11-18 night): work/nfscvita126.cfg-style (boot_race, auto_input driving, screenshots).
      Older note -- Current Vita cfg: boot_race=on, timing_log=on, sampler=on, sampler_thread=audio (set screenshot_every=150
      for screenshots; work/bmp2png.py converts them).
      Open: (minimap mask done), particles, screen effects (downscale/blur passes still run through
      the stand-in for nothing: skip them), IDI_CARNORMALMAP_FX, HUD lost after the pause menu.
      Still open from this stretch: writes to guest address 0 (the game's asserts) should be fatal with a log
      line; the audio pool (32 MB, audio_pool_kb) is untuned; work/*.py older than nfsc_look.py need pefile and
      the old PC's path.

2. **Boot to EA logo** with a stub renderer: registry keys (Install Dir, Language, ...), d3dx9 imports, missing
   kernel32/user32/advapi32 shims, unknown indirect targets -> entry points.
3. **Rendering**: decided 2026-11-15: no hand ports. `src/lib/winapi/d3dx9_fx.cpp` parses Carbon's 30 fx_2_0
   effects (layout from the Mac port, MIT) and implements d3dx9_30's ID3DXEffect (79 slots); the passes' D3D9
   shader bytecode will be translated to Cg at run time and compiled on the device with libshacccg.suprx
   (ur0:data, present on the user's Vita), cached in the data folder. Done: effect object (parameters, techniques,
   passes, annotations, pools). Next: bytecode -> Cg translator, GXM programs per pass, constants from the CTAB.
   Then colour-grading volume LUT (D3DXFillVolumeTexture: black 3D on the Mac without it), cube maps.
4. **Races + performance**: pool sizes, resolution, reflections (car cube 256 -> lower), no post effects, native
   overrides of hot functions (as UG1VITA did: culling, draw queue, mixer, math), PGO.
5. **Comfort**: skip intro movies, rear-touch shifting, Vita button prompts, widescreen (Mac port's slots).

## Ideas from NFSUG2_VITA (danos-x5, v0.8.1, released 2026-11-16; MIT; read-only copy in refs/NFSUG2_VITA)
Another UG1VITA fork, for Underground 2, with the full career playable at ~30 fps. Same runtime layout as ours
(src/lib, src/platform), so its pieces port with little change. Worth taking, in order of use to us now:
- **Direct boot** (src/ug2/boot.cpp, `boot_race` / `boot_mode`): skips the front end into a race (AI-driven).
  For us: a cfg switch that starts the career/a race without the user pressing Start Career every run.
- **Shaders**: no run-time bytecode translator. 11 hand-written Cg program pairs (shaders/*.cg: world, car,
  reflect, headlight, sky, filter, screen, ff), compiled *on the Vita* with libshacccg in a shader-compile mode
  and fetched over FTP (tools/shadercomp/vita_compile.py), shipped as shaders/gxp/. tools/fx/fxtable.py reads
  the exe's 23 compiled effects and writes a table mapping their passes to those programs, failing on anything
  it does not model. Cheaper than our planned translator; Carbon has 30 effects, many shared with UG2's
  families. Decide before starting milestone 3's translator.
- **Movies** (src/ug2/movies.cpp, tools/vita/movies_convert.py): replaces the VP6 codec's frame decode with
  frames from an H.264 .mp4 next to each .vp6 (Vita hardware decoder); the game's demuxer, clock and audio stay.
  Carbon uses the same EA VP6 codec: would let skip_movies default to off.
- **Performance**: PGO profile from Vita runs (pgo/README.md, ~3 % plus far fewer long frames), CapUnlocker for
  the 4th core, audio_defer (SND tick hook on a callback thread so the mixer does not wait for the execution
  lock), a 30 fps lock, scenery small-object cull / distant LOD settings (scenery.h), hitch log (hitch.cpp).
- Comfort: widescreen (widescreen.cpp), rear-touch bindings (padbind.cpp), front touch as mouse, Vita IME text
  entry, LiveArea art made from the user's game files (tools/vita/livearea_art.py).
- Their generator runs in ~5 min for SPEED2.EXE (ours ~60 min for a much larger NFSC.exe): worth comparing
  tools/recompiler/generate.py and games/ug2.py (per-game table module) with ours later.

## Before a release
- Remove the clock setting in `src/nfsc/main.cpp` (cpu_clock, default 444 MHz; added for bring-up because PSVshell
  cannot be used before the game starts), so players tune clocks with PSVshell as usual. Check whether the bus/GPU
  clock calls inherited from UG1VITA also need to go.

## Risks (updated with evidence)
- Memory is the biggest one: 12 MB guest image + translated code + Carbon's pools vs ~365 MB user RAM.
- Performance: UG1VITA already dips below 30 fps with Underground; Carbon is heavier.
- Shaders: 30 effects by hand. The GXP compiler (psp2cgc) is not freely distributable; UG1VITA ships compiled GXP.
  Alternative: compile Cg on the device at first start with libshacccg.suprx (what vitaGL ports do).

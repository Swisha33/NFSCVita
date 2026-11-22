# Testing on the Vita

For players, the [README](../README.md#install) covers installing. This page is the development loop.

## Over Wi-Fi
With [vitacompanion](https://github.com/devnoname120/vitacompanion) on the Vita (FTP on port 1337, commands on 1338)
a build goes to the Vita without a VPK install. Set the Vita's address once in `tools/env.local.ps1` (git ignores it):

```powershell
$env:VITA_IP = "192.168.x.y"
```

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\vita.ps1 run -Seconds 60 -Out work\vita_run1.log
```

`run` uploads `build-release-clang/NFSCVita.self` as the app's `eboot.bin`, starts it, waits and fetches the log;
`log` fetches it again later, `close` closes the app. A changed `sce_sys` (icon, LiveArea, version) still needs a VPK
install. When port 1338 stops answering, the plugin or the Vita hung: restart the Vita.

## Reading a run
`ux0:data/NFSCVITA/nfscvita.log` is written while the game runs, so it holds the last lines before a crash. It is
synced about every 10 s ("stall: watchdog alive", "d3d9: N frames presented"); a crash can look like a freeze, and
its last lines may only arrive on a later fetch. Real crashes also leave `ux0:data/psp2core-*.psp2dmp`.
The registry values the game writes are kept in `ux0:data/NFSCVITA/registry.txt`.

## Useful settings (nfscvita.cfg)
- `timing_log=on`: frame time breakdown, execution lock and critical section statistics every 10 s
  (`tools/perfsum.py LOG` sums them up).
- `sampler=on` (with CapUnlocker): sampling profiler into `samples.bin`; `sampler_thread=audio` profiles the EA audio
  thread, `sampler_mark_section=HEX` marks the time a critical section is held. Read with
  `python -I tools/samples.py samples.bin build-release-clang/NFSCVita.elf [--callers] [--mark ID]`.
- `boot_race=on` skips the front end into a race; `ai_drive=on` lets the AI drive (benchmark);
  `auto_input=FROM-TO:buttons[:hold|tap|weave]` presses buttons without a player.
- `screenshot_every=N` (`screenshot_from`, `screenshot_count`) saves frames as `shot_*.bmp`.
- `texture_zcache_mb=8` (experimental, default 0 = off): kept texture copies dropped by the `texture_cache_mb` cap
  are stored LZ4-compressed, and a later lock decompresses them instead of reading the GPU copy back.
  `texture_zcache_mem=cdram` (default) or `main` picks the memory for it. With `timing_log=on` a
  `perf: zcache ...` line follows the `perf: textures ...` line: compare the zcache hit time with the read-back
  time there. DXT levels barely compress and are mostly rejected (stores N/M).
- `cpu_clock=444` sets the CPU clock at start for a test without PSVshell (PSVshell, when running, overrides it).

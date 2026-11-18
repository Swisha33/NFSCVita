# Third-party code and licensing

NFSCVita's own code, and the code it keeps from UG1VITA (NDRWhun, github.com/NDRWhun/UG1VITA), are licensed under
the MIT License ([LICENSE](LICENSE)). The parts below come from other
projects and keep their own terms.

## pefile

`tools/recompiler/disasm/pefile.py` and `tools/recompiler/disasm/ordlookup/`, from the pefile project (Ero Carrera,
version 2018.8.8), MIT License. The license text is in `tools/recompiler/disasm/LICENSE.pefile`.

## NFSUG2_VITA

`src/platform/shadercomp.cpp`, `include/platform/shadercomp.h` and `tools/shadercomp_list.cmake` (the on-Vita Cg
compile mode), and `src/platform/sampler.cpp` / `include/platform/sampler.h` (sampling profiler), from NFSUG2_VITA
by danos-x5 (github.com/danos-x5/NFSUG2_VITA), MIT License. Also based on it: `tools/vita/livearea_art.py`, the
Vita control layout (`src/nfsc/pad.cpp`) and the audio bank translation design (`tools/recompiler/bankcode.py`,
`src/nfsc/bankcode.cpp`).

## nfs-carbon-mac

Carbon's addresses and data layouts used in `src/lib/winapi/d3dx9_fx.cpp` (fx_2_0 effect layout),
`src/nfsc/pad.cpp` (controller binding table) and `src/nfsc/main.cpp`, from nfs-carbon-mac by vasist15-source
(github.com/vasist15-source/nfs-carbon-mac), MIT License.

## Capstone

The recompiler disassembles with Capstone (BSD 3-Clause License), version pinned in
`tools/recompiler/requirements.txt`. It is not part of the repository: pip or `tools/recompiler/bootstrap.py`
installs it into `.pydeps/`.

## LLVM compiler-rt

The optional profile-guided build links compiler-rt's profile runtime (Apache License 2.0 with LLVM Exceptions). It
is not part of the repository: `cmake/pgo-runtime.cmake` downloads it for the compiler's version.

## Need for Speed Carbon

Game code, binaries and assets are the property of Electronic Arts Inc. and its licensors and are not part of this
repository. Users supply their own copy of the game; the code generated from its executable and audio banks
(`src/nfsc/generated/`) is not tracked. The icon and LiveArea images in `sce_sys/` and `assets/logo.png` are plain
text drawn by `tools/vita/plain_art.py`; images made from the game's own art (`tools/vita/livearea_art.py`, into
`sce_sys/game/`) are not tracked.

## Need for Speed Underground

`src/nfsu/` (not built) and `assets/glyphs.bin` / `assets/icons/` come from UG1VITA.

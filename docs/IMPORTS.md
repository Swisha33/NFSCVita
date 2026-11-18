# Import layer for NFSC.exe

## Status (2026-11-15): every import has a shim

| DLL | Shims | File | Notes |
|---|---|---|---|
| msvcr71 | 132 functions + `_iob`, `_adjust_fdiv` | src/lib/winapi/msvcr71.cpp, msvcr71_stdio.cpp | names `crt_<export>` |
| msvcp71 | 3 | msvcp71.cpp | VC 7.1 `std::string` layout |
| d3dx9_30 | 19 | d3dx9_30.cpp | math real; effects, DDS cube/volume, LoadSurfaceFromSurface, FillVolumeTexture fail and log (rendering milestone) |
| rpcrt4 | 2 | rpcrt4.cpp | |
| kernel32 +16, user32 +6, advapi32 +7, ws2_32 +14 | 43 | nfsc_imports.cpp | waitable timers on a host thread; network fails |

Known gaps, by design for now:
- **C++ exceptions**: `_CxxThrowException` stops the game with the thrown type and message in the log. Catching needs
  MSVC's frame handler to resume inside a translated function (next step if a throw shows up at run time).
- **setjmp/longjmp** need the next generation run (generate.py SETJMP = 0x87ef70); until then a longjmp traps.
- **x87 results of CRT imports** (`floor`, `sqrt`, `_CIpow`, ...): the generator now knows their stack effect
  (generate.py IMPORT_FPU_DELTAS, disasm/fpustack.py); also takes effect at the next generation.
- Local time is UTC; `getenv` finds nothing; stdout/stderr go to the log as `guest: ...` lines.

Generator changes that need a new run of `tools/generate.ps1` (about 65 min): SETJMP, IMPORT_FPU_DELTAS.
`tools/recompiler/rewrite_imports.py` rewrote the import block of the existing `nfsc.cpp` for the new shim names
(no new generation needed for that).

NFSC.exe has 348 imports from 15 DLLs. UG1VITA already implements 187 of them (same names, used by Underground).
The 161 missing ones are listed by `work/imports.py` (spike); this file says how each group gets done.

## Decision: native shims for msvcr71/msvcp71 (not a translated DLL)

The Mac and Android ports translate msvcr71.dll/msvcp71.dll like the game. Here they get native shims, as in
UG1VITA's style:

- UG1VITA's generator and writer handle one module; a second translated image needs rebasing (the DLLs want
  0x7c340000, outside the arena) and multi-module output.
- The CRT's hot functions (memcpy, memset, strlen, sqrt, floor, ...) are faster as native ARM code.
- cdecl and varargs already work: a shim with no declared parameters reads its arguments from `cpu.esp + 4...`
  and the stdcall wrapper pops only the return address, which is cdecl (see `user32::wsprintfA`).

### Conventions
- cdecl, fixed arguments: zero-parameter shim, `app->getMemory<x86::reg32>(cpu.esp + 4 + 4*i)`.
- Varargs (printf family, sscanf): one guest formatter shared with `user32::formatGuest`, extended with %f/%e/%g,
  %I64, %n, and a matching scanner.
- `double` results (`_CIpow`, `_CIfmod`, `_CI*`, `ceil`, `floor`, `sqrt`, `cos`, ...): pushed on the guest x87
  stack (`cpu.fpu.push`). `_CI*` take their operands in ST0/ST1 instead of on the stack.
- 64-bit results (`_time64`, `_mktime64`, `_lseek` is 32): EDX:EAX.
- thiscall (msvcp71 basic_string ctor/dtor, exception ctor/dtor): `this` in ECX, callee pops the arguments
  (stdcall-like), so typed shims work with ECX read from cpu.
- Callbacks into guest code (`qsort`, `bsearch` comparators, `_onexit`, `_initterm`, `_beginthreadex`): call guest
  functions through the runtime's dynamic call (`app->dynamic_call(address, cpu)`) with a pushed frame.

### Data imports
`_iob` (FILE[20], 32 bytes each) and `_adjust_fdiv` (int) are variables: the IAT slot must hold the address of
guest memory, not a fake function address. Writer change: a DATA_IMPORTS table, the registration stores a guest
address the runtime allocates.

### C++ exceptions
`__CxxFrameHandler`, `_CxxThrowException`, `_except_handler3`, `_XcptFilter`, `terminate`, `type_info`/`exception`
members. First step: find out whether Carbon actually throws at run time (xrefs of `_CxxThrowException`); if only on
error paths, log and terminate. Full SEH-based unwinding only if a real path needs it.

### setjmp
`_setjmp3`/`longjmp` are imports. UG1VITA's setjmp support keys on a guest setjmp function called from generated
code; for an import the call site in generated code must be marked instead (generator: calls through the IAT slot
of `_setjmp3` handled like `SETJMP`). Check the xrefs first: it may be unused.

### Call counts (2026-11-15, E8 calls to each import's `jmp [iat]` thunk, rough)
sprintf ~535, sscanf ~53, memcpy ~41, strlen ~39, _CIpow ~19, _snprintf ~19, memset ~19, **_CxxThrowException ~18**,
printf ~7, _vsnprintf ~4, bsearch ~3, **_setjmp3 1** (thunk 0x87ef70), longjmp 1 direct `call [iat]`. Many CRT
functions are reached through thunks MSVC places in the code (0x87eb42.., 0x9911ca..), so most counts above are
lower bounds. C++ exceptions are thrown for real: who throws and who catches is the next thing to find
(UG1VITA's kernel32 already dispatches SEH handlers: RaiseException / RtlUnwind).

## Other DLLs
| DLL | Missing | Plan |
|---|---|---|
| d3dx9_30 | 19 | UG1VITA's d3dx9.cpp has effects and math as native overrides of the linked-in D3DX; turn them into import shims. New: CreateEffectFromResourceA (resources IDI_*_FX, fx_2_0), CreateVolume/CubeTextureFromFileInMemory (DDS), FillVolumeTexture (calls a guest callback per texel: Carbon's colour-grading LUT, black 3D without it), LoadSurfaceFromSurface, Save* (stub). Mac port kit changes 7-11 describe each. |
| kernel32 | 15 | Interlocked*, TryEnterCriticalSection, waitable timers, GlobalMemoryStatusEx (report a 512 MB machine), QueueUserAPC, GetSystemTime... |
| user32 | 6 | MapVirtualKey(Ex)A, ToUnicode, GetKeyboardLayout, GetCursorPos, SendInput (stub). |
| advapi32 | 7 | token/SID checks (not admin), CryptGenRandom (sceKernelGetRandomNumber), RegCreateKeyA. |
| rpcrt4 | 2 | UuidToStringA, RpcStringFreeA. |
| ws2_32 | 14 | Stubs that fail: offline only. |
| msvcp71 | 3 | basic_string<char> ctor (const char*), copy ctor, dtor: MSVC 7.1 layout (16-byte SSO buffer, size +0x14, capacity +0x18, 0x1c bytes). |

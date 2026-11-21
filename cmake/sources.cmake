# Hand-written runtime sources. The generated game code is globbed separately.

# Direct3D frame model and texture layouts (platform neutral).
set(NFSU_RENDER_SOURCES
  src/platform/render.cpp
  src/platform/texture.cpp
)
# Direct3D 9 and D3DX layer.
set(NFSU_D3D9_SOURCES
  src/lib/winapi/d3d9.cpp
  src/lib/winapi/d3d9_device.cpp
  src/lib/winapi/d3d9_native.cpp
  src/lib/winapi/d3d9_record.cpp
  src/lib/winapi/d3d9_resources.cpp
  src/lib/winapi/d3d9_state.cpp
  src/lib/winapi/d3d9_volume.cpp
  src/lib/winapi/d3dx9.cpp
)

set(NFSU_PLATFORM_SOURCES
  ${NFSU_RENDER_SOURCES}
  src/platform/config.cpp
  src/platform/shadercomp.cpp
  src/platform/sampler.cpp
  src/platform/cores.cpp
  src/platform/filesystem.cpp
  src/platform/jobs.cpp
  src/platform/ime.cpp
  src/platform/log.cpp
  src/platform/memfuncs.cpp
  src/platform/perf.cpp
  src/platform/time.cpp
  src/platform/sha256.cpp
  src/platform/sync.cpp
  src/platform/msgqueue.cpp
  src/platform/mmx_selftest.cpp
  src/platform/fpu_selftest.cpp
  src/platform/gxm/gxm_device.cpp
  src/platform/gxm/gxm_render.cpp
  # Compressed texture read-back cache (VitaBoost zcache + LZ4, vendored).
  src/third_party/vitaboost/vb_zcache.c
  src/third_party/vitaboost/lz4.c
)

set(NFSU_LIB_SOURCES
  src/lib/audio.cpp
  src/lib/controls.cpp
  src/lib/cpu.cpp
  src/lib/event.cpp
  src/lib/file.cpp
  src/lib/fpucontrol.cpp
  src/lib/gamepad.cpp
  src/lib/gamethreads.cpp
  src/lib/heap.cpp
  src/lib/jump.cpp
  src/lib/library.cpp
  src/lib/mutex.cpp
  src/lib/palette.cpp
  src/lib/registry.cpp
  src/lib/renderer.cpp
  src/lib/surface.cpp
  src/lib/thread.cpp
  src/lib/timer.cpp
  src/lib/winapp.cpp
  src/lib/window.cpp
)

set(NFSU_WINAPI_SOURCES
  ${NFSU_D3D9_SOURCES}
  src/lib/winapi/advapi32.cpp
  src/lib/winapi/com.cpp
  src/lib/winapi/ddraw.cpp
  src/lib/winapi/dinput.cpp
  src/lib/winapi/dinput8.cpp
  src/lib/winapi/dsound.cpp
  src/lib/winapi/gdi32.cpp
  src/lib/winapi/iforce2.cpp
  src/lib/winapi/imports.cpp
  src/lib/winapi/kernel32.cpp
  src/lib/winapi/netapi32.cpp
  src/lib/winapi/ole32.cpp
  src/lib/winapi/shell32.cpp
  src/lib/winapi/shfolder.cpp
  src/lib/winapi/support.cpp
  src/lib/winapi/tapi32.cpp
  src/lib/winapi/user32.cpp
  src/lib/winapi/winmm.cpp
  src/lib/winapi/ws2_32.cpp
  # NFSC.exe's imports beyond Underground's (docs/IMPORTS.md)
  src/lib/winapi/d3dx9_30.cpp
  src/lib/winapi/d3dx9_fx.cpp
  src/lib/winapi/msvcp71.cpp
  src/lib/winapi/msvcr71.cpp
  src/lib/winapi/msvcr71_stdio.cpp
  src/lib/winapi/nfsc_imports.cpp
  src/lib/winapi/rpcrt4.cpp
  src/lib/winapi/ddraw/idirectdraw2.cpp
  src/lib/winapi/ddraw/idirectdrawpalette.cpp
  src/lib/winapi/ddraw/idirectdrawsurface.cpp
  src/lib/winapi/dinput/idirectinput2.cpp
  src/lib/winapi/dinput/idirectinput8.cpp
  src/lib/winapi/dinput/idirectinputdevice.cpp
  src/lib/winapi/dsound/idirectsound.cpp
  src/lib/winapi/dsound/idirectsound3dlistener.cpp
  src/lib/winapi/dsound/idirectsoundbuffer.cpp
)

# Port code that works with guest addresses of NFSC.exe (UG1VITA's src/nfsu/ is Underground's).
set(NFSU_GAME_PORT_SOURCES
  src/nfsc/audio_kernels.cpp
  src/nfsc/audio_thread.cpp
  src/nfsc/bankcode.cpp
  src/nfsc/native_math.cpp
  src/nfsc/pad.cpp
  src/nfsc/texturepacks.cpp
  src/nfsc/widescreen.cpp
)

# Runtime sources that include headers generated from the game binary (nfsc_layout.h).
set(NFSU_GENERATED_HEADER_SOURCES
  src/lib/memmap.cpp
)

# Runtime sources that build without the generated code (the CI runtime check).
set(NFSU_STANDALONE_SOURCES
  ${NFSU_PLATFORM_SOURCES}
  ${NFSU_LIB_SOURCES}
  ${NFSU_WINAPI_SOURCES}
  ${NFSU_GAME_PORT_SOURCES}
)

set(NFSU_RUNTIME_SOURCES
  ${NFSU_STANDALONE_SOURCES}
  ${NFSU_GENERATED_HEADER_SOURCES}
)

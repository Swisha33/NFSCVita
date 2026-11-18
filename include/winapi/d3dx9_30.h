#ifndef d3dx9_30_H_
#define d3dx9_30_H_

// The D3DX 9 functions NFSC.exe imports from d3dx9_30.dll (stdcall). The math is native; effects and texture loading
// are the rendering milestone's (docs/STATUS.md): until then they fail and log what the game asked for.

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace d3dx9_30
{

typedef x86::reg32 Ptr;     // guest address

Ptr D3DXMatrixMultiply(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr a, Ptr b);
Ptr D3DXMatrixTranspose(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr m);
Ptr D3DXMatrixInverse(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr determinant, Ptr m);
Ptr D3DXMatrixPerspectiveLH(WinApplication* app, x86::CPU& cpu, Ptr out, float w, float h, float zn, float zf);
Ptr D3DXMatrixOrthoLH(WinApplication* app, x86::CPU& cpu, Ptr out, float w, float h, float zn, float zf);
Ptr D3DXMatrixTranslation(WinApplication* app, x86::CPU& cpu, Ptr out, float x, float y, float z);
Ptr D3DXVec3Transform(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m);
Ptr D3DXVec4Transform(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m);
Ptr D3DXVec3TransformNormal(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v, Ptr m);
Ptr D3DXVec3TransformCoordArray(WinApplication* app, x86::CPU& cpu, Ptr out, UINT outStride, Ptr v, UINT vStride,
                                Ptr m, UINT count);
Ptr D3DXVec3Normalize(WinApplication* app, x86::CPU& cpu, Ptr out, Ptr v);

HRESULT D3DXCreateEffectPool(WinApplication* app, x86::CPU& cpu, Ptr pool);
HRESULT D3DXCreateEffectFromResourceA(WinApplication* app, x86::CPU& cpu, x86::reg32 device, HMODULE module,
                                      Ptr resource, Ptr defines, Ptr include, DWORD flags, x86::reg32 pool,
                                      Ptr effect, Ptr errors);
HRESULT D3DXCreateCubeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, Ptr data,
                                              UINT size, Ptr texture);
HRESULT D3DXCreateVolumeTextureFromFileInMemory(WinApplication* app, x86::CPU& cpu, x86::reg32 device, Ptr data,
                                                UINT size, Ptr texture);
HRESULT D3DXLoadSurfaceFromSurface(WinApplication* app, x86::CPU& cpu, x86::reg32 destination, Ptr destinationPalette,
                                   Ptr destinationRect, x86::reg32 source, Ptr sourcePalette, Ptr sourceRect,
                                   DWORD filter, DWORD colorKey);
HRESULT D3DXFillVolumeTexture(WinApplication* app, x86::CPU& cpu, x86::reg32 texture, x86::reg32 function, Ptr data);
HRESULT D3DXSaveSurfaceToFileA(WinApplication* app, x86::CPU& cpu, Ptr file, DWORD format, x86::reg32 surface,
                               Ptr palette, Ptr rect);
HRESULT D3DXSaveSurfaceToFileInMemory(WinApplication* app, x86::CPU& cpu, Ptr buffer, DWORD format,
                                      x86::reg32 surface, Ptr palette, Ptr rect);

}}

#endif

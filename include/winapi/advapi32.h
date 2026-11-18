#ifndef advapi32_H_
#define advapi32_H_
#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace advapi32
{

typedef x86::reg32  HKEY;

LSTATUS RegCloseKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey);
LSTATUS RegCreateKeyExA(WinApplication* app, x86::CPU& cpu,
                        HKEY hKey, LPCSTR lpSubKey, DWORD Reserved, LPSTR lpClass, DWORD dwOptions,
                        REGSAM samDesired, LPSECURITY_ATTRIBUTES lpSecurityAttributes, PHKEY phkResult,
                        LPDWORD lpdwDisposition);
LSTATUS RegFlushKey(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey);
LSTATUS RegOpenKeyA(WinApplication* app, x86::CPU& cpu,
                    HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
LSTATUS RegOpenKeyExA(WinApplication* app, x86::CPU& cpu,
                      HKEY hKey, LPCSTR lpSubKey, DWORD ulOptions, REGSAM samDesired, PHKEY phkResult);
LSTATUS RegQueryValueExA(WinApplication* app, x86::CPU& cpu,
                         HKEY hKey, LPCSTR lpValueName, LPDWORD lpReserved, LPDWORD lpType, LPBYTE lpData, LPDWORD lpcbData);
LSTATUS RegSetValueExA(WinApplication* app, x86::CPU& cpu,
                       HKEY hKey, LPCSTR lpValueName, DWORD Reserved, DWORD dwType, const BYTE* lpData,
                       DWORD cbData);

// ---- Imports of NFSC.exe (src/lib/winapi/nfsc_imports.cpp) ----
LSTATUS RegCreateKeyA(WinApplication* app, x86::CPU& cpu, HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult);
BOOL AllocateAndInitializeSid(WinApplication* app, x86::CPU& cpu, x86::reg32 pIdentifierAuthority, BYTE nSubAuthorityCount,
                              DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7,
                              x86::reg32* pSid);
BOOL CheckTokenMembership(WinApplication* app, x86::CPU& cpu, HANDLE TokenHandle, x86::reg32 SidToCheck, BOOL* IsMember);
x86::reg32 FreeSid(WinApplication* app, x86::CPU& cpu, x86::reg32 pSid);
BOOL CryptAcquireContextA(WinApplication* app, x86::CPU& cpu, x86::reg32* phProv, LPCSTR szContainer, LPCSTR szProvider,
                          DWORD dwProvType, DWORD dwFlags);
BOOL CryptGenRandom(WinApplication* app, x86::CPU& cpu, x86::reg32 hProv, DWORD dwLen, BYTE* pbBuffer);
BOOL CryptReleaseContext(WinApplication* app, x86::CPU& cpu, x86::reg32 hProv, DWORD dwFlags);

}}

#endif

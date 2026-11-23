// Win32 functions NFSC.exe imports beyond the ones UG1VITA implements for Underground (kernel32, user32, advapi32,
// ws2_32). The game plays offline: the network functions fail, their event objects work.

#include <winapi/advapi32.h>
#include <winapi/kernel32.h>
#include <winapi/user32.h>
#include <winapi/ws2_32.h>
#include "crt.h"

#include <lib/window.h>
#include <lib/gamepad.h>
#include <lib/event.h>
#include <lib/file.h>
#include <lib/mutex.h>
#include <lib/timer.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <psp2/kernel/processmgr.h>
#include <cstdio>
#include <cstring>
#include <map>

namespace win32
{

namespace
{

// Waitable timers: auto- or manual-reset events that one host thread signals at their due time (and again every
// period). Times are in microseconds of the process clock.
struct WaitableTimer
{
    uint64_t due;
    uint32_t periodMs;
};

pf::LwMutex s_timerMutex("waitable timers");
std::map<x86::reg32, WaitableTimer> s_timers;   // event handle -> schedule
pf::Semaphore* s_timerWake = nullptr;
pf::Thread* s_timerThread = nullptr;

int timerService(void*)
{
    for (;;)
    {
        uint64_t wait = 100000;
        {
            pf::LwLockGuard guard(s_timerMutex);
            const uint64_t now = sceKernelGetProcessTimeWide();
            for (auto it = s_timers.begin(); it != s_timers.end();)
            {
                if (it->second.due <= now)
                {
                    Event::setHandle(it->first);
                    if (!it->second.periodMs)
                    {
                        it = s_timers.erase(it);
                        continue;
                    }
                    it->second.due = now + uint64_t(it->second.periodMs) * 1000;
                }
                wait = std::min<uint64_t>(wait, it->second.due - now);
                ++it;
            }
        }
        s_timerWake->waitForUs(uint32_t(std::max<uint64_t>(wait, 500)));
    }
    return 0;
}

}

namespace kernel32
{

// MEMORYSTATUSEX: a 512 MB machine with half of it free (the PC game sizes nothing from it that matters here).
BOOL GlobalMemoryStatusEx(WinApplication* app, x86::CPU& cpu, x86::reg32 lpBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!lpBuffer || crt::ld32(lpBuffer) != 64)
        return 0;
    const uint64_t values[7] = { 512ull << 20, 256ull << 20, 1024ull << 20, 768ull << 20, 2047ull << 20,
                                 1536ull << 20, 0 };
    crt::st32(lpBuffer + 4, 50);
    memcpy(crt::ptr(lpBuffer + 8), values, sizeof(values));
    return 1;
}

void FatalAppExitA(WinApplication* app, x86::CPU& cpu, UINT uAction, LPCSTR lpMessageText)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(uAction);
    pf::fatal("FatalAppExitA: %s", lpMessageText ? lpMessageText : "");
}

DWORD QueueUserAPC(WinApplication* app, x86::CPU& cpu, x86::reg32 pfnAPC, HANDLE hThread, x86::reg32 dwData)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::logError("QueueUserAPC(0x%08x, thread 0x%x, 0x%x): not supported", unsigned(pfnAPC), unsigned(hThread),
                 unsigned(dwData));
    return 0;
}

// In x86::AtomicSection like the generated lock-prefixed instructions: the EA audio thread runs without the
// execution lock (src/nfsc/audio_thread.cpp).
LONG InterlockedCompareExchange(WinApplication* app, x86::CPU& cpu, LONG* Destination, LONG Exchange, LONG Comparand)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::AtomicSection atomic;
    const LONG old = *Destination;
    if (old == Comparand)
        *Destination = Exchange;
    return old;
}

LONG InterlockedExchangeAdd(WinApplication* app, x86::CPU& cpu, LONG* Addend, LONG Value)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::AtomicSection atomic;
    const LONG old = *Addend;
    *Addend = LONG(x86::reg32(old) + x86::reg32(Value));
    return old;
}

BOOL TryEnterCriticalSection(WinApplication* app, x86::CPU& cpu, LPCRITICAL_SECTION lpCriticalSection)
{
    if (!(*lpCriticalSection)->tryLock())
        return 0;
    kernel32::critSecEntered(app, cpu, lpCriticalSection);
    return 1;
}

DWORD SetThreadIdealProcessor(WinApplication* app, x86::CPU& cpu, HANDLE hThread, DWORD dwIdealProcessor)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hThread);
    RC_USE(dwIdealProcessor);
    return 0;
}

BOOL GetDiskFreeSpaceExA(WinApplication* app, x86::CPU& cpu, LPCSTR lpDirectoryName, x86::reg64* lpFreeBytesAvailable,
                         x86::reg64* lpTotalNumberOfBytes, x86::reg64* lpTotalNumberOfFreeBytes)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpDirectoryName);
    x86::reg64 total = 0, free = 0;
    if (!File::diskSpace(&total, &free))
    {
        total = 4ull << 30;
        free = 2ull << 30;
    }
    if (lpFreeBytesAvailable)
        *lpFreeBytesAvailable = free;
    if (lpTotalNumberOfBytes)
        *lpTotalNumberOfBytes = total;
    if (lpTotalNumberOfFreeBytes)
        *lpTotalNumberOfFreeBytes = free;
    return 1;
}

HANDLE CreateWaitableTimerA(WinApplication* app, x86::CPU& cpu, LPSECURITY_ATTRIBUTES lpTimerAttributes,
                            BOOL bManualReset, LPCSTR lpTimerName)
{
    return CreateEventA(app, cpu, lpTimerAttributes, bManualReset, 0, lpTimerName);
}

// Due time: negative = relative in 100 ns units, positive = absolute FILETIME.
BOOL SetWaitableTimer(WinApplication* app, x86::CPU& cpu, HANDLE hTimer, const x86::sreg64* lpDueTime, LONG lPeriod,
                      x86::reg32 pfnCompletionRoutine, x86::reg32 lpArgToCompletionRoutine, BOOL fResume)
{
    RC_USE(lpArgToCompletionRoutine);
    RC_USE(fResume);
    if (!lpDueTime)
        return 0;
    if (pfnCompletionRoutine)
        pf::logError("SetWaitableTimer: completion routine 0x%08x not called", unsigned(pfnCompletionRoutine));
    int64_t delay100ns = *lpDueTime;
    if (delay100ns >= 0)
    {
        FILETIME now;
        GetSystemTimeAsFileTime(app, cpu, &now);
        delay100ns = std::max<int64_t>(0, delay100ns - int64_t(uint64_t(now.dwHighDateTime) << 32 | now.dwLowDateTime));
    }
    else
        delay100ns = -delay100ns;
    ResetEvent(app, cpu, hTimer);
    pf::LwLockGuard guard(s_timerMutex);
    if (!s_timerThread)
    {
        s_timerWake = new pf::Semaphore("waitable timer wake", 0);
        s_timerThread = new pf::Thread("waitable timers", timerService, nullptr, pf::Thread::PriorityHigh, 16 * 1024);
        s_timerThread->start();
    }
    s_timers[hTimer] = WaitableTimer{ sceKernelGetProcessTimeWide() + uint64_t(delay100ns / 10),
                                      lPeriod > 0 ? uint32_t(lPeriod) : 0 };
    s_timerWake->post();
    return 1;
}

void GetSystemTime(WinApplication* app, x86::CPU& cpu, LPSYSTEMTIME lpSystemTime)
{
    GetLocalTime(app, cpu, lpSystemTime);
}

BOOL SystemTimeToFileTime(WinApplication* app, x86::CPU& cpu, const SYSTEMTIME* st, LPFILETIME lpFileTime)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!st || !lpFileTime || st->wMonth < 1 || st->wMonth > 12)
        return 0;
    int64_t y = st->wYear;
    const unsigned m = st->wMonth;
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + st->wDay - 1;
    const int64_t days = era * 146097 + int64_t(yoe * 365 + yoe / 4 - yoe / 100 + doy) - 719468;
    const int64_t ms = ((days * 24 + st->wHour) * 60 + st->wMinute) * 60000 + int64_t(st->wSecond) * 1000
                     + st->wMilliseconds;
    const uint64_t ticks = uint64_t(ms * 10000 + 116444736000000000LL);
    lpFileTime->dwLowDateTime = DWORD(ticks);
    lpFileTime->dwHighDateTime = DWORD(ticks >> 32);
    return 1;
}

BOOL GetComputerNameA(WinApplication* app, x86::CPU& cpu, LPSTR lpBuffer, LPDWORD nSize)
{
    RC_USE(app);
    RC_USE(cpu);
    static const char name[] = "PSVITA";
    if (!lpBuffer || !nSize || *nSize < sizeof(name))
    {
        if (nSize)
            *nSize = sizeof(name);
        return 0;
    }
    memcpy(lpBuffer, name, sizeof(name));
    *nSize = sizeof(name) - 1;
    return 1;
}

BOOL CreateProcessA(WinApplication* app, x86::CPU& cpu, LPCSTR lpApplicationName, x86::reg32 lpCommandLine,
                    x86::reg32 lpProcessAttributes, x86::reg32 lpThreadAttributes, BOOL bInheritHandles,
                    DWORD dwCreationFlags, x86::reg32 lpEnvironment, LPCSTR lpCurrentDirectory,
                    x86::reg32 lpStartupInfo, x86::reg32 lpProcessInformation)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(lpProcessAttributes);
    RC_USE(lpThreadAttributes);
    RC_USE(bInheritHandles);
    RC_USE(dwCreationFlags);
    RC_USE(lpEnvironment);
    RC_USE(lpCurrentDirectory);
    RC_USE(lpStartupInfo);
    RC_USE(lpProcessInformation);
    pf::logError("CreateProcessA(%s, %s): no processes on the Vita", lpApplicationName ? lpApplicationName : "-",
                 lpCommandLine ? crt::ptr(lpCommandLine) : "-");
    return 0;
}

BOOL IsDebuggerPresent(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

UINT GetSystemDirectoryA(WinApplication* app, x86::CPU& cpu, LPSTR lpBuffer, UINT uSize)
{
    RC_USE(app);
    RC_USE(cpu);
    static const char path[] = "C:\\WINDOWS\\system32";
    if (!lpBuffer || uSize < sizeof(path))
        return sizeof(path);
    memcpy(lpBuffer, path, sizeof(path));
    return sizeof(path) - 1;
}

}

namespace user32
{

UINT SendInput(WinApplication* app, x86::CPU& cpu, UINT cInputs, x86::reg32 pInputs, int cbSize)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(cInputs);
    RC_USE(pInputs);
    RC_USE(cbSize);
    return 0;
}

namespace
{

// Set 1 scan codes of the keys a US keyboard layout maps for the letters, digits and common keys.
struct KeyCode
{
    UINT vk;
    UINT scan;
};

const KeyCode s_keys[] = {
    { 0x1b, 0x01 }, { '1', 0x02 }, { '2', 0x03 }, { '3', 0x04 }, { '4', 0x05 }, { '5', 0x06 }, { '6', 0x07 },
    { '7', 0x08 }, { '8', 0x09 }, { '9', 0x0a }, { '0', 0x0b }, { 0x08, 0x0e }, { 0x09, 0x0f },
    { 'Q', 0x10 }, { 'W', 0x11 }, { 'E', 0x12 }, { 'R', 0x13 }, { 'T', 0x14 }, { 'Y', 0x15 }, { 'U', 0x16 },
    { 'I', 0x17 }, { 'O', 0x18 }, { 'P', 0x19 }, { 0x0d, 0x1c }, { 0x11, 0x1d }, { 'A', 0x1e }, { 'S', 0x1f },
    { 'D', 0x20 }, { 'F', 0x21 }, { 'G', 0x22 }, { 'H', 0x23 }, { 'J', 0x24 }, { 'K', 0x25 }, { 'L', 0x26 },
    { 0x10, 0x2a }, { 'Z', 0x2c }, { 'X', 0x2d }, { 'C', 0x2e }, { 'V', 0x2f }, { 'B', 0x30 }, { 'N', 0x31 },
    { 'M', 0x32 }, { 0x12, 0x38 }, { 0x20, 0x39 }, { 0x70, 0x3b }, { 0x71, 0x3c }, { 0x72, 0x3d }, { 0x73, 0x3e },
    { 0x74, 0x3f }, { 0x75, 0x40 }, { 0x76, 0x41 }, { 0x77, 0x42 }, { 0x78, 0x43 }, { 0x79, 0x44 },
    { 0x24, 0x47 }, { 0x26, 0x48 }, { 0x21, 0x49 }, { 0x25, 0x4b }, { 0x27, 0x4d }, { 0x23, 0x4f }, { 0x28, 0x50 },
    { 0x22, 0x51 }, { 0x2d, 0x52 }, { 0x2e, 0x53 }, { 0x7a, 0x57 }, { 0x7b, 0x58 },
};

}

// Map types: 0 virtual key -> scan code, 1 scan code -> virtual key, 2 virtual key -> character, 3 as 1.
UINT MapVirtualKeyA(WinApplication* app, x86::CPU& cpu, UINT uCode, UINT uMapType)
{
    RC_USE(app);
    RC_USE(cpu);
    if (uMapType == 2)
        return (uCode >= 'A' && uCode <= 'Z') || (uCode >= '0' && uCode <= '9') || uCode == 0x20 ? uCode : 0;
    for (const KeyCode& k : s_keys)
    {
        if (uMapType == 0 && k.vk == uCode)
            return k.scan;
        if ((uMapType == 1 || uMapType == 3) && k.scan == uCode)
            return k.vk;
    }
    return 0;
}

UINT MapVirtualKeyExA(WinApplication* app, x86::CPU& cpu, UINT uCode, UINT uMapType, HANDLE dwhkl)
{
    RC_USE(dwhkl);
    return MapVirtualKeyA(app, cpu, uCode, uMapType);
}

int ToUnicode(WinApplication* app, x86::CPU& cpu, UINT wVirtKey, UINT wScanCode, const BYTE* lpKeyState, LPWSTR pwszBuff,
              int cchBuff, UINT wFlags)
{
    WORD c = 0;
    const int n = ToAscii(app, cpu, wVirtKey, wScanCode, lpKeyState, &c, wFlags);
    if (n <= 0 || !pwszBuff || cchBuff < 1)
        return 0;
    pwszBuff[0] = c & 0xff;
    return 1;
}

// The pad's pointer (Gamepad::pointer: rear touch panel = the screen, right stick, rear tap = click) in screen
// coordinates of the focus window. Carbon's menus read it at 00731580 (world map, mouse menus).
BOOL GetCursorPos(WinApplication* app, x86::CPU& cpu, LPPOINT lpPoint)
{
    if (!lpPoint)
        return 0;
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        pf::logInfo("input: GetCursorPos read (pointer from the rear touch panel and the right stick)");
    }
    const HWND window = HWND(Window::focus());
    RECT client = { 0, 0, 640, 480 };
    if (window)
        user32::GetClientRect(app, cpu, window, &client);
    int x = 0, y = 0;
    Gamepad::pointer(int(client.right - client.left), int(client.bottom - client.top), &x, &y);
    lpPoint->x = x;
    lpPoint->y = y;
    if (window)
        user32::ClientToScreen(app, cpu, window, lpPoint);
    return 1;
}

// US English.
HANDLE GetKeyboardLayout(WinApplication* app, x86::CPU& cpu, DWORD idThread)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(idThread);
    return 0x04090409;
}

}

namespace advapi32
{

LSTATUS RegCreateKeyA(WinApplication* app, x86::CPU& cpu, HKEY hKey, LPCSTR lpSubKey, PHKEY phkResult)
{
    return RegCreateKeyExA(app, cpu, hKey, lpSubKey, 0, nullptr, 0, 0xf003f, nullptr, phkResult, nullptr);
}

// The process is not an administrator: a SID exists, the token is never a member.
BOOL AllocateAndInitializeSid(WinApplication* app, x86::CPU& cpu, x86::reg32 pIdentifierAuthority, BYTE nSubAuthorityCount,
                              DWORD a0, DWORD a1, DWORD a2, DWORD a3, DWORD a4, DWORD a5, DWORD a6, DWORD a7,
                              x86::reg32* pSid)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pIdentifierAuthority);
    RC_USE(nSubAuthorityCount);
    RC_USE(a0); RC_USE(a1); RC_USE(a2); RC_USE(a3); RC_USE(a4); RC_USE(a5); RC_USE(a6); RC_USE(a7);
    if (!pSid)
        return 0;
    *pSid = crt::allocate(48, true);
    return *pSid != 0;
}

BOOL CheckTokenMembership(WinApplication* app, x86::CPU& cpu, HANDLE TokenHandle, x86::reg32 SidToCheck, BOOL* IsMember)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(TokenHandle);
    RC_USE(SidToCheck);
    if (!IsMember)
        return 0;
    *IsMember = 0;
    return 1;
}

x86::reg32 FreeSid(WinApplication* app, x86::CPU& cpu, x86::reg32 pSid)
{
    RC_USE(app);
    RC_USE(cpu);
    crt::release(pSid);
    return 0;
}

BOOL CryptAcquireContextA(WinApplication* app, x86::CPU& cpu, x86::reg32* phProv, LPCSTR szContainer, LPCSTR szProvider,
                          DWORD dwProvType, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(szContainer);
    RC_USE(szProvider);
    RC_USE(dwProvType);
    RC_USE(dwFlags);
    if (!phProv)
        return 0;
    *phProv = 1;
    return 1;
}

// Not cryptographic: the game uses it for identifiers.
BOOL CryptGenRandom(WinApplication* app, x86::CPU& cpu, x86::reg32 hProv, DWORD dwLen, BYTE* pbBuffer)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hProv);
    static uint64_t state = 0;
    if (!state)
        state = sceKernelGetProcessTimeWide() | 1;
    for (DWORD i = 0; i < dwLen; ++i)
    {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        pbBuffer[i] = BYTE(state >> 24);
    }
    return 1;
}

BOOL CryptReleaseContext(WinApplication* app, x86::CPU& cpu, x86::reg32 hProv, DWORD dwFlags)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(hProv);
    RC_USE(dwFlags);
    return 1;
}

}

namespace ws2_32
{

namespace
{

const int SOCKET_ERROR = -1;

}

HANDLE WSACreateEvent(WinApplication* app, x86::CPU& cpu)
{
    return kernel32::CreateEventA(app, cpu, nullptr, 1, 0, nullptr);
}

BOOL WSACloseEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::CloseHandle(app, cpu, hEvent);
}

BOOL WSASetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::SetEvent(app, cpu, hEvent);
}

BOOL WSAResetEvent(WinApplication* app, x86::CPU& cpu, HANDLE hEvent)
{
    return kernel32::ResetEvent(app, cpu, hEvent);
}

DWORD WSAWaitForMultipleEvents(WinApplication* app, x86::CPU& cpu, DWORD cEvents, const HANDLE* lphEvents, BOOL fWaitAll,
                               DWORD dwTimeout, BOOL fAlertable)
{
    return kernel32::WaitForMultipleObjectsEx(app, cpu, cEvents, lphEvents, fWaitAll, dwTimeout, fAlertable);
}

int WSARecv(WinApplication* app, x86::CPU& cpu, x86::reg32 s, x86::reg32 lpBuffers, DWORD dwBufferCount,
            x86::reg32 lpNumberOfBytesRecvd, x86::reg32 lpFlags, x86::reg32 lpOverlapped,
            x86::reg32 lpCompletionRoutine)
{
    RC_USE(app); RC_USE(cpu); RC_USE(s); RC_USE(lpBuffers); RC_USE(dwBufferCount); RC_USE(lpNumberOfBytesRecvd);
    RC_USE(lpFlags); RC_USE(lpOverlapped); RC_USE(lpCompletionRoutine);
    return SOCKET_ERROR;
}

int WSARecvFrom(WinApplication* app, x86::CPU& cpu, x86::reg32 s, x86::reg32 lpBuffers, DWORD dwBufferCount,
                x86::reg32 lpNumberOfBytesRecvd, x86::reg32 lpFlags, x86::reg32 lpFrom, x86::reg32 lpFromlen,
                x86::reg32 lpOverlapped, x86::reg32 lpCompletionRoutine)
{
    RC_USE(app); RC_USE(cpu); RC_USE(s); RC_USE(lpBuffers); RC_USE(dwBufferCount); RC_USE(lpNumberOfBytesRecvd);
    RC_USE(lpFlags); RC_USE(lpFrom); RC_USE(lpFromlen); RC_USE(lpOverlapped); RC_USE(lpCompletionRoutine);
    return SOCKET_ERROR;
}

BOOL WSAGetOverlappedResult(WinApplication* app, x86::CPU& cpu, x86::reg32 s, x86::reg32 lpOverlapped,
                            x86::reg32 lpcbTransfer, BOOL fWait, x86::reg32 lpdwFlags)
{
    RC_USE(app); RC_USE(cpu); RC_USE(s); RC_USE(lpOverlapped); RC_USE(lpcbTransfer); RC_USE(fWait); RC_USE(lpdwFlags);
    return 0;
}

int gethostname(WinApplication* app, x86::CPU& cpu, LPSTR name, int namelen)
{
    RC_USE(app);
    RC_USE(cpu);
    static const char host[] = "psvita";
    if (!name || namelen < int(sizeof(host)))
        return SOCKET_ERROR;
    memcpy(name, host, sizeof(host));
    return 0;
}

x86::reg32 gethostbyaddr(WinApplication* app, x86::CPU& cpu, x86::reg32 addr, int len, int type)
{
    RC_USE(app); RC_USE(cpu); RC_USE(addr); RC_USE(len); RC_USE(type);
    return 0;
}

x86::reg32 getservbyname(WinApplication* app, x86::CPU& cpu, LPCSTR name, LPCSTR proto)
{
    RC_USE(app); RC_USE(cpu); RC_USE(name); RC_USE(proto);
    return 0;
}

x86::reg32 getservbyport(WinApplication* app, x86::CPU& cpu, int port, LPCSTR proto)
{
    RC_USE(app); RC_USE(cpu); RC_USE(port); RC_USE(proto);
    return 0;
}

// in_addr by value (network byte order) -> "a.b.c.d" in a static buffer.
x86::reg32 inet_ntoa(WinApplication* app, x86::CPU& cpu, DWORD in)
{
    RC_USE(app);
    RC_USE(cpu);
    const x86::reg32 text = crt::staticBlock("inet_ntoa", 16);
    snprintf(crt::ptr(text), 16, "%u.%u.%u.%u", unsigned(in & 0xff), unsigned((in >> 8) & 0xff),
             unsigned((in >> 16) & 0xff), unsigned(in >> 24));
    return text;
}

DWORD inet_addr(WinApplication* app, x86::CPU& cpu, LPCSTR cp)
{
    RC_USE(app);
    RC_USE(cpu);
    unsigned a, b, c, d;
    if (!cp || sscanf(cp, "%u.%u.%u.%u", &a, &b, &c, &d) != 4 || a > 255 || b > 255 || c > 255 || d > 255)
        return 0xffffffff;
    return DWORD(a | b << 8 | c << 16 | d << 24);
}

}

}

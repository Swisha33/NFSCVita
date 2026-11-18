#ifndef winmm_H_
#define winmm_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace winmm
{

typedef x86::reg32 MMRESULT;
struct TIMECAPS
{
    UINT wPeriodMin;
    UINT wPeriodMax;
};
typedef TIMECAPS*   LPTIMECAPS;
typedef x86::reg32  LPTIMECALLBACK;


UINT joyGetNumDevs(WinApplication* app, x86::CPU& cpu);
BOOL PlaySoundA(WinApplication* app, x86::CPU& cpu,
                x86::reg32 pszSound, HANDLE hmod, DWORD fdwSound);
MMRESULT timeBeginPeriod(WinApplication* app, x86::CPU& cpu,
                         UINT uPeriod);
MMRESULT timeEndPeriod(WinApplication* app, x86::CPU& cpu,
                       UINT uPeriod);
MMRESULT timeGetDevCaps(WinApplication* app, x86::CPU& cpu,
                        LPTIMECAPS ptc, UINT cbtc);
DWORD timeGetTime(WinApplication* app, x86::CPU& cpu);
// The thread started at this guest address gets a timeGetTime that advances by at most maxStepMs per call.
void setPacedThreadStart(x86::reg32 start, uint32_t maxStepMs);
// kernel32 CreateThread: a thread was created.
void threadCreated(x86::reg32 start, x86::reg32 threadId);
// Duty limit of the paced thread (kernel32 SleepEx): its sleeps last at least percent of the time it worked.
void setPacedDuty(uint32_t percent);
DWORD pacedSleep(WinApplication* app, x86::CPU& cpu, DWORD requestedMs);
void pacedWoke(WinApplication* app, x86::CPU& cpu);
MMRESULT timeKillEvent(WinApplication* app, x86::CPU& cpu,
                       UINT uTimerID);
MMRESULT timeSetEvent(WinApplication* app, x86::CPU& cpu,
                      UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      x86::reg32 dwUser, UINT fuEvent);

}}

#endif

#include <winapi/winmm.h>
#include <winapi/support.h>
#include <winapi/kernel32.h>
#include <platform/config.h>
#include <platform/sampler.h>
#include <platform/time.h>
#include <algorithm>
#include <cstring>
#if defined(__vita__)
#include <psp2/kernel/threadmgr.h>
#endif
#include <lib/timer.h>
#include <lib/gamepad.h>
#include <platform/log.h>
#include <x86.h>

namespace win32 { namespace winmm
{

UINT joyGetNumDevs(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return Gamepad::getCount();
}

// System sounds are not played.
BOOL PlaySoundA(WinApplication* app, x86::CPU& cpu,
                x86::reg32 pszSound, HANDLE hmod, DWORD fdwSound)
{
    RC_USE(app);
    RC_USE(cpu);
    RC_USE(pszSound);
    RC_USE(hmod);
    RC_USE(fdwSound);
    return 1;
}

static const MMRESULT TIMERR_NOERROR = 0;
static const MMRESULT TIMERR_NOCANDO = 97;     // TIMERR_BASE + 1
static const UINT PeriodMin = 1;
static const UINT PeriodMax = 1000000;

// Timers always run at 1 ms resolution (the timer service sleeps to the due time), so a period only has to be
// valid: 1 ms up to the maximum timeGetDevCaps reports.
MMRESULT timeBeginPeriod(WinApplication* app, x86::CPU& cpu,
                         UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    return uPeriod >= PeriodMin && uPeriod <= PeriodMax ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

MMRESULT timeEndPeriod(WinApplication* app, x86::CPU& cpu,
                       UINT uPeriod)
{
    RC_USE(app);
    RC_USE(cpu);
    return uPeriod >= PeriodMin && uPeriod <= PeriodMax ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

MMRESULT timeGetDevCaps(WinApplication* app, x86::CPU& cpu,
                        LPTIMECAPS ptc, UINT cbtc)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!ptc || cbtc != sizeof(*ptc))
        return TIMERR_NOCANDO;
    ptc->wPeriodMin = PeriodMin;
    ptc->wPeriodMax = PeriodMax;
    return TIMERR_NOERROR;
}

namespace
{
x86::reg32 s_pacedStart = 0, s_pacedThread = 0;
uint32_t s_pacedStep = 50;
DWORD s_pacedReal = 0, s_pacedValue = 0;
bool s_pacedStarted = false;
uint64_t s_pacedWakeUs = 0;         // when the paced thread's last Sleep returned
uint32_t s_dutyPercent = 100;       // audio_duty: sleep at least this % of the time worked since the last Sleep
uint32_t s_dutyStretches = 0, s_dutyExtraMs = 0;
uint64_t s_dutyLogUs = 0;
}

void setPacedThreadStart(x86::reg32 start, uint32_t maxStepMs)
{
    s_pacedStart = start;
    s_pacedStep = maxStepMs;
}

void setPacedDuty(uint32_t percent)
{
    s_dutyPercent = percent;
}

// The paced thread's Sleep (kernel32 SleepEx): at least s_dutyPercent of the time it worked since its last Sleep,
// so it holds the execution lock at most ~1 / (1 + percent/100) of the time. Run 76: after a load the EA mixer
// ran behind real time and mixed back to back under the lock for 8-20 s (game frozen, sound fine); with the capped
// clock the time it does not get to is skipped instead of piling up.
DWORD pacedSleep(WinApplication* app, x86::CPU& cpu, DWORD requestedMs)
{
    if (!s_pacedThread || !s_dutyPercent || kernel32::GetCurrentThreadId(app, cpu) != s_pacedThread)
        return requestedMs;
    // Without the execution lock the thread competes for nothing: no duty limit.
    if (app->isUnlockedThread(cpu))
        return requestedMs;
    const uint64_t now = pf::ticksUs();
    DWORD ms = requestedMs;
    if (s_pacedWakeUs)
    {
        const uint64_t workedMs = (now - s_pacedWakeUs) / 1000;
        const DWORD floor = DWORD(std::min<uint64_t>(workedMs * s_dutyPercent / 100, 200));
        if (floor > ms)
        {
            ++s_dutyStretches;
            s_dutyExtraMs += floor - ms;
            ms = floor;
        }
    }
    if (now - s_dutyLogUs >= 10000000)
    {
        if (s_dutyStretches)
            pf::logInfo("winmm: audio thread duty limit stretched %u sleeps by %u ms in the last %u s",
                        s_dutyStretches, s_dutyExtraMs, unsigned((now - s_dutyLogUs) / 1000000));
        s_dutyStretches = s_dutyExtraMs = 0;
        s_dutyLogUs = now;
    }
    return ms;
}

void pacedWoke(WinApplication* app, x86::CPU& cpu)
{
    if (s_pacedThread && kernel32::GetCurrentThreadId(app, cpu) == s_pacedThread)
        s_pacedWakeUs = pf::ticksUs();
}

void threadCreated(x86::reg32 start, x86::reg32 threadId)
{
    if (s_pacedStart && start == s_pacedStart)
    {
        s_pacedThread = threadId;
        pf::logInfo("winmm: thread %u (start 0x%08x) gets a paced timeGetTime (at most %u ms per call)",
                    unsigned(threadId), unsigned(start), unsigned(s_pacedStep));
    }
}

// Same clock as GetTickCount. The paced thread's clock advances by at most s_pacedStep per call: a mixer that
// computes what is due from the elapsed time (Carbon's EA audio thread, 008a7c75) then never faces seconds of backlog
// after a stall, which it would mix slower than real time while holding the execution lock (run 55's hang).
DWORD timeGetTime(WinApplication* app, x86::CPU& cpu)
{
    const DWORD now = timeGetTickCount();
    if (!s_pacedThread || kernel32::GetCurrentThreadId(app, cpu) != s_pacedThread)
        return now;
    if (!s_pacedStarted)
    {
        s_pacedStarted = true;
        // sampler=on, sampler_thread=audio: profile this thread instead of the game thread.
        if (strcmp(pf::Config::getString("sampler_thread", "game"), "audio") == 0)
            pf::sampler::retarget(sceKernelGetThreadId());
        s_pacedReal = s_pacedValue = now;
        return now;
    }
    const DWORD step = now - s_pacedReal;
    s_pacedReal = now;
    if (step > s_pacedStep)
    {
        if (step >= 1000)
            pf::logInfo("winmm: paced clock skipped %u ms of audio after a stall", unsigned(step - s_pacedStep));
        s_pacedValue += s_pacedStep;
    }
    else
        s_pacedValue += step;
    return s_pacedValue;
}

MMRESULT timeKillEvent(WinApplication* app, x86::CPU& cpu,
                       UINT uTimerID)
{
    RC_USE(app);
    RC_USE(cpu);
    // TIMERR_NOCANDO for an id that is not pending.
    return win32::Timer::kill(uTimerID) ? TIMERR_NOERROR : TIMERR_NOCANDO;
}

// One-shot (TIME_ONESHOT 0) or periodic (TIME_PERIODIC 1) timers calling a function (TIME_CALLBACK_FUNCTION); the
// game's EA timer re-arms a one-shot timer from its own callback every 10 ms.
// TIME_KILL_SYNCHRONOUS (0x100) needs nothing more: a callback runs guest code only under the execution lock, which
// the caller of timeKillEvent holds, and a killed timer is not fired again.
// Event callbacks (TIME_CALLBACK_EVENT_SET / _PULSE) are not used by the game and trap.
MMRESULT timeSetEvent(WinApplication* app, x86::CPU& cpu,
                      UINT uDelay, UINT uResolution, LPTIMECALLBACK lpTimeProc,
                      x86::reg32 dwUser, UINT fuEvent)
{
    RC_USE(uResolution);
    if (fuEvent & ~0x101u)
        unsupported(app, cpu, "timeSetEvent", "flags 0x%x", fuEvent);
    if (cpu.terminate || !lpTimeProc || uDelay < PeriodMin || uDelay > PeriodMax)
        return 0;
    const MMRESULT id = win32::Timer::set(app, uDelay, lpTimeProc, dwUser, (fuEvent & 1) != 0);
    // Run 79: timer callbacks held the execution lock ~68 ms per frame in the city; which ones are they?
    static unsigned s_logged = 0;
    if (s_logged < 32)
    {
        ++s_logged;
        pf::logInfo("winmm: timeSetEvent %u ms, callback 0x%08x (user 0x%08x), %s -> id %u", unsigned(uDelay),
                    unsigned(lpTimeProc), unsigned(dwUser), (fuEvent & 1) ? "periodic" : "one-shot", unsigned(id));
    }
    return id;
}

}}

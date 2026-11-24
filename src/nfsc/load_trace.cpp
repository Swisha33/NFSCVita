// Diagnostic wrappers around the game's callback queue and the car loader (generate.py NATIVE_OVERRIDES), for the
// quick race freeze when the teammate's car finished loading in the menu (STATUS runs 302-309): the car loader
// 0xb77018 waits with its busy flag [+0x14] set while no callback is queued to clear it.
//
// Setting load_trace=on logs, at most TraceLines lines:
//   006997a0 queue(callback, argument, defer)   cdecl: the entry, the queue counts, the caller
//   006b68d0 run the queue                      entry/exit counts when there is work, and the nesting depth
//   007d7440 car preview start (thiscall; car)  the car, its done byte +0x19, the loader flag
//   007e6e00 car loader re-check (thiscall; car) the loader flag before and after, the preview state
//   007e4b30 race start: unload cars            entry and exit
// Off (default), each calls the translated body and nothing else.

#include <lib/winapp.h>
#include <winapi/kernel32.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <recomp.h>
#include <x86.h>

namespace win32
{
uint64_t lastFrameUs();     // src/lib/winapp.cpp
}

namespace nfsc
{

void sub_6997a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_6b68d0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7d7440_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e6e00_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_7e4b30_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

const reg32 QueueCount = 0x00a995dc, DeferredCount = 0x00a995e4, Running = 0x00a995e0;
const reg32 LoaderFlag = 0x00b7702c, PreviewState = 0x00b76894;
const int TraceLines = 30000;

bool tracing()
{
    static pf::LazyBool on_setting;
    const bool on = on_setting.get("load_trace", false);
    static int s_lines = 0;
    if (!on || s_lines >= TraceLines)
        return false;
    if (++s_lines == TraceLines)
        pf::logInfo("load_trace: line limit reached");
    return true;
}

int s_depth = 0;
// True while 007e4b30 (race start: unload cars) runs: it spins `while [LoaderFlag] call 006b68d0` without a wait
// or a Present, so it never gives up the execution lock. Run 313: the car reads it waits for sat queued at EA's job
// worker (thread 6, entry 008d6b40), which thread_normal keeps at normal priority, so yieldContext never hands it
// the lock: deadlock (quick race freeze when the teammate's car loaded in the menu, issue #1).
bool s_raceStartWait = false;

// Setting race_start_yield (default on): in that loop, give up the execution lock for 1 ms per pass.
void raceStartYield(win32::WinApplication* app, x86::CPU& cpu)
{
    static pf::LazyBool on_setting;
    const bool on = on_setting.get("race_start_yield", true);
    static bool s_logged = false;
    if (!on || !s_raceStartWait || !rc::ld32(LoaderFlag) || !app->holdsContext(cpu))
        return;
    if (!s_logged)
    {
        s_logged = true;
        pf::logInfo("load_trace: race start waits for the car loader; yielding the execution lock (race_start_yield)");
    }
    app->unlockContext(cpu);
    pf::sleepUs(1000);
    app->lockContext(cpu);
}

// Any wait loop that pumps 006b68d0 without a Present has the same deadlock when what it waits for needs an EA job
// worker: 006b7be0 (close a file handle) spins `while ([handle + 0x30] == 0) call 006b68d0` until its read is done,
// and the read's completion runs on job worker 0 (issue #9: black screen after the Kenji boss race, FEAnyMovie.fng
// open, worker 0 with one job, one issued read, main thread holding the lock for 10+ minutes).
// Setting pump_yield (default on): once the pump ran PumpSpins times since the last Present and that Present is
// PumpIdleUs old, each further pass while a job worker (entry 008d6b40) has a job gives up the lock for 200 us.
const unsigned PumpSpins = 64;
const uint64_t PumpIdleUs = 100000;

bool workerBusy()
{
    static reg32 s_workers[4] = {};
    static unsigned s_found = 0;
    if (s_found < 2)
    {
        s_found = 0;
        for (unsigned i = 0; i < 4; ++i)
        {
            s_workers[i] = win32::kernel32::threadParameter(0x008d6b40, i);
            if (s_workers[i])
                ++s_found;
        }
    }
    for (unsigned i = 0; i < 4; ++i)
        if (s_workers[i] && rc::ld32(s_workers[i] + 0x10))
            return true;
    return false;
}

void pumpYield(win32::WinApplication* app, x86::CPU& cpu)
{
    static pf::LazyBool on_setting;
    if (!on_setting.get("pump_yield", true))
        return;
    static uint64_t s_frame = 0;
    static unsigned s_spins = 0;
    static uint32_t s_episodes = 0, s_yields = 0;
    const uint64_t frame = win32::lastFrameUs();
    if (frame != s_frame)
    {
        if (s_yields && s_episodes <= 20)
            pf::logInfo("load_trace: pump_yield: frames again after %u yields", s_yields);
        s_frame = frame;
        s_spins = s_yields = 0;
        return;
    }
    if (++s_spins < PumpSpins || pf::ticksUs() - frame < PumpIdleUs || !workerBusy() || !app->holdsContext(cpu))
        return;
    if (s_yields++ == 0 && ++s_episodes <= 20)
        pf::logInfo("load_trace: the loader pump spins without a frame (%u passes) while a job worker has work; "
                    "yielding the execution lock (pump_yield)", s_spins);
    app->unlockContext(cpu);
    pf::sleepUs(200);
    app->lockContext(cpu);
}

}

namespace native
{

void sub_6997a0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (tracing())
        pf::logInfo("load_trace: queue %08x(%08x) defer %u from %08x | running %u queue %u deferred %u flag %u",
                    unsigned(rc::ld32(cpu.esp + 4)), unsigned(rc::ld32(cpu.esp + 8)),
                    unsigned(rc::ld8(cpu.esp + 12)), unsigned(rc::ld32(cpu.esp)), unsigned(rc::ld8(Running)),
                    unsigned(rc::ld32(QueueCount)), unsigned(rc::ld32(DeferredCount)), unsigned(rc::ld32(LoaderFlag)));
    sub_6997a0_guest(app, cpu);
}

void sub_6b68d0(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 queued = rc::ld32(QueueCount), deferred = rc::ld32(DeferredCount);
    const bool work = queued || deferred;
    if (work && tracing())
        pf::logInfo("load_trace: run depth %d | queue %u deferred %u, first %08x(%08x)", s_depth, unsigned(queued),
                    unsigned(deferred), unsigned(rc::ld32(0x00a99358)), unsigned(rc::ld32(0x00a9935c)));
    ++s_depth;
    sub_6b68d0_guest(app, cpu);
    --s_depth;
    if (s_depth == 0)
    {
        raceStartYield(app, cpu);
        pumpYield(app, cpu);
    }
    if (work && tracing())
        pf::logInfo("load_trace: run depth %d done | queue %u deferred %u flag %u", s_depth,
                    unsigned(rc::ld32(QueueCount)), unsigned(rc::ld32(DeferredCount)), unsigned(rc::ld32(LoaderFlag)));
}

void sub_7d7440(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 car = rc::ld32(cpu.esp + 4);
    const reg32 self = cpu.ecx;
    if (tracing())
        pf::logInfo("load_trace: preview start this %08x car %08x done %u id %08x from %08x | flag %u state %u",
                    unsigned(self), unsigned(car), car ? unsigned(rc::ld8(car + 0x19)) : 0u,
                    car ? unsigned(rc::ld32(car + 8)) : 0u, unsigned(rc::ld32(cpu.esp)),
                    unsigned(rc::ld32(LoaderFlag)), unsigned(rc::ld32(PreviewState)));
    sub_7d7440_guest(app, cpu);
    if (tracing())
        pf::logInfo("load_trace: preview start returns %u | flag %u", unsigned(cpu.eax), unsigned(rc::ld32(LoaderFlag)));
}

void sub_7e6e00(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 car = rc::ld32(cpu.esp + 4);
    const reg32 before = rc::ld32(LoaderFlag);
    sub_7e6e00_guest(app, cpu);
    if (tracing())
        pf::logInfo("load_trace: re-check car %08x | flag %u -> %u, state %u, car done %u", unsigned(car),
                    unsigned(before), unsigned(rc::ld32(LoaderFlag)), unsigned(rc::ld32(PreviewState)),
                    car ? unsigned(rc::ld8(car + 0x19)) : 0u);
}

void sub_7e4b30(win32::WinApplication* app, x86::CPU& cpu)
{
    if (tracing())
        pf::logInfo("load_trace: unload cars from %08x | flag %u queue %u deferred %u running %u",
                    unsigned(rc::ld32(cpu.esp)), unsigned(rc::ld32(LoaderFlag)), unsigned(rc::ld32(QueueCount)),
                    unsigned(rc::ld32(DeferredCount)), unsigned(rc::ld8(Running)));
    s_raceStartWait = true;
    sub_7e4b30_guest(app, cpu);
    s_raceStartWait = false;
    if (tracing())
        pf::logInfo("load_trace: unload cars done | flag %u", unsigned(rc::ld32(LoaderFlag)));
}

}

}

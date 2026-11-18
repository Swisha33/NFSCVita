// Carbon's EA audio thread ("RWAudioCore Dac", entry 0x008A800C, loop 0x008A7DE6) without the execution lock.
//
// Run 80: in the city the audio thread held the execution lock ~50 ms per ~128 ms frame, and the game waited for
// it. Its profile (run 73): render 0x8A74DD ~15 %, tick 0x8993FA's sound objects ~40 % plus bank code ~16 %, all
// reached through function pointers. On PC this thread runs in parallel with the game, synchronised only by EA's
// own locks (0x8D8640: EnterCriticalSection / WaitForSingleObject imports; the thread library's lock xadd /
// lock cmpxchg / xchg, now atomic in the generated code), and the game's allocator locks too (bMalloc -> 0x88C858).
// So the whole thread runs unlocked here; WinApplication::dynamic_call takes the lock for its calls to anything
// but recompiled code (imports, COM methods, bank routines), see WinApplication::setUnlockedThread.
// (NFSUG2_VITA runs only Underground 2's render loop unlocked, behind audits of every module; Carbon's synthesis is
// in the tick, so the narrower design would leave most of the cost under the lock.)
//
// Setting audio_unlocked (default on).

#include <lib/mutex.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <x86.h>

namespace nfsc
{

namespace
{

const x86::reg32 AudioThreadEntry = 0x008a800c;
const x86::reg32 TextBegin = 0x00401000;
const x86::reg32 TextEnd = 0x009c1000;

win32::MethodPtr s_entry = nullptr;

void audioThread(win32::WinApplication* app, x86::CPU& cpu)
{
    win32::Mutex* const lock = app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock);
    if (!lock)
    {
        s_entry(app, cpu);
        return;
    }
    pf::logInfo("audio: the EA audio thread (0x%08x) runs without the execution lock (audio_unlocked)",
                unsigned(AudioThreadEntry));
    app->unlockContext(cpu);
    app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock) = nullptr;
    app->setUnlockedThread(cpu.efs);
    s_entry(app, cpu);
    app->setUnlockedThread(0);
    app->getMemory<win32::Mutex*>(cpu.efs + win32::teb::HostLock) = lock;
    app->lockContext(cpu);
    pf::logInfo("audio: the EA audio thread returned");
}

}

void installUnlockedAudio(win32::WinApplication& app)
{
    app.setGuestText(TextBegin, TextEnd);
    if (!pf::Config::getBool("audio_unlocked", true))
        return;
    s_entry = app.replaceMethod(AudioThreadEntry, &audioThread);
    if (!s_entry)
        pf::logError("audio: thread entry 0x%08x not registered", unsigned(AudioThreadEntry));
}

}

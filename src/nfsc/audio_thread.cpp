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
//
// 0x008b70b0 is the mixer's linear-interpolation resampler (set as a voice's +0x24 function pointer by 0x008adfeb,
// called through it): cdecl (count, float* src, float* dst, int* position, uint32_t* fraction, step, fractionStep);
// dst[i] = (src[p + 1] - src[p]) * fraction * [0x00a7f198] + src[p] with the 32.32 position p.fraction advanced by
// step.fractionStep; fild qword reads the fraction from [0x00a7f190] (high word [0x00a7f194]), which it keeps current.
// Run 418: inlined into the mixer 0x008a25b8 (PGO), its x87 emulation was the mixer's hot loop, 13 % of the audio
// thread's time under EA's SND lock. Natively in float. Setting audio_resample_native (default on).

#include <lib/mutex.h>
#include <lib/teb.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

namespace nfsc
{

namespace
{

const x86::reg32 AudioThreadEntry = 0x008a800c;
const x86::reg32 TextBegin = 0x00401000;
const x86::reg32 TextEnd = 0x009c1000;

win32::MethodPtr s_entry = nullptr;
win32::MethodPtr s_resample = nullptr;

const x86::reg32 Resample = 0x008b70b0, ResampleFraction = 0x00a7f190, ResampleScale = 0x00a7f198;

void resample(win32::WinApplication* app, x86::CPU& cpu)
{
    const x86::reg32 sp = cpu.esp;
    const x86::reg32 positionAddress = rc::ld32(sp + 16), fractionAddress = rc::ld32(sp + 20);
    if (rc::ld32(ResampleFraction + 4) != 0)
        return s_resample(app, cpu);    // fild qword with a high word: not seen; the guest code handles it
    int32_t count = int32_t(rc::ld32(sp + 4));
    const x86::reg32 src = rc::ld32(sp + 8);
    x86::reg32 dst = rc::ld32(sp + 12);
    const x86::reg32 step = rc::ld32(sp + 24), fractionStep = rc::ld32(sp + 28);
    x86::reg32 position = rc::ld32(positionAddress), fraction = rc::ld32(fractionAddress);
    const float scale = rc::ldf32(ResampleScale);
    for (; count > 0; --count)
    {
        const x86::reg32 at = src + position * 4;
        const float a = rc::ldf32(at), b = rc::ldf32(at + 4);
        rc::store<float>(dst, (b - a) * (float(fraction) * scale) + a);
        dst += 4;
        const uint64_t sum = uint64_t(fraction) + fractionStep;
        fraction = x86::reg32(sum);
        position += step + x86::reg32(sum >> 32);
    }
    rc::st32(ResampleFraction, fraction);
    rc::st32(fractionAddress, fraction);
    rc::st32(positionAddress, position);
    cpu.esp = sp + 4;   // pushad / popad: every register as it was
}

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
    if (pf::Config::getBool("audio_resample_native", true))
    {
        s_resample = app.replaceMethod(Resample, &resample);
        if (!s_resample)
            pf::logError("audio: resampler 0x%08x not registered", unsigned(Resample));
    }
    if (!pf::Config::getBool("audio_unlocked", true))
        return;
    s_entry = app.replaceMethod(AudioThreadEntry, &audioThread);
    if (!s_entry)
        pf::logError("audio: thread entry 0x%08x not registered", unsigned(AudioThreadEntry));
}

}

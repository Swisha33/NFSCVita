#include <platform/time.h>
#include <platform/perf.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <atomic>

namespace pf
{

// The process clock from its low word (376 ns per read against 678 for the wide one, sampler run 352; the game reads
// QueryPerformanceCounter ~1000 times per frame), extended to 64 bits with the high word of the latest reading. The
// low word wraps every 71 minutes, far longer than the gap between reads. Monotonic across threads: a reading older
// than the latest one returns the latest one.
uint64_t ticksUs()
{
    perf::count(perf::CountClock);
    static std::atomic<uint64_t> s_latest{0};
    const uint32_t low = sceKernelGetProcessTimeLow();
    uint64_t latest = s_latest.load(std::memory_order_relaxed);
    for (;;)
    {
        uint64_t now = (latest & ~uint64_t(0xffffffffu)) | low;
        if (now < latest)
        {
            if (latest - now < 0x80000000u)
                return latest;
            now += uint64_t(1) << 32;
        }
        if (s_latest.compare_exchange_weak(latest, now, std::memory_order_relaxed))
            return now;
    }
}

uint32_t ticksUsLow()
{
    perf::count(perf::CountClock);
    return sceKernelGetProcessTimeLow();
}

uint32_t ticksMs()
{
    perf::count(perf::CountClock);
    return uint32_t(ticksUs() / 1000);
}

void sleepMs(uint32_t milliseconds)
{
    // Sleep(0) must still let other guest threads take the execution lock.
    sleepUs(milliseconds ? milliseconds * 1000 : 50);
}

void sleepUs(uint32_t microseconds)
{
    sceKernelDelayThread(microseconds);
}

}

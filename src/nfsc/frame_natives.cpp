// Native overrides of per-frame guest functions (generate.py NATIVE_OVERRIDES).
//
// 0072e460 draws one view of the world. The car environment map loop (0072ecea..0072ed1a, in 0072eba0 while
// g_CarEnvironmentMapEnable [0xa65340] is on and the game flow [0xa99bbc] is 6) calls it for the six cube-face views
// at 0xb4ba10 + 0x80 * face with [0xab0b98] = 1: ~1.9 ms of a ~44 ms frame (stack profile, run 377). Setting
// envmap_interval=n (1..6, default 3; 1 = every face every frame, as the game) draws face f only in frames where
// f % n == frame % n;
// the other faces keep the previous picture.
//
// 0046cec0 reads QueryPerformanceCounter and shifts it right by [0xa8534c] (0087edf0: arithmetic 64-bit shift; 64 or
// more gives the sign). The section loader calls it per resource; natively it skips the import thunk.

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/time.h>
#include <recomp.h>
#include <x86.h>

#include <algorithm>

namespace nfsc
{

void sub_72e460_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46cec0_guest(win32::WinApplication* app, x86::CPU& cpu);

uint32_t g_presentedFrames = 0;     // main.cpp framePresent

namespace
{

using x86::reg32;

const reg32 FirstFaceView = 0x00b4ba10, FaceViewStride = 0x80, FaceCount = 6, InEnvLoop = 0x00ab0b98;

int envmapInterval()
{
    static int s_interval = 0;     // 0: not read yet (a plain static: no guard check per call)
    if (!s_interval)
    {
        s_interval = std::min(std::max(pf::Config::getInt("envmap_interval", 3), 1), 6);
        if (s_interval > 1)
            pf::logInfo("envmap: car environment map faces drawn over %d frames (envmap_interval)", s_interval);
    }
    return s_interval;
}

}

namespace native
{

void sub_72e460(win32::WinApplication* app, x86::CPU& cpu)
{
    const int interval = envmapInterval();
    if (interval > 1 && rc::ld32(InEnvLoop) == 1)
    {
        const reg32 view = rc::ld32(cpu.esp + 4);
        const reg32 offset = view - FirstFaceView;
        if (offset < FaceViewStride * FaceCount && offset % FaceViewStride == 0)
        {
            const uint32_t face = offset / FaceViewStride;
            if (face % uint32_t(interval) != g_presentedFrames % uint32_t(interval))
            {
                cpu.esp += 4;   // cdecl: pop the return address only
                return;
            }
        }
    }
    sub_72e460_guest(app, cpu);
}

void sub_46cec0(win32::WinApplication* app, x86::CPU& cpu)
{
    static pf::LazyBool s_native_setting;
    const bool s_native = s_native_setting.get("timer_native", true);
    if (!s_native)
        return sub_46cec0_guest(app, cpu);
    const int64_t now = int64_t(pf::ticksUs());
    const reg32 shift = rc::ld32(0x00a8534c);
    const uint32_t cl = shift & 0xff;
    const int64_t result = cl >= 64 ? (now < 0 ? -1 : 0) : (now >> cl);
    cpu.eax = reg32(uint64_t(result));
    cpu.edx = reg32(uint64_t(result) >> 32);
    cpu.ecx = cl >= 32 && cl < 64 ? ((shift & ~0xffu) | (cl & 0x1f)) : shift;
    cpu.esp += 4;
}

}

}

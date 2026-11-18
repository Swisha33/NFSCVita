// Native versions of the hottest EA audio kernels of NFSC.exe (generate.py NATIVE_OVERRIDES), after
// NFSUG2_VITA's audio_kernels.h. Run 71's audio-thread profile: 0x8AD305 15.5 %, 0x89F363 9.2 %, 0x8AD12E 3.8 %.
//
// 0x8AD305 mix    (dst, src, gain, n): dst[i] += src[i] * gain
// 0x8AD12E scale  (dst, src, gain, n): dst[i]  = src[i] * gain
//   Both take an SSE path ([0xBC6E88] set, all three 16-byte aligned) or an x87 one; single precision either way
//   in the result, so float NEON math matches the SSE path exactly and the x87 path to rounding.
// 0x89F363 filter (n, a, b, c, d, x0, in, buf, out1, out2, accumulate): per frame i, with x = x0 then y(i-1):
//   y = in[i] - x * a - b * buf[i + 1]          stored to out1[i] as float, the next x reads it back
//   z = (c * buf[i] + buf[i + 1]) * d           out2[i] = z, or out2[i] += z when accumulate
//   returns the last x in st0 (x0 when n <= 0). Computed in double like the translated x87 code.
//
// Setting audio_kernel_check=N: the first N calls of each kernel also run the translated body on a copy and log
// any difference (then restore the native result).

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <arm_neon.h>
#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <cstring>
#include <vector>

namespace nfsc
{

void sub_8ad305_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_8ad12e_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_89f363_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

inline reg32 arg(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

inline float argf(const x86::CPU& cpu, int n)
{
    return rc::ldf32(cpu.esp + 4 + 4 * n);
}

void mix(float* dst, const float* src, float gain, int32_t n)
{
    int32_t i = 0;
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vaddq_f32(vld1q_f32(dst + i), vmulq_f32(vld1q_f32(src + i), g)));
    for (; i < n; ++i)
        dst[i] = dst[i] + src[i] * gain;
}

void scale(float* dst, const float* src, float gain, int32_t n)
{
    int32_t i = 0;
    const float32x4_t g = vdupq_n_f32(gain);
    for (; i + 4 <= n; i += 4)
        vst1q_f32(dst + i, vmulq_f32(vld1q_f32(src + i), g));
    for (; i < n; ++i)
        dst[i] = src[i] * gain;
}

// The guest loops run while dst < dst + n * 4 (unsigned): n <= 0 does nothing. They use the pointers as given;
// the SSE path requires n to be a multiple of 16 floats, the x87 one of 4 (aligned case) or any n.
float* floats(reg32 address)
{
    return reinterpret_cast<float*>(rc::host(address));
}

int s_checks = -1;

int checksLeft()
{
    if (s_checks < 0)
        s_checks = std::max(0, pf::Config::getInt("audio_kernel_check", 0));
    return s_checks;
}

struct Region
{
    reg32 address;
    reg32 bytes;
};

// Runs the translated body on the current memory after the native one ran on a saved copy; compares, logs,
// and leaves the native result in place.
template <typename Native, typename Guest>
void checked(win32::WinApplication* app, x86::CPU& cpu, const char* name, std::initializer_list<Region> outputs,
             Native native, Guest guest, bool fpuResult)
{
    --s_checks;
    std::vector<std::vector<uint8_t>> before;
    for (const Region& r : outputs)
        before.emplace_back(rc::host(r.address), rc::host(r.address) + r.bytes);
    const reg32 depth = cpu.fpu.count;
    native(app, cpu);
    const double nativeResult = fpuResult ? cpu.fpu.pop() : 0.0;
    std::vector<std::vector<uint8_t>> mine;
    size_t k = 0;
    for (const Region& r : outputs)
    {
        mine.emplace_back(rc::host(r.address), rc::host(r.address) + r.bytes);
        memcpy(rc::host(r.address), before[k++].data(), r.bytes);
    }
    guest(app, cpu);
    const double guestResult = fpuResult ? cpu.fpu.pop() : 0.0;
    reg32 differing = 0;
    float worst = 0.0f;
    k = 0;
    for (const Region& r : outputs)
    {
        const float* g = floats(r.address);
        const float* m = reinterpret_cast<const float*>(mine[k].data());
        for (reg32 i = 0; i < r.bytes / 4; ++i)
        {
            if (memcmp(&g[i], &m[i], 4) != 0)
            {
                ++differing;
                worst = std::max(worst, std::fabs(g[i] - m[i]));
            }
        }
        memcpy(rc::host(r.address), mine[k++].data(), r.bytes);
    }
    // Registers stay as the translated body left them (valid guest state); the x87 stack gets the native result.
    cpu.fpu.count = depth;
    if (fpuResult)
        cpu.fpu.push(nativeResult);
    if (differing || nativeResult != guestResult)
        pf::logInfo("audio kernel %s: %u floats differ (largest gap %g), result %.9g vs %.9g (native vs guest)", name,
                    unsigned(differing), double(worst), nativeResult, guestResult);
    else if (s_checks == 0)
        pf::logInfo("audio kernel check: last check (%s) identical", name);
}

void mixNative(win32::WinApplication*, x86::CPU& cpu)
{
    const int32_t n = int32_t(arg(cpu, 3));
    if (n > 0)
        mix(floats(arg(cpu, 0)), floats(arg(cpu, 1)), argf(cpu, 2), n);
}

void scaleNative(win32::WinApplication*, x86::CPU& cpu)
{
    const int32_t n = int32_t(arg(cpu, 3));
    if (n > 0)
        scale(floats(arg(cpu, 0)), floats(arg(cpu, 1)), argf(cpu, 2), n);
}

void filterNative(win32::WinApplication*, x86::CPU& cpu)
{
    const int32_t n = int32_t(arg(cpu, 0));
    const double a = argf(cpu, 1), b = argf(cpu, 2), c = argf(cpu, 3), d = argf(cpu, 4);
    double x = argf(cpu, 5);
    const float* in = floats(arg(cpu, 6));
    const float* buf = floats(arg(cpu, 7));
    float* out1 = floats(arg(cpu, 8));
    float* out2 = floats(arg(cpu, 9));
    const bool accumulate = arg(cpu, 10) != 0;
    for (int32_t i = 0; i < n; ++i)
    {
        const double y = (double(in[i]) - x * a) - b * double(buf[i + 1]);
        out1[i] = float(y);
        const double z = (c * double(buf[i]) + double(buf[i + 1])) * d;
        out2[i] = accumulate ? float(z + double(out2[i])) : float(z);
        x = double(out1[i]);
    }
    cpu.fpu.push(x);
}

}

namespace native
{

void sub_8ad305(win32::WinApplication* app, x86::CPU& cpu)
{
    if (checksLeft() > 0)
    {
        const int32_t n = int32_t(arg(cpu, 3));
        checked(app, cpu, "mix 0x8ad305", { { arg(cpu, 0), reg32(std::max(n, 0)) * 4 } }, mixNative,
                sub_8ad305_guest, false);
        return;
    }
    mixNative(app, cpu);
    cpu.esp += 4;           // as the guest's ret (see native_math.cpp popReturn)
}

void sub_8ad12e(win32::WinApplication* app, x86::CPU& cpu)
{
    if (checksLeft() > 0)
    {
        const int32_t n = int32_t(arg(cpu, 3));
        checked(app, cpu, "scale 0x8ad12e", { { arg(cpu, 0), reg32(std::max(n, 0)) * 4 } }, scaleNative,
                sub_8ad12e_guest, false);
        return;
    }
    scaleNative(app, cpu);
    cpu.esp += 4;
}

void sub_89f363(win32::WinApplication* app, x86::CPU& cpu)
{
    if (checksLeft() > 0)
    {
        const reg32 bytes = reg32(std::max(int32_t(arg(cpu, 0)), 0)) * 4;
        checked(app, cpu, "filter 0x89f363", { { arg(cpu, 8), bytes }, { arg(cpu, 9), bytes } }, filterNative,
                sub_89f363_guest, true);
        return;
    }
    filterNative(app, cpu);
    cpu.esp += 4;
}

}

}

// Native versions of hot x87 leaf functions of NFSC.exe (generate.py NATIVE_OVERRIDES), after NFSUG2_VITA's
// src/ug2/native.cpp (_ftol2) and its hot-function conversions. Run 151's game-thread profile: 0071b630 1.9 %,
// 0087eac0 1.4 %, 0046fcc0 0.7 %, 0070f0b0 0.7 %.
//
// The translated x87 code keeps the stack in double and rounds only at fst/fstp dword; these do the same in the same
// order, so the results are bit-identical to the translated bodies (setting native_check=N compares the first N calls
// of each against them and logs any difference).
//
// 0087eac0 _ftol2: st0 (popped) to edx:eax, truncated toward zero.
// 0070f0b0 box transform (cdecl; matrix, min*, max*): Arvo's method. Per axis the translation (row 3), then for
//   rows i = 0..2 and column j: a = min[i] * m[i][j] (double), b = float(max[i] * m[i][j]); if a < b the minimum
//   gets a and the maximum b, else the minimum b and the maximum a. min and max are written back as floats.
// 0071b630 box against the view's 6 planes (thiscall; min*, max*, matrix or 0): returns 0 outside, 1 crossing,
//   2 inside. The planes are 16 bytes each from [this] + 0x140 (x, y, z, w).
// 0046fcc0 square overlap (cdecl; p*, e*, q*, r float): 0 when p.x - r > q.x, r + e.x < q.x, p.y - r > q.y or
//   r + e.y < q.y, else 1 (NaN compares as the x87 test does: never rejects).

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace nfsc
{

void sub_87eac0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_70f0b0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_71b630_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_46fcc0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_79f9c0_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

inline reg32 arg(const x86::CPU& cpu, int n)
{
    return rc::ld32(cpu.esp + 4 + 4 * n);
}

inline double ldf(reg32 address)
{
    return double(rc::ldf32(address));
}

// A native function leaves cpu.esp as the guest's ret would (return address plus the callee-popped bytes):
// address-taken guest functions write cpu.esp on return, and a caller that reached them through dynamic_call takes
// esp from it. Without this, _ftol2 reached indirectly returned with esp 4 bytes low; a caller further up popped
// the wrong saved edi and the stream library 0x8d14a0 crashed with edi = a heap pointer (runs 158-161, the same
// dump four times; native_math=off ran clean in run 162).
inline void popReturn(x86::CPU& cpu, reg32 popped)
{
    cpu.esp += 4 + popped;
}

// Each override has its own budget of native_check calls (a shared one went almost all to _ftol2).
// Setting native_math=off runs the translated bodies instead of the native versions (to rule them out).
bool nativeOff()
{
    static const bool off = !pf::Config::getBool("native_math", true);
    return off;
}

int checksLeft(int& left)
{
    if (left < 0)
        left = std::max(0, pf::Config::getInt("native_check", 0));
    return left;
}

struct Region
{
    reg32 address = 0;
    reg32 bytes = 0;        // up to 16
};

// Runs native, then the translated body on the same inputs, and compares eax/edx, the x87 depth and top, and the
// bytes of up to two memory regions; the native result stays.
template <typename Native, typename Guest>
void checked(win32::WinApplication* app, x86::CPU& cpu, int& left, reg32 popped, const char* name, Native native,
             Guest guest, Region r0 = Region(), Region r1 = Region())
{
    --left;
    const reg32 esp = cpu.esp;
    Region regions[2] = { r0, r1 };
    uint8_t before[2][16], mine[2][16];
    for (int k = 0; k < 2; ++k)
        if (regions[k].bytes)
            memcpy(before[k], rc::host(regions[k].address), regions[k].bytes);
    const reg32 count = cpu.fpu.count, ecx = cpu.ecx;
    double regs[8];
    memcpy(regs, cpu.fpu.regs, sizeof(regs));
    native(app, cpu);
    const reg32 eax = cpu.eax, edx = cpu.edx, nativeCount = cpu.fpu.count;
    double nativeRegs[8];
    memcpy(nativeRegs, cpu.fpu.regs, sizeof(nativeRegs));
    for (int k = 0; k < 2; ++k)
        if (regions[k].bytes)
            memcpy(mine[k], rc::host(regions[k].address), regions[k].bytes);
    for (int k = 1; k >= 0; --k)
        if (regions[k].bytes)
            memcpy(rc::host(regions[k].address), before[k], regions[k].bytes);
    cpu.fpu.count = count;
    memcpy(cpu.fpu.regs, regs, sizeof(regs));
    cpu.ecx = ecx;
    cpu.esp = esp;
    guest(app, cpu);
    // edx and esp are compared only for _ftol2: it returns a 64-bit result and is address-taken, so its body leaves
    // cpu.esp as its ret does. The others return in eax, every caller overwrites edx before reading it or returns
    // (work/edx_after.py over all call sites, run 158), and their bodies leave cpu.esp at an inner call's value.
    const bool edxResult = name[0] == '_';
    const bool espSame = !edxResult || cpu.esp == esp + 4 + popped;
    cpu.esp = esp;
    bool same = espSame && cpu.eax == eax && (!edxResult || cpu.edx == edx) && cpu.fpu.count == nativeCount
             && (nativeCount == 0 || memcmp(&nativeRegs[nativeCount & 7], &cpu.fpu.regs[nativeCount & 7], 8) == 0);
    for (int k = 0; k < 2; ++k)
        if (regions[k].bytes && memcmp(mine[k], rc::host(regions[k].address), regions[k].bytes) != 0)
            same = false;
    if (!same)
        pf::logError("native %s differs: eax 0x%08x/0x%08x edx 0x%08x/0x%08x depth %u/%u (native/guest)", name,
                     unsigned(eax), unsigned(cpu.eax), unsigned(edx), unsigned(cpu.edx), unsigned(nativeCount),
                     unsigned(cpu.fpu.count));
    else if (left == 0)
        pf::logInfo("native check: %s: last of %d checks identical", name, pf::Config::getInt("native_check", 0));
    cpu.eax = eax;
    cpu.edx = edx;
    cpu.fpu.count = nativeCount;
    memcpy(cpu.fpu.regs, nativeRegs, sizeof(nativeRegs));
    for (int k = 0; k < 2; ++k)
        if (regions[k].bytes)
            memcpy(rc::host(regions[k].address), mine[k], regions[k].bytes);
}

// 0079f9c0 scenery size (cdecl; info*, position*, radius float, distance* out): the on-screen size the scenery
// renderer 0079fa60 (its only caller, 0079fb30) uses to skip an instance (<= 1) or draw its low-detail model (< 32):
// 0 when the centre is more than radius behind the view (info+0x30 forward, info+0x20 eye); else *distance = |p - eye|
// and the size is trunc(info+0x40 when distance - radius <= radius, else radius / (distance - radius) * info+0x40)
// (info+0x40: the view's pixel scale). Setting scenery_scale (default 0.5; 1 = the game's sizes) multiplies the size
// before the truncation, so small scenery drops out and the low-detail models take over earlier (as NFSUG2_VITA's
// world_min_pixels / world_lod_pixels, here through Carbon's own thresholds). AI-drive benchmark: 1.0 18.6 fps (native
// identical to the guest in 300 checks), 0.5 19.9 (603 draws vs 681), 0.35 19.5 (runs 223-225); no visible gaps.
float sceneryScale()
{
    static const float scale = [] {
        const char* text = pf::Config::getString("scenery_scale", "0.5");
        const float v = float(atof(text));
        return v > 0.0f && v <= 4.0f ? v : 1.0f;
    }();
    return scale;
}

void sizeNative(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 info = arg(cpu, 0), pos = arg(cpu, 1), out = arg(cpu, 3);
    const double r = ldf(cpu.esp + 12);
    const double dx = ldf(pos) - ldf(info + 0x20);
    const double dy = ldf(pos + 4) - ldf(info + 0x24);
    const double dz = ldf(pos + 8) - ldf(info + 0x28);
    const double dot = (dz * ldf(info + 0x38) + dy * ldf(info + 0x34)) + dx * ldf(info + 0x30);
    if (-r > dot)
    {
        cpu.edx_eax = 0;
        return;
    }
    const double dist = std::sqrt((dx * dx + dz * dz) + dy * dy);
    rc::stf32(out, float(dist));
    const float t = float(dist - r);
    const double scale = ldf(info + 0x40);
    double size = double(t) > r ? r / double(t) * scale : scale;
    const float extra = sceneryScale();
    if (extra != 1.0f)
        size *= extra;
    cpu.edx_eax = x86::reg64(x86::truncInteger64(size));
}

void ftolNative(win32::WinApplication*, x86::CPU& cpu)
{
    cpu.edx_eax = x86::reg64(x86::truncInteger64(cpu.fpu.pop()));
}

// Box (min, max as host floats, updated in place) by the guest matrix at m.
void transformBox(reg32 m, float* lo, float* hi)
{
    double minX = ldf(m + 0x30), minY = ldf(m + 0x34), minZ = ldf(m + 0x38);
    double maxX = minX, maxY = minY, maxZ = minZ;
    for (int i = 0; i < 3; ++i)
    {
        const reg32 row = m + 16 * i;
        const double l = lo[i], h = hi[i];
        double* mins[3] = { &minX, &minY, &minZ };
        double* maxs[3] = { &maxX, &maxY, &maxZ };
        for (int j = 0; j < 3; ++j)
        {
            const double e = ldf(row + 4 * j);
            const double a = l * e;
            const double b = double(float(h * e));
            if (a < b)
            {
                *mins[j] += a;
                *maxs[j] += b;
            }
            else
            {
                *mins[j] += b;
                *maxs[j] += a;
            }
        }
    }
    lo[0] = float(minX); lo[1] = float(minY); lo[2] = float(minZ);
    hi[0] = float(maxX); hi[1] = float(maxY); hi[2] = float(maxZ);
}

void transformNative(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 m = arg(cpu, 0), lo = arg(cpu, 1), hi = arg(cpu, 2);
    float l[3], h[3];
    memcpy(l, rc::host(lo), 12);
    memcpy(h, rc::host(hi), 12);
    transformBox(m, l, h);
    memcpy(rc::host(lo), l, 12);
    memcpy(rc::host(hi), h, 12);
}

void frustumNative(win32::WinApplication*, x86::CPU& cpu)
{
    float lo[3], hi[3];
    memcpy(lo, rc::host(arg(cpu, 0)), 12);
    memcpy(hi, rc::host(arg(cpu, 1)), 12);
    if (const reg32 m = arg(cpu, 2))
        transformBox(m, lo, hi);
    const double half = ldf(0x009c2888), zero = ldf(0x009c248c);
    float c[3], e[3];
    for (int k = 0; k < 3; ++k)
        c[k] = float((double(hi[k]) + double(lo[k])) * half);
    for (int k = 0; k < 3; ++k)
        e[k] = float(double(hi[k]) - double(c[k]));
    reg32 plane = rc::ld32(cpu.ecx) + 0x140;
    bool crossing = false;
    for (int k = 0; k < 6; ++k, plane += 16)
    {
        const double nx = ldf(plane), ny = ldf(plane + 4), nz = ldf(plane + 8), w = ldf(plane + 12);
        const double r = (std::fabs(ny) * double(e[1]) + std::fabs(nz) * double(e[2])) + std::fabs(nx) * double(e[0]);
        const double d = ((double(c[1]) * ny + double(c[2]) * nz) + double(c[0]) * nx) + w;
        if (d + r < zero)
        {
            cpu.eax = 0;
            return;
        }
        if (d - r < zero)
            crossing = true;
    }
    cpu.eax = crossing ? 1 : 2;
}

void overlapNative(win32::WinApplication*, x86::CPU& cpu)
{
    const reg32 p = arg(cpu, 0), e = arg(cpu, 1), q = arg(cpu, 2);
    const double r = double(rc::ldf32(cpu.esp + 16));
    const bool apart = ldf(p) - r > ldf(q) || r + ldf(e) < ldf(q) || ldf(p + 4) - r > ldf(q + 4)
                    || r + ldf(e + 4) < ldf(q + 4);
    cpu.eax = apart ? 0 : 1;
}

}

namespace native
{

void sub_87eac0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (nativeOff())
        return sub_87eac0_guest(app, cpu);
    static int checks = -1;
    if (checksLeft(checks) > 0)
        checked(app, cpu, checks, 0, "_ftol2 0x87eac0", ftolNative, sub_87eac0_guest);
    else
        ftolNative(app, cpu);
    popReturn(cpu, 0);
}

void sub_70f0b0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (nativeOff())
        return sub_70f0b0_guest(app, cpu);
    static int checks = -1;
    if (checksLeft(checks) > 0)
    {
        Region lo, hi;
        lo.address = arg(cpu, 1);
        hi.address = arg(cpu, 2);
        lo.bytes = hi.bytes = 12;
        checked(app, cpu, checks, 0, "box transform 0x70f0b0", transformNative, sub_70f0b0_guest, lo, hi);
    }
    else
        transformNative(app, cpu);
    popReturn(cpu, 0);
}

void sub_71b630(win32::WinApplication* app, x86::CPU& cpu)
{
    if (nativeOff())
        return sub_71b630_guest(app, cpu);
    static int checks = -1;
    if (checksLeft(checks) > 0)
        checked(app, cpu, checks, 12, "frustum test 0x71b630", frustumNative, sub_71b630_guest);
    else
        frustumNative(app, cpu);
    popReturn(cpu, 12);     // ret 0xc
}

void sub_79f9c0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (nativeOff())
        return sub_79f9c0_guest(app, cpu);
    static int checks = -1;
    if (checksLeft(checks) > 0 && sceneryScale() == 1.0f)
    {
        Region distance;
        distance.address = arg(cpu, 3);
        distance.bytes = 4;
        checked(app, cpu, checks, 0, "scenery size 0x79f9c0", sizeNative, sub_79f9c0_guest, distance);
    }
    else
        sizeNative(app, cpu);
    popReturn(cpu, 0);
}

void sub_46fcc0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (nativeOff())
        return sub_46fcc0_guest(app, cpu);
    static int checks = -1;
    if (checksLeft(checks) > 0)
        checked(app, cpu, checks, 0, "overlap test 0x46fcc0", overlapNative, sub_46fcc0_guest);
    else
        overlapNative(app, cpu);
    popReturn(cpu, 0);
}

}

}

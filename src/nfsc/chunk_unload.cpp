// Native version of the game's chunk unloader 006a83a0 (generate.py NATIVE_OVERRIDES).
//
// 006a83a0 (cdecl; start, size, unused) unloads the chunks of a resource block last to first. Chunks only link
// forward (id, size, data; the next one at +size+8), so the game walks the block from the start, keeps the last 64
// chunks in a ring on its stack, unloads those 64 from the last one back, then walks again from the start up to the
// lowest chunk it unloaded. A block of n chunks costs about n * n / 128 header reads. On a PC that is nothing; on
// the Vita each read misses the cache somewhere in a section of several MB: dropping a big track section froze the
// game for 20 s to over 2 minutes with the execution lock held (runs 281-286; run 286's profile of the stalled time:
// 93 % in this function's own walk, not in the unload callbacks).
//
// This walks the block about twice (checkpoints, then segment by segment) and unloads the chunks in the same order
// (last to first), with the
// same calls per chunk: 0069a170 (the chunk id being handled), 0046cec0 (timer), 0046d700 (the loader of the id),
// the loader's unload callback at +0xc, and if no loader is found the fallback callbacks 00a62bd8..00a62c34. A
// callback that returns 0 hits the game's assert (a write to address 0), as in the game. The start (00732840) and
// the end (0046cec0 and 00557f30 unless byte 00a995f8 is set) are kept too.
//
// The game re-reads each earlier chunk's size on every pass; the single walk assumes the unload callbacks leave the
// headers of the chunks before them alone. Each chunk's size is checked again right before it is unloaded and a
// difference is logged. Setting fast_chunk_unload=off runs the translated body instead.

#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>


namespace nfsc
{

void sub_6a83a0_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_732840(win32::WinApplication* app, x86::CPU& cpu);
void sub_69a170(win32::WinApplication* app, x86::CPU& cpu);
void sub_46cec0(win32::WinApplication* app, x86::CPU& cpu);
void sub_46d700(win32::WinApplication* app, x86::CPU& cpu);
void sub_557f30(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;
using Function = void (*)(win32::WinApplication*, x86::CPU&);

const reg32 FallbackFirst = 0x00a62bd8, FallbackEnd = 0x00a62c34;
const reg32 SkipTimerFlag = 0x00a995f8;

// A direct call as the translated code makes it: arguments, then a return address slot the callee pops. Only the
// guest esp is put back (cdecl: the caller pops the arguments); eax comes back.
reg32 callDirect(win32::WinApplication* app, x86::CPU& cpu, Function function, const reg32* args = nullptr,
                 int count = 0)
{
    const reg32 esp = cpu.esp;
    for (int i = count - 1; i >= 0; --i)
        rc::st32(cpu.esp -= 4, args[i]);
    cpu.esp -= 4;
    function(app, cpu);
    cpu.esp = esp;
    return cpu.eax;
}

// An indirect call (a loader's or a fallback unload callback, cdecl with the chunk).
reg32 callPointer(win32::WinApplication* app, x86::CPU& cpu, reg32 function, reg32 chunk)
{
    const reg32 esp = cpu.esp;
    rc::st32(cpu.esp -= 4, chunk);
    ::native::callGuest(app, cpu, function);
    cpu.esp = esp;
    return cpu.eax;
}

bool fastOff()
{
    static const bool off = !pf::Config::getBool("fast_chunk_unload", true);
    return off;
}

struct Chunk
{
    reg32 address;
    reg32 size;
};

const uint32_t Checkpoints = 4096;      // every stride-th chunk of the block
const uint32_t SegmentMax = 4096;       // the longest stride: blocks of up to 4096 * 4096 chunks

}

namespace native
{

void sub_6a83a0(win32::WinApplication* app, x86::CPU& cpu)
{
    if (fastOff())
    {
        sub_6a83a0_guest(app, cpu);
        return;
    }
    const reg32 entryEsp = cpu.esp;
    const reg32 start = rc::ld32(entryEsp + 4);
    const reg32 size = rc::ld32(entryEsp + 8);
    const reg32 ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi, ebp = cpu.ebp;
    // The game's frame: 0x100 bytes of ring and edi below the return address; callees run under it.
    cpu.esp = entryEsp - 0x104;

    callDirect(app, cpu, sub_732840);
    if (cpu.terminate)
        return;

    // No heap: the newlib heap has a few MB at most, and a section holds hundreds of thousands of chunks (run 288:
    // a std::vector of them ended in std::bad_alloc). One walk keeps every stride-th chunk as a checkpoint; then
    // each segment, last to first, is read again into a fixed buffer and unloaded backwards: ~2 header reads per
    // chunk. Only the game's main thread unloads chunks, so the static tables are not shared.
    const reg32 end = start + size;
    static reg32 s_checkpoints[Checkpoints];
    static Chunk s_segment[SegmentMax];
    uint32_t stride = 64, count = 0, chunks = 0;
    for (reg32 p = start; p < end; p = p + rc::ld32(p + 4) + 8, ++chunks)
    {
        if (chunks % stride)
            continue;
        if (count == Checkpoints)
        {
            // Full: keep every other checkpoint and double the stride.
            for (uint32_t k = 0; k < Checkpoints / 2; ++k)
                s_checkpoints[k] = s_checkpoints[2 * k];
            count = Checkpoints / 2;
            stride *= 2;
            if (chunks % stride)
                continue;
        }
        s_checkpoints[count++] = p;
    }
    if (stride > SegmentMax)
    {
        // Over 4096 * 4096 chunks: no section is that big; leave it to the game's own loop.
        pf::logError("chunks: %u chunks at 0x%08x, more than the native unloader handles", unsigned(chunks),
                     unsigned(start));
        cpu.esp = entryEsp;
        cpu.ebx = ebx, cpu.esi = esi, cpu.edi = edi, cpu.ebp = ebp;
        sub_6a83a0_guest(app, cpu);
        return;
    }

    static unsigned s_changed = 0;
    for (uint32_t segment = count; segment-- > 0;)
    {
        const reg32 to = segment + 1 < count ? s_checkpoints[segment + 1] : end;
        uint32_t n = 0;
        for (reg32 p = s_checkpoints[segment]; p < to && n < SegmentMax; p = p + rc::ld32(p + 4) + 8)
            s_segment[n++] = {p, rc::ld32(p + 4)};
        for (uint32_t i = n; i-- > 0;)
        {
            const reg32 chunk = s_segment[i].address;
            if (rc::ld32(chunk + 4) != s_segment[i].size && s_changed < 8)
            {
                ++s_changed;
                pf::logError("chunks: unloading 0x%08x: its size changed from %u to %u during the unload (the game's "
                             "own walk would have read the new one)", unsigned(chunk), unsigned(s_segment[i].size),
                             unsigned(rc::ld32(chunk + 4)));
            }
            const reg32 id = rc::ld32(chunk);
            callDirect(app, cpu, sub_69a170, &id, 1);
            if (cpu.terminate)
                return;
            callDirect(app, cpu, sub_46cec0);
            if (cpu.terminate)
                return;
            const reg32 current = rc::ld32(chunk);
            if (!current)
                continue;
            const reg32 loader = callDirect(app, cpu, sub_46d700, &current, 1);
            if (cpu.terminate)
                return;
            bool done;
            if (loader)
            {
                done = callPointer(app, cpu, rc::ld32(loader + 0xc), chunk) != 0;
            }
            else
            {
                done = false;
                for (reg32 slot = FallbackFirst; slot < FallbackEnd && !done; slot += 4)
                {
                    done = callPointer(app, cpu, rc::ld32(slot), chunk) != 0;
                    if (cpu.terminate)
                        return;
                }
            }
            if (cpu.terminate)
                return;
            if (!done)
                rc::st32(0, 0);     // the game's assert: no loader unloaded this chunk
        }
    }

    const uint8_t skip = rc::ld8(SkipTimerFlag);
    if (!skip)
    {
        callDirect(app, cpu, sub_46cec0);
        if (cpu.terminate)
            return;
        callDirect(app, cpu, sub_557f30);
        if (cpu.terminate)
            return;
    }
    cpu.eax = (cpu.eax & 0xffffff00u) | skip;
    cpu.ebx = ebx;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.ebp = ebp;
    cpu.esp = entryEsp + 4;     // ret: the return address
}

}

}

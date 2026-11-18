// Texture-pack chunks (0xB3300000 container: 0xB3310000 pack header, 0xB3320000 pack data) and the pending list at
// 0xA94210 (run 74's crash).
//
// The chunk loader 0x560580 links each data chunk of a container into a circular list (head 0xA94210: node +0 next,
// +4 prev; the node is the data chunk + 0x10, its +0xC the pack hash) and resolves one entry whose hash matches the
// header ([header + 0x60]), unlinking it. The unloader 0x560A90 releases the headers but never unlinks entries still
// pending, so after a pack is unloaded (load transitions: run 74 crashed ~20 s after a device reset with all effects
// recreated) the list points into freed memory; once that is reused, the walk at 0x560617 reads garbage
// ([esi + 0xC] with esi = 0xb8d52b05 -> C2-12828-1).
//
// Both functions are reached only through the chunk-handler table (registered by 0x4735D0), i.e. through
// dynamic_call, so they are wrapped with replaceMethod:
//  - the unloader first unlinks the pending entries inside the container being unloaded;
//  - the loader first drops entries whose chunk id (node - 0x10) is no longer 0xB3320000 (a safety net), rebuilding
//    the list from the valid nodes found walking forward and backward.
// Each repair is logged.

#include <lib/memmap.h>
#include <lib/winapp.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <vector>

namespace nfsc
{

namespace
{

using x86::reg32;

const reg32 PendingHead   = 0x00a94210;
const reg32 LoadFunction  = 0x00560580;
const reg32 UnloadFunction = 0x00560a90;
const reg32 PackContainer = 0xb3300000;
const reg32 PackData      = 0xb3320000;
const reg32 MaxNodes      = 100000;

win32::MethodPtr s_load = nullptr;
win32::MethodPtr s_unload = nullptr;
unsigned s_unlinked = 0;
unsigned s_dropped = 0;

bool plausible(reg32 node)
{
    return node == PendingHead
        || (node >= 0x1000 && (node & 3) == 0 && win32::MemMap::isGuestRange(node - 0x10, 0x20)
            && rc::ld32(node - 0x10) == PackData);
}

void unlink(reg32 node)
{
    const reg32 next = rc::ld32(node);
    const reg32 prev = rc::ld32(node + 4);
    rc::st32(prev, next);
    rc::st32(next + 4, prev);
}

// Drops nodes that no longer sit in a pack data chunk. Returns how many were dropped.
unsigned repairList()
{
    std::vector<reg32> forward;
    reg32 node = rc::ld32(PendingHead);
    bool broken = false;
    while (node != PendingHead && forward.size() < MaxNodes)
    {
        if (!plausible(node))
        {
            broken = true;
            break;
        }
        forward.push_back(node);
        node = rc::ld32(node);
    }
    if (!broken)
        return 0;
    const reg32 bad = node;
    std::vector<reg32> backward;
    node = rc::ld32(PendingHead + 4);
    while (node != PendingHead && backward.size() < MaxNodes && plausible(node))
    {
        if (!forward.empty() && node == forward.back())
            break;
        backward.push_back(node);
        node = rc::ld32(node + 4);
    }
    std::vector<reg32> kept(forward);
    for (auto it = backward.rbegin(); it != backward.rend(); ++it)
    {
        bool seen = false;
        for (reg32 k : kept)
            seen = seen || k == *it;
        if (!seen)
            kept.push_back(*it);
    }
    reg32 prev = PendingHead;
    for (reg32 k : kept)
    {
        rc::st32(prev, k);
        rc::st32(k + 4, prev);
        prev = k;
    }
    rc::st32(prev, PendingHead);
    rc::st32(PendingHead + 4, prev);
    ++s_dropped;
    pf::logError("texture packs: pending list 0x%08x had a stale node 0x%08x (chunk id 0x%08x); kept %u nodes "
                 "(%u forward, %u backward), repair %u", unsigned(PendingHead), unsigned(bad),
                 win32::MemMap::isGuestRange(bad - 0x10, 4) ? unsigned(rc::ld32(bad - 0x10)) : 0u,
                 unsigned(kept.size()), unsigned(forward.size()), unsigned(backward.size()), s_dropped);
    return 1;
}

void loadPack(win32::WinApplication* app, x86::CPU& cpu)
{
    repairList();
    s_load(app, cpu);
}

void unloadPack(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 chunk = rc::ld32(cpu.esp + 4);
    // Containers hold pending data nested in them (0x5605BB); a top-level data chunk is linked itself (0x55CAA0)
    // and its unloader (0x560AD4) returns 1 without unlinking either (run 75: stale nodes during the first load).
    const reg32 id = chunk && win32::MemMap::isGuestRange(chunk, 8) ? rc::ld32(chunk) : 0;
    if (id == PackContainer || id == PackData)
    {
        repairList();
        const reg32 begin = chunk;
        const reg32 end = chunk + 8 + rc::ld32(chunk + 4);
        unsigned here = 0;
        reg32 node = rc::ld32(PendingHead);
        for (reg32 n = 0; node != PendingHead && n < MaxNodes; ++n)
        {
            const reg32 next = rc::ld32(node);
            if (node >= begin && node < end)
            {
                unlink(node);
                ++here;
            }
            node = next;
        }
        if (here)
        {
            s_unlinked += here;
            pf::logInfo("texture packs: unloading %s chunk 0x%08x-0x%08x unlinked %u pending data entries "
                        "(%u in all)", id == PackContainer ? "pack" : "pack data", unsigned(begin), unsigned(end),
                        here, s_unlinked);
        }
    }
    s_unload(app, cpu);
}

}

void installTexturePackFix(win32::WinApplication& app)
{
    s_load = app.replaceMethod(LoadFunction, &loadPack);
    s_unload = app.replaceMethod(UnloadFunction, &unloadPack);
    if (!s_load || !s_unload)
        pf::logError("texture packs: chunk handlers 0x%08x / 0x%08x not registered", unsigned(LoadFunction),
                     unsigned(UnloadFunction));
}

}

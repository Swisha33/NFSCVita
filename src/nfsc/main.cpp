// NFSCVita entry point: loads the user's NFSC.exe (Need for Speed Carbon PC v1.4), maps its sections into the guest
// arena and runs the recompiled code (src/nfsc/generated, tools/generate.ps1).
// Derived from UG1VITA's src/nfsu/main.cpp (MIT). Bring-up stage: no game-specific hooks yet.

#include <lib/file.h>
#include <lib/memmap.h>
#include <lib/registry.h>
#include <lib/scheduler.h>
#include <platform/config.h>
#include <platform/cores.h>
#include <platform/gxm.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/pgo.h>
#include <platform/sha256.h>
#include <platform/sampler.h>
#include <platform/shadercomp.h>
#include <fpu.h>
#include <mmx.h>
#include <nfsc.h>
#include "bankcode.h"
#include <nfsc_layout.h>
#include <winapi/d3d9_native.h>
#include <winapi/kernel32.h>
#include <winapi/msvcr71.h>
#include <winapi/winmm.h>
#include <cstdlib>
#include <string>
#include <utility>
#include <vector>
#include <psp2/ctrl.h>
#include <psp2/power.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <sys/stat.h>
#include <malloc.h>
#include <algorithm>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <vector>

// Process parameters picked up by the VitaSDK runtime.
extern "C" {
// Run 40: 19.4 MB in use at the career load; runs 253/254: 22976 KB taken at most. 36 -> 30 MB gives the guest
// arena 6 MB more (it takes the free user memory at startup less a reserve) for the city section's reload.
int          _newlib_heap_size_user      = 30 * 1024 * 1024;
unsigned int sceUserMainThreadStackSize  = 4 * 1024 * 1024;
}

// The data folder is the game folder and the guest's C:\ drive: NFSC.exe and the game's data folders lie in it beside
// nfscvita.cfg, nfscvita.log and registry.txt.
static std::string s_dataDirectory = "ux0:data/NFSCVITA";
static const char* const s_exeName = "NFSC.exe";

static std::string dataPath(const char* name)
{
    return s_dataDirectory + "/" + name;
}

// Guest view of the executable and its folder.
static const char* s_guestGameFolder = "C:\\";
static const char* s_guestExePath = "C:\\NFSC.exe";
static const char* s_commandLine = "\"C:\\NFSC.exe\"";

static bool loadExecutable(std::vector<uint8_t>& image)
{
    const std::string exePath = dataPath(s_exeName);
    FILE* file = fopen(exePath.c_str(), "rb");
    if (!file)
    {
        pf::logError("%s not found: copy the game folder's contents to %s/", exePath.c_str(), s_dataDirectory.c_str());
        return false;
    }
    fseek(file, 0, SEEK_END);
    const long size = ftell(file);
    fseek(file, 0, SEEK_SET);
    image.resize(size > 0 ? size_t(size) : 0);
    const size_t read = image.empty() ? 0 : fread(image.data(), 1, image.size(), file);
    fclose(file);
    if (read != image.size() || image.empty())
    {
        pf::logError("%s: reading %u bytes failed", exePath.c_str(), unsigned(image.size()));
        return false;
    }
    const std::string hash = pf::sha256Hex(image.data(), image.size());
    if (hash != nfsc::layout::exeSha256)
    {
        pf::logError("%s: SHA-256 %s, expected %s (Carbon PC v1.4, unwrapped NFSC.exe)", exePath.c_str(), hash.c_str(),
                     nfsc::layout::exeSha256);
        return false;
    }
    return true;
}

static uint32_t imageWord(const std::vector<uint8_t>& image, size_t offset, size_t bytes)
{
    uint32_t value = 0;
    for (size_t i = 0; i < bytes && offset + i < image.size(); ++i)
        value |= uint32_t(image[offset + i]) << (8 * i);
    return value;
}

// The headers and the raw data of the sections at their guest addresses, each followed by an entry without data that
// covers its whole virtual size (the arena is zeroed, so .data's uninitialised part reads as zero).
static bool imageSections(const std::vector<uint8_t>& image, std::vector<win32::Section>& sections)
{
    const size_t pe = imageWord(image, 0x3c, 4);
    const size_t count = imageWord(image, pe + 6, 2);
    const size_t table = pe + 24 + imageWord(image, pe + 20, 2);
    sections.push_back(win32::Section{ image.data(), nfsc::layout::imageBase,
                                       x86::reg32(std::min<size_t>(nfsc::layout::headerSize, image.size())) });
    for (const nfsc::layout::ImageSection& s : nfsc::layout::sections)
    {
        size_t entry = table;
        while (entry < table + count * 40 && imageWord(image, entry + 12, 4) != s.address - nfsc::layout::imageBase)
            entry += 40;
        if (entry >= table + count * 40)
        {
            pf::logError("%s has no section at 0x%08x (%s)", s_exeName, s.address, s.name);
            return false;
        }
        const size_t rawOffset = imageWord(image, entry + 20, 4);
        const size_t rawSize = imageWord(image, entry + 16, 4);
        const x86::reg32 size = std::max(s.virtualSize, s.rawSize);
        if (rawOffset < image.size() && rawSize)
        {
            const size_t bytes = std::min<size_t>({ rawSize, size, image.size() - rawOffset });
            sections.push_back(win32::Section{ image.data() + rawOffset, s.address, x86::reg32(bytes) });
        }
        sections.push_back(win32::Section{ nullptr, s.address, size });
    }
    return true;
}

// A silent end (no crash dump, no log line) is abort() or exit(): an uncaught exception such as std::bad_alloc when
// the newlib heap is full ends in abort. These log it, with the heap use.
namespace nfsc
{
void installTexturePackFix(win32::WinApplication& app);
void installUnlockedAudio(win32::WinApplication& app);
void installWidescreen(win32::WinApplication& app);
void installPad();
void padPresent(win32::WinApplication* app, x86::CPU& cpu);
}

static void logHeap(const char* why)
{
    const struct mallinfo info = mallinfo();
    pf::logError("%s (newlib heap: %u KB in use, %u KB free in the arena of %u KB)", why, unsigned(info.uordblks / 1024),
                  unsigned(info.fordblks / 1024), unsigned(info.arena / 1024));
}

static void onTerminate()
{
    logHeap("std::terminate (an uncaught exception from the C++ library, std::bad_alloc when out of heap)");
    sceKernelExitProcess(1);
    for (;;) {}
}

static void onAbort(int)
{
    logHeap("abort()");
    sceKernelExitProcess(1);
}

static void onExit()
{
    logHeap("exit() called");
}

namespace
{

// Setting ai_drive (from the Mac port's auto_drive, mods/core/nfsc/extras.h): the game's AI drives the player's car,
// so test runs follow the same route and frame rates can be compared. Game_ForceAIControl 0065c2c0 (cdecl): its
// argument picks the player (0 the first of the list [0xa9ff5c] / count [0xa9ff64], else the last), it finds the
// vehicle's AI control interface (0060cb50 with 0x4087c0) and switches AI on (+0xc(1)) unless +0x8 says it is on.
// Called every 30 frames while the game state [0xa99bbc] is 6 (driving): in the career the state stays 6 through the
// prologue's scenes and restarts, and the player's car is replaced or handed back to the pad there, so the single
// call at the start (run 250: once, during the intro scene) left the car standing (BUSTED loop).
bool s_aiDrive = false;

void aiDrivePresent(win32::WinApplication* app, x86::CPU& cpu)
{
    static uint32_t s_frames = 0, s_calls = 0;
    if (app->getMemory<x86::reg32>(0x00a99bbc) != 6)
    {
        s_frames = 0;
        return;
    }
    if (s_frames++ % 30 != 0)
        return;
    // cdecl call with one argument: a return-address slot, then the argument; the callee pops the slot.
    const x86::reg32 eax = cpu.eax, ecx = cpu.ecx, edx = cpu.edx;
    cpu.esp -= 8;
    app->getMemory<x86::reg32>(cpu.esp + 0) = 0x0065c2c0;
    app->getMemory<x86::reg32>(cpu.esp + 4) = 1;
    app->dynamic_call(0x0065c2c0, cpu);
    cpu.esp += 4;
    cpu.eax = eax;
    cpu.ecx = ecx;
    cpu.edx = edx;
    if (s_calls++ == 0)
        pf::logInfo("ai_drive: Game_ForceAIControl(1) called (again every 30 frames while driving)");
}

// Setting debug_poke (development): guest dwords written at every Present, "ADDR=VALUE,..." in hex, e.g. the
// graphics settings the detection 0071d0a0 fills (g_CarEnvironmentMapEnable a65340, g_ParticleSystemEnable a6536c,
// g_VisualTreatment a65394, g_ShadowDetail a65398, g_WorldLodLevel a65370; table at 00711c60) to measure them.
void pokePresent(win32::WinApplication* app)
{
    static std::vector<std::pair<x86::reg32, x86::reg32>> s_pokes;
    static bool s_parsed = false;
    if (!s_parsed)
    {
        s_parsed = true;
        const std::string spec = pf::Config::getString("debug_poke", "");
        size_t at = 0;
        while (at < spec.size())
        {
            size_t end = spec.find(',', at);
            if (end == std::string::npos)
                end = spec.size();
            const std::string item = spec.substr(at, end - at);
            const size_t eq = item.find('=');
            if (eq != std::string::npos)
            {
                s_pokes.emplace_back(x86::reg32(strtoul(item.substr(0, eq).c_str(), nullptr, 16)),
                                     x86::reg32(strtoul(item.substr(eq + 1).c_str(), nullptr, 16)));
                pf::logInfo("debug_poke: [0x%08x] = 0x%x every frame", unsigned(s_pokes.back().first),
                            unsigned(s_pokes.back().second));
            }
            at = end + 1;
        }
    }
    for (const auto& poke : s_pokes)
        app->getMemory<x86::reg32>(poke.first) = poke.second;
}

// Setting car_reflections (default on): off writes 0 to g_CarEnvironmentMapEnable [0xa65340] at every Present;
// 0072ecc8 checks it every frame before drawing the car environment map (one 256x256 pass of 50-90 world draws in
// lowlod, ~10% of a race frame's draws). Off was +11 % before thread_normal (17.1 / 17.2 vs 16.3 / 14.4 fps, runs
// 198-203) but nothing measurable after it (off 17.7, on 18.5, runs 216 / 217), and the pass also feeds the car's
// light from the surroundings: off, cars were lit bright and flat in dark streets (user report, run 217 shots).
void carReflectionPresent(win32::WinApplication* app)
{
    static const bool s_off = !pf::Config::getBool("car_reflections", true);
    if (s_off)
        app->getMemory<x86::reg32>(0x00a65340) = 0;
}

// Setting pool_log (diagnostic, seconds, default 0 = off): every N s, the free lists of the game's bMalloc pools
// (table 0xa84c20, 00470d80; free list at pool + 4: [node] next, [node + 8] size, as 00476560 walks it). The audio
// pool is pool [0xa59958] (0051f7c1), [0xa8c2c0] bytes: how much of its 32 MB the sound banks really take, to size
// audio_pool_kb (guest memory for the city section's 74 MB load buffer, run 253).
void poolLogPresent(win32::WinApplication* app)
{
    static const uint64_t s_every = uint64_t(std::max(0, pf::Config::getInt("pool_log", 0))) * 1000000u;
    static uint64_t s_next = 0;
    if (!s_every)
        return;
    const uint64_t now = sceKernelGetProcessTimeWide();
    if (now < s_next)
        return;
    s_next = now + s_every;
    const auto word = [](x86::reg32 address) {
        x86::reg32 value = 0;
        if (win32::MemMap::isGuestRange(address, 4))
            memcpy(&value, win32::g_guestBase + address, 4);
        return value;
    };
    const x86::reg32 audioPool = word(0x00a59958);
    char line[512];
    int length = 0;
    for (x86::reg32 n = 0; n < 16; ++n)
    {
        const x86::reg32 pool = word(0x00a84c20 + n * 4);
        if (!pool)
            continue;
        x86::reg32 largest = 0, total = 0, count = 0;
        for (x86::reg32 node = word(pool + 4); node && node != pool + 4 && count < 100000; node = word(node))
        {
            const x86::reg32 size = word(node + 8);
            largest = std::max(largest, size);
            total += size;
            ++count;
        }
        // +0x40: 004758e0 fills each freed block (0xeeeeeeee or a pattern) when this byte is set.
        length += snprintf(line + length, sizeof(line) - size_t(length), " %u%s: %u KB free (%u, largest %u KB, fill %u);",
                           unsigned(n), n == audioPool ? " audio" : "", unsigned(total / 1024), unsigned(count),
                           unsigned(largest / 1024), unsigned(word(pool + 0x40) & 0xff));
        if (length >= int(sizeof(line)) - 64)
            break;
    }
    pf::logInfo("pool_log: audio pool %u KB; [a85384] %u [a4e298] %u; pools%s", unsigned(word(0x00a8c2c0) / 1024),
                unsigned(word(0x00a85384)), unsigned(word(0x00a4e298)), line);
}

// Setting pool_fill (default off): the game's free 004758e0 fills every freed block with 0xeeeeeeee (rep stosd at
// 004759d6) when the pool's byte +0x40 is set; run 318: set in 8 of the 9 pools (pool_log "fill 1"). The fill is
// a debug aid nothing reads back; 004758e0 was the hottest game function (3 % of samples, run 315). Off clears the
// byte in every pool at each Present (pools made later get it cleared at the next one).
void poolFillPresent(win32::WinApplication* app)
{
    static const bool s_fill = pf::Config::getBool("pool_fill", false);
    if (s_fill)
        return;
    for (x86::reg32 n = 0; n < 16; ++n)
    {
        const x86::reg32 pool = app->getMemory<x86::reg32>(0x00a84c20 + n * 4);
        if (pool && win32::MemMap::isGuestRange(pool + 0x40, 1))
            app->getMemory<uint8_t>(pool + 0x40) = 0;
    }
}

// Per Present: the pad layout (pad.cpp), car_reflections, pool_fill, debug_poke, pool_log, then ai_drive.
void framePresent(win32::WinApplication* app, x86::CPU& cpu)
{
    nfsc::padPresent(app, cpu);
    carReflectionPresent(app);
    poolFillPresent(app);
    pokePresent(app);
    poolLogPresent(app);
    if (s_aiDrive)
        aiDrivePresent(app, cpu);
}

}

int main(int, char**)
{
    scePowerSetBusClockFrequency(222);
    scePowerSetGpuClockFrequency(222);
    scePowerSetGpuXbarClockFrequency(166);

    mkdir("ux0:data", 0777);
    mkdir(s_dataDirectory.c_str(), 0777);
    // Shader compile mode (tools/shadercomp.ps1): compiles the Cg programs listed in shaders/compile.txt with the
    // Vita's own compiler and exits, the game does not start.
    if (shadercomp::runIfRequested(dataPath("shaders").c_str()))
    {
        sceKernelExitProcess(0);
        return 0;
    }
    pf::logInit(dataPath("nfscvita.log").c_str());
    pf::logInfo("NFSCVita %s starting (Vita up for %u s)", NFSU_VERSION, unsigned(sceKernelGetSystemTimeWide() / 1000000));
    std::set_terminate(&onTerminate);
    signal(SIGABRT, &onAbort);
    atexit(&onExit);
    pf::logInfo("clocks: arm %d MHz, bus %d MHz, gpu %d MHz, xbar %d MHz", scePowerGetArmClockFrequency(),
                scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency(), scePowerGetGpuXbarClockFrequency());
    x86::mmxSelfTest();
    x86::fpuSelfTest();
    pf::Config::load(dataPath("nfscvita.cfg").c_str());
    pf::logSetConsole(pf::Config::getBool("console_log", false));
    // Setting cpu_clock: 0 (default) leaves the ARM clock to the player (PSVshell); 333, 444 or 500 set it here, so a
    // test build needs no overclocking tool before the start. (Bus 222 / GPU 222 / xbar 166 MHz are set above.)
    const int cpuClock = pf::Config::getInt("cpu_clock", 0);
    if (cpuClock == 333 || cpuClock == 444 || cpuClock == 500)
        scePowerSetArmClockFrequency(cpuClock);
    pf::logInfo("clocks now: arm %d MHz, bus %d MHz, gpu %d MHz", scePowerGetArmClockFrequency(),
                scePowerGetBusClockFrequency(), scePowerGetGpuClockFrequency());
    pf::cores::init();
    pf::perf::init(pf::Config::getBool("timing_log", false), pf::Config::getBool("hitch_log", false));
    // Setting sampler=on: the game thread's pc about every sampler_interval_us into samples.bin (NFSUG2_VITA's
    // profiler; needs CapUnlocker), read with tools/samples.py.
    pf::sampler::start(dataPath("samples.bin").c_str());
#ifdef NFSU_PGO_GENERATE
    // Instrumented build (tools/build.ps1 -PgoGenerate): the profile counters go to nfsu.profraw every 30 s.
    pf::pgo::start(dataPath(pf::pgo::FileName).c_str());
#endif
    sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);

    std::vector<uint8_t> image;
    std::vector<win32::Section> sections;
    if (!loadExecutable(image) || !imageSections(image, sections))
        pf::fatal("NFSC.exe is missing or not version 1.4 (see the log)");
    if (!pf::gxm::Device::get().init())
        pf::fatal("graphics initialisation failed");

    win32::File::setDataDirectory((s_dataDirectory + "/").c_str());
    win32::File::setCdDirectory((s_dataDirectory + "/").c_str());
    // Carbon reads HKLM\SOFTWARE\Electronic Arts\EA GAMES\Need for Speed Carbon (Install Dir, Language, ...) in
    // 007346b0; registry.txt in the data folder supplies them.
    win32::registry::load(dataPath("registry.txt").c_str());
    win32::kernel32::setModulePath(s_guestExePath);
    win32::kernel32::setResourceSection(nfsc::layout::imageBase, nfsc::layout::resourceSection);
    {
        nfsc::Application app(s_commandLine, sections);
        // Carbon's main streaming pool (006b6e00 -> 007a42e0) is 68.4 MB + [0xa62c48] KB + extras, 139 MB on PC,
        // more than the 126 MB the guest arena has left at that point. [0xa62c48] (65000 KB on PC) sizes the
        // sub-pool 007e4910 carves out of it for 0xb77018; the game's other mode runs it with [0xa62c4c] = 1 KB.
        // On guest memory exhaustion: the state of the game's pool 7 (free list at pool + 4: [node] next,
        // [node + 8] size, 00476560) and the switches that size it, to tell why a section did not fit (run 35).
        win32::MemMap::addReport([] {
            const auto word = [](x86::reg32 address) {
                x86::reg32 value = 0;
                if (win32::MemMap::isGuestRange(address, 4))
                    memcpy(&value, win32::g_guestBase + address, 4);
                return value;
            };
            const struct mallinfo host = mallinfo();
            pf::logInfo("memory: newlib heap %u KB in use, %u KB free of %u KB taken from its %d KB",
                        unsigned(host.uordblks / 1024), unsigned(host.fordblks / 1024), unsigned(host.arena / 1024),
                        _newlib_heap_size_user / 1024);
            // Audio pool use: the arena starts zeroed and this block is taken at boot, so pages that are still
            // all zero were never written.
            const x86::reg32 audio = word(0x00a8c2b8), audioSize = word(0x00a8c2c0);
            if (audio && audioSize && win32::MemMap::isGuestRange(audio, audioSize))
            {
                x86::reg32 touched = 0, last = 0;
                for (x86::reg32 page = 0; page + 4096 <= audioSize; page += 4096)
                {
                    const x86::reg8* p = win32::g_guestBase + audio + page;
                    bool used = false;
                    for (int i = 0; i < 4096 && !used; i += 4)
                        used = p[i] | p[i + 1] | p[i + 2] | p[i + 3];
                    if (used)
                    {
                        ++touched;
                        last = page + 4096;
                    }
                }
                pf::logInfo("memory: audio pool at 0x%08x, %u KB: %u KB in written pages, highest written offset %u KB",
                            unsigned(audio), unsigned(audioSize / 1024), unsigned(touched * 4), unsigned(last / 1024));
            }
            const x86::reg32 pool = word(0x00a84c20 + 7 * 4);
            x86::reg32 largest = 0, total = 0, count = 0;
            if (pool)
                for (x86::reg32 node = word(pool + 4); node && node != pool + 4 && count < 100000; node = word(node))
                {
                    const x86::reg32 size = word(node + 8);
                    largest = std::max(largest, size);
                    total += size;
                    ++count;
                }
            pf::logInfo("memory: game pool 7 at 0x%08x: %u free blocks, %u bytes free, largest %u; car sub-pool "
                        "[0xb77084] %u, [0xa9e620] %u, [0xa9e62c] %u, [0xa9e668] %u, [0xb69c10] %u, [0xa631b8] %u",
                        unsigned(pool), unsigned(count), unsigned(total), unsigned(largest),
                        unsigned(word(0x00b77084)), unsigned(word(0x00a9e620)), unsigned(word(0x00a9e62c) & 0xff),
                        unsigned(word(0x00a9e668)), unsigned(word(0x00b69c10)), unsigned(word(0x00a631b8)));
            // The resource-file load slots of 0055f990: 64 of 16 bytes at 0xa8fb20, +2 in use, +3 keeps the slot
            // after the load (0055fb4b), +0xc the requester. With all 64 in use a new load is dropped without a
            // callback and its requester waits forever (quick race freeze, STATUS runs 302-307).
            unsigned used = 0, kept = 0;
            char slots[64 * 20 + 1];
            int length = 0;
            slots[0] = 0;
            for (x86::reg32 slot = 0x00a8fb20; slot < 0x00a8ff20; slot += 16)
            {
                const x86::reg32 flags = word(slot);
                if (!(flags & 0x00ff0000))
                    continue;
                ++used;
                kept += (flags & 0xff000000) ? 1 : 0;
                if (length < int(sizeof(slots)) - 24)
                    length += snprintf(slots + length, sizeof(slots) - length, " %02x:%08x:%08x",
                                       unsigned((slot - 0x00a8fb20) / 16), unsigned(flags), unsigned(word(slot + 0xc)));
            }
            pf::logInfo("memory: resource load slots %u of 64 in use (%u kept after loading); slot:flags:requester%s",
                        used, kept, slots);
            for (x86::reg32 slot = 0x00a8fb20; slot < 0x00a8ff20; slot += 16)
                if (word(slot) & 0x00ff0000)
                    pf::logInfo("memory: load slot %02x: %08x callback %08x(%08x) requester %08x",
                                unsigned((slot - 0x00a8fb20) / 16), unsigned(word(slot)), unsigned(word(slot + 4)),
                                unsigned(word(slot + 8)), unsigned(word(slot + 0xc)));
            // The read queue 006b5d30 issues nothing while [a99690] - [a99694] + [a99698] >= 3 (reads in flight).
            unsigned queued = 0;
            char reads[8 * 40 + 1];
            int readsLength = 0;
            reads[0] = 0;
            for (x86::reg32 node = word(0x00a99c40); node && node != 0x00a99c40 && queued < 100000; node = word(node))
            {
                if (queued++ < 8 && readsLength < int(sizeof(reads)) - 40)
                    readsLength += snprintf(reads + readsLength, sizeof(reads) - readsLength, " %08x:%08x:%08x:%08x",
                                            unsigned(node), unsigned(word(node + 0xc)), unsigned(word(node + 0x1c)),
                                            unsigned(word(node + 0x10)));
            }
            pf::logInfo("memory: read queue [a99690] %u [a99694] %u [a99698] %u [a62ed4] %u [a9968c] %08x; %u queued "
                        "(node:+c:+1c:+10)%s", unsigned(word(0x00a99690)), unsigned(word(0x00a99694)),
                        unsigned(word(0x00a99698)), unsigned(word(0x00a62ed4)), unsigned(word(0x00a9968c)), queued, reads);
            // The issued reads ([a99698] of them): list 0xa99a94, appended at 006b5e93; 006b4760 completes only the
            // first, when its status +0x40 is 2 (done) or 3 (error). Run 312: 3 issued, 1 file read logged.
            unsigned issued = 0;
            for (x86::reg32 node = word(0x00a99a94); node && node != 0x00a99a94 && issued < 16; node = word(node))
            {
                ++issued;
                pf::logInfo("memory: issued read %08x: status +40 %u, +8 %08x +c %08x +10 %08x +14 %08x +18 %08x "
                            "+1c %08x +20 %08x +24 %08x +28 %08x +2c %08x +30 %08x +34 %08x +38 %08x +3c %08x",
                            unsigned(node), unsigned(word(node + 0x40)), unsigned(word(node + 8)),
                            unsigned(word(node + 0xc)), unsigned(word(node + 0x10)), unsigned(word(node + 0x14)),
                            unsigned(word(node + 0x18)), unsigned(word(node + 0x1c)), unsigned(word(node + 0x20)),
                            unsigned(word(node + 0x24)), unsigned(word(node + 0x28)), unsigned(word(node + 0x2c)),
                            unsigned(word(node + 0x30)), unsigned(word(node + 0x34)), unsigned(word(node + 0x38)),
                            unsigned(word(node + 0x3c)));
            }
            pf::logInfo("memory: %u issued reads in list 0xa99a94; [a99660] %08x [a99664] %08x [a99668] %08x",
                        issued, unsigned(word(0x00a99660)), unsigned(word(0x00a99664)), unsigned(word(0x00a99668)));
            // Job worker threads (entry 008d6b40, threads 6 and 10): the parameter object has +4 running, +8 queue
            // head, +0xc tail, +0x10 count; each job runs through [job + 0x44] (008d6b9a).
            for (unsigned index = 0; index < 4; ++index)
            {
                const x86::reg32 worker = win32::kernel32::threadParameter(0x008d6b40, index);
                if (!worker)
                    break;
                char jobs[8 * 48 + 1];
                int jobsLength = 0;
                jobs[0] = 0;
                unsigned count = 0;
                for (x86::reg32 job = word(worker + 8); job && count < 8; job = word(job), ++count)
                    jobsLength += snprintf(jobs + jobsLength, sizeof(jobs) - jobsLength, " %08x:%08x:%08x:%08x",
                                           unsigned(job), unsigned(word(job + 4)), unsigned(word(job + 8)),
                                           unsigned(word(job + 0xc)));
                pf::logInfo("memory: job worker %u at %08x: running %u head %08x tail %08x count %u; words +0 %08x "
                            "+14 %08x +18 %08x +1c %08x +20 %08x; jobs (job:+4:+8:+c)%s",
                            index, unsigned(worker), unsigned(word(worker + 4) & 0xff), unsigned(word(worker + 8)),
                            unsigned(word(worker + 0xc)), unsigned(word(worker + 0x10)), unsigned(word(worker)),
                            unsigned(word(worker + 0x14)), unsigned(word(worker + 0x18)), unsigned(word(worker + 0x1c)),
                            unsigned(word(worker + 0x20)), jobs);
            }
        });
        // The city (L5RA.BUN) also has a permanent section, number 2500, of 74.4 MB that 0079ebf4 takes from the
        // main heap, so on the Vita the pool has to shrink by about that much: 139 MB (PC) -> ~37 MB.
        // 24000 KB until v0.1.3: a quick race with top-tier cars (Lamborghini) loads ~10 MB per car and needed
        // more than 36 MB (run 299: 33.7 MB taken, still loading); with too little the car loader 0xb77018 waits
        // for room forever and the race never starts (runs 294-299, issue #1). The extra 22 MB: 10 MB more arena
        // (memmap.cpp), the rest from the reclaimable buffer and texture copies.
        const int subPoolKb = pf::Config::getInt("stream_subpool_kb", 46000);
        app.getMemory<x86::reg32>(0x00a62c48) = x86::reg32(subPoolKb);
        pf::logInfo("memory: streaming sub-pool [0xa62c48] %d KB (PC: 65000)", subPoolKb);
        // Track part of that pool: 0x445c000 / 0x441c000 in the PC code (006b6ef2, 00699a68), patched by
        // generate.py PATCHES to read the slot 0x00c2b400.
        // Run 78: 20 MB left the free roam waiting 8-9 s for track sections at every new area (frozen picture,
        // the world filling in afterwards); 50 MB streams ahead without long stops (arena: ~108 MB were free).
        // 46 MB since 2026-11-18: with 50 MB the career load after the intro race lacked ~300 KB of main heap
        // (stopgap until the 74 MB city section is handled; confirmed by the user).
        const int trackKb = pf::Config::getInt("stream_track_kb", 46000);
        app.getMemory<x86::reg32>(0x00c2b400) = x86::reg32(trackKb) * 1024u;
        pf::logInfo("memory: streaming track part [0xc2b400] %d KB (PC: 70000)", trackKb);
        // Track section buffers: generate.py PATCHES turn the streamer's oversized-section allocation into malloc
        // and its frees into free; free passes the game allocator's own blocks on to bFree (00476d70).
        win32::msvcr71::setForeignFree(0x00476d70);
        // The city section (74,368,384 bytes) needs one free range of that size. Before its first load the arena
        // fills from both ends and splits up: run 296 (car pool 40 MB, quick race from the menu) had 88.7 MB free
        // but no range over 68.5 MB. Reserve the top of the arena for it from the start (memmap.cpp soft
        // reservation; other blocks go there only when nothing else fits). Setting city_reserve=off turns it off.
        if (pf::Config::getBool("city_reserve", true))
            win32::MemMap::reserveTop(74368384);
        // Texture-pack chunk handlers: unlink pending pack data on unload (run 74 crash, texturepacks.cpp).
        nfsc::installTexturePackFix(app);
        // The EA audio thread runs in parallel with the game, as on PC (audio_thread.cpp).
        nfsc::installUnlockedAudio(app);
        // Setting audio_thread_core: the core of the EA audio thread (0x008a800c). "audio" (default, core 2: SceAudioOut
        // mixer and timer service), "helper" (core 3 with CapUnlocker, shared with the other guest threads, io, a job
        // phase worker and a GXM command list worker) or "render" (core 1). Runs 229-236: on core 3 the game spent
        // ~4 ms per frame re-taking the execution lock after its waits (a helper thread holding the lock preempted by
        // this priority-15 thread on the same core); on core 2 0.3 ms, and the SND lock waits halve: +8 % fps.
        {
            const std::string core = pf::Config::getString("audio_thread_core", "audio");
            if (core == "audio")
                win32::scheduler::setThreadRole(0x008a800c, pf::cores::Audio, "EA audio");
            else if (core == "render")
                win32::scheduler::setThreadRole(0x008a800c, pf::cores::Render, "EA audio");
            pf::logInfo("audio: EA audio thread core role %s (audio_thread_core)", core.c_str());
        }
        // Audio pool (0051f780, patched to read the slot 0x00c2b404): 32 MB on PC.
        const int audioKb = pf::Config::getInt("audio_pool_kb", 32768);
        app.getMemory<x86::reg32>(0x00c2b404) = x86::reg32(audioKb) * 1024u;
        pf::logInfo("memory: audio pool [0xc2b404] %d KB (PC: 32768)", audioKb);
        // Widescreen: HUD/menu layout and projection slots for the 960x544 display (widescreen.cpp).
        nfsc::installWidescreen(app);
        // [0xa9e6d8] is the game's skip-movies switch (Extra Options): a front-end movie object (005baea0, type 7)
        // that finds it set posts "movie done" (0xc3960eb9) at once. Movies are slow (VP6 decoded by guest code,
        // ~8 fps) and grey (no shader translation yet), so they are skipped unless skip_movies=off.
        if (pf::Config::getBool("skip_movies", true))
        {
            app.getMemory<x86::reg32>(0x00a9e6d8) = 1;
            pf::logInfo("movies: skipped ([0xa9e6d8] = 1, setting skip_movies)");
        }
        // EA's audio thread (start 008a800c, loop 008a7de6) mixes what is due since its start time (008a7c75:
        // elapsed ms * rate - mixed) in 256-sample chunks under the execution lock; after a stall it mixed slower
        // than real time and never let the game run again (run 55). Its clock is paced instead.
        // Run 76: after loads it still fell behind and mixed back to back for 8-20 s (frozen picture, sound fine):
        // the step per call is 20 ms (setting audio_paced_step) and its sleeps last at least audio_duty % of the
        // time it worked (default 100: at most about half of the lock).
        win32::winmm::setPacedThreadStart(0x008a800c,
                                          uint32_t(std::max(5, pf::Config::getInt("audio_paced_step", 20))));
        win32::winmm::setPacedDuty(uint32_t(std::max(0, pf::Config::getInt("audio_duty", 100))));
        // Direct boot (as NFSUG2_VITA's boot_race): the game's own SkipFE debug switch [0xa9e620] skips the front
        // end into a quick race. Its settings live in the debug-variable block serialised by 006bf080 and are
        // copied into the race parameters (0xa7a1a8...) by 007a9b90: track number [0xa6313c] (PC default 5000,
        // -1 -> 2000 at 006bf060), opponents [0xa63154] (2), laps [0xa63150] (1), player car preset [0xa63184]
        // ("M3GTRCAREERSTART"). For test runs without pressing through the menus.
        s_aiDrive = pf::Config::getBool("ai_drive", false);
        nfsc::installPad();
        win32::d3d9::bridge::setPresentHook(framePresent);
        if (pf::Config::getBool("boot_race", false))
        {
            app.getMemory<x86::reg32>(0x00a9e620) = 1;
            const int track = pf::Config::getInt("boot_track", 5000);
            const int opponents = pf::Config::getInt("boot_opponents", 2);
            app.getMemory<x86::reg32>(0x00a6313c) = x86::reg32(track);
            app.getMemory<x86::reg32>(0x00a63154) = x86::reg32(opponents);
            // Traffic: in a SkipFE race the race status has no race parameters ([[0xa98284]+0x6a1c] stays null, run
            // 135), so the spawner's density 00422cc0 takes the SkipFE branch: clamp([0xa9e640] * 0.01, 0, 1)
            // unless [0xa9e644] is set. [0xa9e640] is therefore on a 0..100 scale (the setup 0075e446 also feeds
            // it * 100 into attribute 0xc64bc341, clamped to 100, which nothing reads without race parameters).
            // Runs 126-133 wrote the 0..1 fraction itself: density 0.003, no traffic. For scale: free roam = 1.0,
            // Quick Race traffic Low/Medium/High = 0.03/0.10/0.50. Setting boot_traffic: 0..1 (default 0.3).
            const float traffic = std::max(0.0f, std::min(1.0f, float(atof(pf::Config::getString("boot_traffic", "0.3")))));
            const float trafficPercent = traffic * 100.0f;
            memcpy(win32::g_guestBase + 0x00a9e640, &trafficPercent, 4);
            app.getMemory<x86::reg8>(0x00a9e644) = 0;
            pf::logInfo("boot: SkipFE race ([0xa9e620] = 1), track %d, %d opponents, traffic %.2f", track, opponents,
                        double(traffic));
        }
        std::vector<win32::Section>().swap(sections);
        std::vector<uint8_t>().swap(image);
        nfsc::bankcode::install(app);
        if (!win32::File::setCurrentDirectory(s_guestGameFolder))
            pf::fatal("%s (the game folder) is missing", s_guestGameFolder);
        app.execute();
        win32::d3d9::bridge::drain();
    }
    pf::logInfo("NFSCVita exiting");
    pf::gxm::Device::get().shutdown();
    pf::logShutdown();
    sceKernelExitProcess(0);
    return 0;
}

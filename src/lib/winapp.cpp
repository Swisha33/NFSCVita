#include <lib/winapp.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/threadmgr.h>
#include <psp2/kernel/threadmgr/eventflag.h>
#include <algorithm>
#include <string>
#include <lib/file.h>
#include <lib/foreigncode.h>
#include <lib/mutex.h>
#include <lib/event.h>
#include <lib/heap.h>
#include <lib/library.h>
#include <lib/memmap.h>
#include <lib/scheduler.h>
#include <lib/teb.h>
#include <lib/thread.h>
#include <lib/timer.h>
#include <winapi/types.h>
#include <x86.h>

#include <winapi/com.h>
#include <winapi/ddraw.h>
#include <winapi/kernel32.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/perf.h>
#include <platform/sampler.h>
#include <platform/sync.h>
#include <platform/time.h>
#include <malloc.h>
#include <atomic>
#include <cstring>
#include <vector>

namespace win32
{

static x86::reg32 s_resourceIndex;
static std::atomic<int> s_resourceCount{0};

// Words of the guest thread block (fs) used by the host; guest code only uses fs:[0].
static const x86::reg32 ThreadLockOffset     = teb::HostLock;
static const x86::reg32 ThreadControlOffset  = teb::HostControl;

// How long a thread of a raised level waits awake for the execution lock before it blocks, and how long a thread
// that yielded it waits for such a thread to take it before queueing again.
static const uint32_t PreemptSpinUs = 500;
// How long a thread that yielded the lock waits for the higher-level waiter to take it. The waiter spins on
// s_lockHeld and takes it within a few microseconds; 500 us here cost 7 % of the game thread (runs 96, 132).
static const uint32_t YieldSpinUs = 100;

// Threads waiting for the execution lock per preemption level above normal (m_preemptWaiters is their sum).
static std::atomic<int> s_levelWaiters[scheduler::LevelCount];

// True while a thread holds the execution lock: spinning waiters read it instead of calling the kernel.
static std::atomic<bool> s_lockHeld{false};
// Spin iterations between two clock reads of the spin loops.
static const uint32_t SpinClockMask = 15;

// Stall watchdog: the CPU of the thread that took the execution lock last, and the time of the last Present. When no
// frame comes for StallUs, a host thread logs the guest return addresses on the holder's stack (a likely call chain).
static std::atomic<const x86::CPU*> s_holderCpu{nullptr};
static std::atomic<uint64_t> s_lastFrameUs{0};
static pf::Thread* s_watchdog = nullptr;
static const uint64_t StallUs = 8000000;

// Setting debug_watch: guest values the watchdog logs every 10 s when they change, e.g.
// "a98284>bc9c,a61290:b". Each entry is hex addresses joined by '>' (read a pointer at the address, add the next
// offset); ":b" reads a byte, else a word. Unreadable chains log as such.
// Setting debug_scan: a text the watchdog looks for in guest memory every 10 s (from the image base up), logging
// each address where it starts, with the 16 bytes before it, while the set of addresses changes.
static void logFinds()
{
    static const std::string s_text = pf::Config::getString("debug_scan", "");
    static std::string s_last;
    if (s_text.empty())
        return;
    std::string found;
    unsigned hits = 0;
    const size_t n = s_text.size();
    x86::reg32 end = 0x400000;
    while (MemMap::isGuestRange(end + 0x100000, 1))
        end += 0x100000;
    for (x86::reg32 a = 0x400000; hits < 12 && a + n < end; ++a)
    {
        const void* p = memchr(g_guestBase + a, s_text[0], end - n - a);
        if (!p)
            break;
        a = x86::reg32(static_cast<const uint8_t*>(p) - g_guestBase);
        if (memcmp(g_guestBase + a, s_text.data(), n) != 0)
            continue;
        char line[128];
        int at = snprintf(line, sizeof(line), " %08x[", unsigned(a));
        for (int k = 16; k > 0; --k)
            at += snprintf(line + at, sizeof(line) - at, "%02x", unsigned(g_guestBase[a - k]));
        snprintf(line + at, sizeof(line) - at, "]");
        found += line;
        ++hits;
    }
    static bool s_sized = false;
    if (!s_sized)
        pf::logInfo("debug_scan: scanning %u MB", unsigned((end - 0x400000) >> 20));
    s_sized = true;
    if (found != s_last)
        pf::logInfo("debug_scan \"%s\":%s", s_text.c_str(), found.empty() ? " none" : found.c_str());
    s_last = found;
}

static void logWatches()
{
    static std::string s_spec = pf::Config::getString("debug_watch", "");
    static uint32_t s_last[16];
    static bool s_known[16];
    if (s_spec.empty())
        return;
    unsigned index = 0;
    size_t begin = 0;
    while (begin < s_spec.size() && index < 16)
    {
        size_t end = s_spec.find(',', begin);
        if (end == std::string::npos)
            end = s_spec.size();
        const std::string entry = s_spec.substr(begin, end - begin);
        begin = end + 1;
        const bool byteRead = entry.size() > 2 && entry.compare(entry.size() - 2, 2, ":b") == 0;
        const std::string chain = byteRead ? entry.substr(0, entry.size() - 2) : entry;
        uint32_t address = 0;
        bool readable = true;
        size_t at = 0;
        for (bool first = true; at <= chain.size(); first = false)
        {
            size_t next = chain.find('>', at);
            if (next == std::string::npos)
                next = chain.size();
            const uint32_t offset = uint32_t(strtoul(chain.substr(at, next - at).c_str(), nullptr, 16));
            if (!first)
            {
                if (!MemMap::isGuestRange(address, 4))
                {
                    readable = false;
                    break;
                }
                memcpy(&address, g_guestBase + address, 4);
            }
            address += offset;
            at = next + 1;
        }
        uint32_t value = 0;
        if (readable && MemMap::isGuestRange(address, byteRead ? 1 : 4))
            memcpy(&value, g_guestBase + address, byteRead ? 1 : 4);
        else
            readable = false;
        if (!readable)
            value = 0xdeadbeef;
        if (!s_known[index] || s_last[index] != value)
        {
            s_known[index] = true;
            s_last[index] = value;
            if (readable)
                pf::logInfo("watch: %s = 0x%08x (%d)", entry.c_str(), unsigned(value), int(value));
            else
                pf::logInfo("watch: %s not readable", entry.c_str());
        }
        ++index;
    }
}

static void logGuestStack(const char* who, const x86::CPU* cpu)
{
    if (!cpu)
        return;
    const x86::reg32 esp = cpu->esp;
    char line[512];
    int length = snprintf(line, sizeof(line), "stall: %s esp 0x%08x, code addresses on the stack:", who, unsigned(esp));
    unsigned found = 0;
    for (x86::reg32 slot = esp & ~3u; slot < esp + 0x2000 && found < 24 && length < int(sizeof(line)) - 12; slot += 4)
    {
        x86::reg32 word;
        memcpy(&word, g_guestBase + slot, 4);
        if (word >= 0x00401000 && word < 0x009c1000)
        {
            length += snprintf(line + length, sizeof(line) - length, " %08x", unsigned(word));
            ++found;
        }
    }
    pf::logInfo("%s", line);
}

static std::atomic<int> s_mainThread{0};

// Setting hitch_probe_ms=N (default 0: off): when a frame takes longer than N ms, logs once per such frame what the
// main thread waits on (kernel wait type and object, named where the kernel knows it), whether the execution lock is
// held, and the guest return addresses on the main thread's stack. Checked every 50 ms.
static void probeHitch(const x86::CPU* mainCpu, uint64_t idle, uint32_t thresholdMs, bool& reported)
{
    if (idle < uint64_t(thresholdMs) * 1000)
    {
        reported = false;
        return;
    }
    if (reported)
        return;
    reported = true;
    SceKernelThreadInfo info;
    info.size = sizeof(info);
    const int thread = s_mainThread.load(std::memory_order_relaxed);
    if (!thread || sceKernelGetThreadInfo(thread, &info) < 0)
        return;
    char name[32] = "?";
    SceKernelMutexInfo mutex;
    mutex.size = sizeof(mutex);
    SceKernelSemaInfo sema;
    sema.size = sizeof(sema);
    SceKernelEventFlagInfo flag;
    flag.size = sizeof(flag);
    const char* lw = pf::LwMutex::nameOf(uint32_t(info.waitId));
    if (lw)
        snprintf(name, sizeof(name), "lw %s", lw);
    else if (sceKernelGetMutexInfo(info.waitId, &mutex) >= 0)
        memcpy(name, mutex.name, sizeof(name));
    else if (sceKernelGetSemaInfo(info.waitId, &sema) >= 0)
        memcpy(name, sema.name, sizeof(name));
    else if (sceKernelGetEventFlagInfo(info.waitId, &flag) >= 0)
        memcpy(name, flag.name, sizeof(name));
    name[31] = 0;
    const x86::CPU* holder = s_holderCpu.load(std::memory_order_relaxed);
    pf::logInfo("hitch: frame over %u ms: main thread status 0x%x wait type 0x%x on 0x%08x %s, execution lock %s",
                unsigned(idle / 1000), unsigned(info.status), unsigned(info.waitType), unsigned(info.waitId), name,
                !s_lockHeld.load(std::memory_order_relaxed) ? "free" : holder == mainCpu ? "held by main" : "held");
    logGuestStack("hitch main thread", mainCpu);
    if (holder && holder != mainCpu)
        logGuestStack("hitch lock holder", holder);
}

static int watchdogRun(void* argument)
{
    const x86::CPU* mainCpu = static_cast<const x86::CPU*>(argument);
    bool stalled = false, hitchReported = false;
    const uint32_t hitchMs = uint32_t(std::max(0, pf::Config::getInt("hitch_probe_ms", 0)));
    pf::logInfo("stall: watchdog started%s", hitchMs ? " (hitch probe on)" : "");
    for (unsigned tick = 1;; ++tick)
    {
        if (hitchMs)
        {
            for (int k = 0; k < 40; ++k)
            {
                pf::sleepMs(50);
                probeHitch(mainCpu, pf::ticksUs() - s_lastFrameUs.load(std::memory_order_relaxed), hitchMs,
                           hitchReported);
            }
        }
        else
            pf::sleepMs(2000);
        // Keeps the Vita from dimming, turning the screen off or suspending while nobody presses a button (a game
        // driven by auto_input, a long loading screen). Without it, runs 106-123 froze in file I/O ~20 s after the
        // start once the user had been away for a while.
        sceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT);
        // Guest page 0 should stay zero: Carbon's allocator bMalloc 00476b20 writes there when a pool is out of
        // memory (the Mac port's notes), which no port notices. Logged once per new nonzero word.
        {
            static uint32_t s_seen[16];
            static unsigned s_seenCount = 0;
            for (uint32_t offset = 0; offset < 0x1000 && s_seenCount < 16; offset += 4)
            {
                uint32_t word;
                memcpy(&word, g_guestBase + offset, 4);
                if (!word)
                    continue;
                bool known = false;
                for (unsigned k = 0; k < s_seenCount; ++k)
                    known = known || s_seen[k] == (offset << 20 ^ word);
                if (known)
                    continue;
                s_seen[s_seenCount++] = offset << 20 ^ word;
                pf::logError("memory: guest address 0x%03x holds 0x%08x (a write through a null pointer: the "
                             "allocator's out-of-memory path?)", unsigned(offset), unsigned(word));
            }
        }
        if (tick % 5 == 0)
            logWatches();
        logFinds();
        const uint64_t idle = pf::ticksUs() - s_lastFrameUs.load(std::memory_order_relaxed);
        if (tick % 5 == 0)
        {
            // The host (newlib) heap too: issue #5 died with it full (26 MB in use) after 30-40 minutes.
            const struct mallinfo host = mallinfo();
            pf::logInfo("stall: watchdog alive, last frame %u ms ago, %d guest files open (%d parked), host heap %u KB "
                        "in use", unsigned(idle / 1000), File::openCount(), File::parkedCount(),
                        unsigned(host.uordblks / 1024));
        }
        // Every 2 s: a process that dies keeps only synced lines (run 73b lost its last ~10 s).
        pf::logSync();
        if (idle < StallUs)
        {
            if (stalled)
            {
                pf::logInfo("stall: frames again");
                pf::sampler::mark(0);
            }
            stalled = false;
            continue;
        }
        if (stalled)
            continue;
        stalled = true;
        pf::sampler::mark(1);   // sampler=on: `samples.py --mark 1` counts only the stalled time
        const x86::CPU* holder = s_holderCpu.load(std::memory_order_relaxed);
        pf::logInfo("stall: no frame for %u s, execution lock %s", unsigned(idle / 1000000),
                    s_lockHeld.load(std::memory_order_relaxed) ? "held" : "free");
        logGuestStack(holder == mainCpu ? "lock holder (main thread)" : "lock holder", holder);
        // The memory picture at the stall (run 39: a loader that found no room in a game pool froze instead of
        // stopping: its assert is a write to address 0, which is plain guest memory here).
        pf::logSync();
        MemMap::report(0);
        pf::logSync();
        if (holder != mainCpu)
            logGuestStack("main thread", mainCpu);
    }
    return 0;
}

void WinApplication::noteFrame()
{
    s_lastFrameUs.store(pf::ticksUs(), std::memory_order_relaxed);
    if (!s_watchdog)
    {
        s_mainThread.store(sceKernelGetThreadId(), std::memory_order_relaxed);
        // Guest memory exhausted: the allocating thread holds the execution lock, its stack names the caller.
        // Direct calls leave no return address there (run 34 showed only SEH handlers), so the registers and the raw
        // words go into the log too: arguments and saved registers identify the frame.
        static const auto logRequester = [] {
            const x86::CPU* cpu = s_holderCpu.load(std::memory_order_relaxed);
            if (!cpu)
                return;
            logGuestStack("guest memory requested by", cpu);
            pf::logInfo("memory: requester eax %08x ecx %08x edx %08x ebx %08x ebp %08x esi %08x edi %08x",
                        unsigned(cpu->eax), unsigned(cpu->ecx), unsigned(cpu->edx), unsigned(cpu->ebx),
                        unsigned(cpu->ebp), unsigned(cpu->esi), unsigned(cpu->edi));
            char line[512];
            int length = 0;
            for (x86::reg32 slot = cpu->esp & ~3u; slot < (cpu->esp & ~3u) + 4 * 40; slot += 4)
            {
                x86::reg32 word;
                memcpy(&word, g_guestBase + slot, 4);
                length += snprintf(line + length, sizeof(line) - length, " %08x", unsigned(word));
            }
            pf::logInfo("memory: requester stack words:%s", line);
        };
        MemMap::addReport(logRequester);
        heap::setLargeHook(logRequester);
        // High priority: it sleeps almost always, and threads spinning at normal priority must not starve it.
        s_watchdog = new pf::Thread("stall_watchdog", &watchdogRun, &m_cpu, pf::Thread::PriorityHigh, 16 * 1024,
                                    pf::Thread::CpuAll);
        s_watchdog->start();
    }
}

// Thread blocks and guest stacks of returned runThread calls, reused: timer callbacks run about 125 times a second,
// and finding 1 MB in the guest memory map takes a long first-fit search.
struct ThreadMemory
{
    MemMap* block;
    MemMap* stack;
};
static std::vector<ThreadMemory> s_threadMemory;
static pf::LwMutex s_threadMemoryLock("thread_memory");

// Holds one thread block and stack for the duration of a runThread call.
class ThreadMemoryLease
{
public:
    ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        if (s_threadMemory.empty())
        {
            m_memory = ThreadMemory{ new MemMap(teb::Size, MemMap::Low, pf::perf::AllocThread),
                                     new MemMap(1024 * 1024, MemMap::Low, pf::perf::AllocThread) };
        }
        else
        {
            m_memory = s_threadMemory.back();
            s_threadMemory.pop_back();
        }
    }

    ~ThreadMemoryLease()
    {
        pf::LwLockGuard guard(s_threadMemoryLock);
        s_threadMemory.push_back(m_memory);
    }

    const MemMap& block() const { return *m_memory.block; }
    const MemMap& stack() const { return *m_memory.stack; }

private:
    ThreadMemoryLease(const ThreadMemoryLease&) = delete;
    ThreadMemoryLease& operator=(const ThreadMemoryLease&) = delete;
    ThreadMemory m_memory;
};

static void freeThreadMemory()
{
    pf::LwLockGuard guard(s_threadMemoryLock);
    for (const ThreadMemory& memory : s_threadMemory)
    {
        delete memory.stack;
        delete memory.block;
    }
    s_threadMemory.clear();
}

// The method table doubles while methods register beyond half of it; reserveMethods presizes it.
static const x86::reg32 s_initialMethodSlotBits = 10;

static ForeignCodeResolver s_foreignCodeResolver = nullptr;

void setForeignCodeResolver(ForeignCodeResolver resolver)
{
    s_foreignCodeResolver = resolver;
}

[[noreturn]] static void missingMethod(x86::reg32 address)
{
    pf::logError("dynamic_call: no method registered at guest address 0x%08x", address);
    // Evidence for code the game made at run time: the bytes there, the nearest audio bank header ("ABKC") below it
    // and the call chain on the calling thread's stack (the lock holder).
    if (address >= 0x1000 && address < 0xfff00000)
    {
        const x86::reg8* p = g_guestBase + address;
        pf::logError("dynamic_call: bytes %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x", p[0], p[1], p[2],
                     p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
        for (x86::reg32 at = address & ~3u, limit = 0; at >= 0x1000 && limit < 0x400000; at -= 4, limit += 4)
        {
            x86::reg32 word;
            memcpy(&word, g_guestBase + at, 4);
            if (word == 0x434b4241)
            {
                pf::logError("dynamic_call: ABKC header at 0x%08x (target at +0x%x)", unsigned(at),
                             unsigned(address - at));
                break;
            }
        }
    }
    logGuestStack("caller", s_holderCpu.load(std::memory_order_relaxed));
    __builtin_trap();
}

// Empty slots hold address 0, so only a call to guest address 0 reaches this.
static void callAddressZero(WinApplication* application, x86::CPU& cpu)
{
    RC_USE(application);
    RC_USE(cpu);
    missingMethod(0);
}

GenericResource::GenericResource()
    :   m_resourceIndex(0)
{
    s_resourceCount.fetch_add(1);
}

GenericResource::~GenericResource()
{
    RC_ASSERT(m_resourceIndex == 0);
    s_resourceCount.fetch_sub(1);
}

void GenericResource::setResourceIndex(x86::reg32 resourceIndex)
{
    RC_ASSERT(!resourceIndex || !m_resourceIndex);
    m_resourceIndex = resourceIndex;
}

x86::reg32 GenericResource::getResourceIndex() const
{
    return m_resourceIndex;
}


WinApplication::WinApplication(const char* appName, x86::reg32 baseAddress, const std::vector<Section>& sections)
    :   m_appName(nullptr)
    ,   m_appNameW(nullptr)
    ,   m_env(nullptr)
    ,   m_executionContext(new Mutex("ExecutionContext", Mutex::KernelPriority))
    ,   m_preemptWaiters(0)
    ,   m_contextDepth(0)
    ,   m_resourceContext(new Mutex("Resource"))
    ,   m_methodSlots((x86::reg32(1) << s_initialMethodSlotBits) + 1, MethodSlot{0, &callAddressZero})
    ,   m_methodShift(32 - s_initialMethodSlotBits)
    ,   m_cpu{}
{
    MemMap::init(baseAddress, sections);
    m_appName = new MemMap(4096);
    m_appNameW = new MemMap(4096);
    m_env = new MemMap(4096);
    ddraw::s_dDrawRegistry->registerSymbols(this);
    kernel32::registerModules(this);
    scheduler::configureGameThreads();
    Event::init();
    Timer::init();
    com::InitialiseComMemory(this);
    strcpy(&getMemory<char>(m_appName->getBlockStart()), appName);
    getMemory<char>(m_env->getBlockStart()) = 0;
    WCHAR* wchar = &getMemory<WCHAR>(m_appNameW->getBlockStart());
    for (const char* appNameW = appName; *appNameW; ++wchar, ++appNameW)
    {
        *wchar = WCHAR(*appNameW);
    }
    *wchar = 0;
}

WinApplication::~WinApplication()
{
    // Timer callbacks and guest threads stop first so no guest code runs while resources are freed.
    Timer::fini();
    Thread::terminateAll();
    freeThreadMemory();
    delete m_appNameW;
    delete m_appName;
    // Highest handles first: later objects depend on earlier ones (sound buffers on their device).
    m_resourceContext->lock();
    while (!m_resources.empty())
    {
        std::map<x86::reg32, GenericResource*>::iterator it = --m_resources.end();
        GenericResource* resource = it->second;
        m_resources.erase(it);
        resource->setResourceIndex(0);
        delete resource;
    }
    m_resourceContext->unlock();
    Event::fini();
    MemMap::fini();
    delete m_resourceContext;
    delete m_executionContext;
    RC_ASSERT(s_resourceCount.load() == 0);
}

void WinApplication::setEnvironment(std::initializer_list<const char*> variables)
{
    char* block = &getMemory<char>(m_env->getBlockStart());
    const char* const end = block + m_env->getBlockSize() - 1;
    for (const char* variable : variables)
    {
        const size_t length = strlen(variable) + 1;
        if (block + length >= end)
            break;
        memcpy(block, variable, length);
        block += length;
    }
    *block = 0;
}

x86::reg32 WinApplication::allocateResourceFixed(GenericResource *resource, x86::reg32 fixedHandle)
{
    RC_ASSERT((fixedHandle < 0x20000) || (fixedHandle > 0x20000 + 0x30000));
    m_resourceContext->lock();
    resource->setResourceIndex(fixedHandle);
    bool inserted = m_resources.insert(std::make_pair(fixedHandle, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return fixedHandle;
}

x86::reg32 WinApplication::allocateResource(GenericResource *resource)
{
    m_resourceContext->lock();
    x86::reg32 resourceIndex = ++s_resourceIndex + 0x20000;
    resource->setResourceIndex(resourceIndex);
    bool inserted = m_resources.insert(std::make_pair(resourceIndex, resource)).second;
    RC_ASSERT(inserted);
    m_resourceContext->unlock();
    return resourceIndex;
}

GenericResource* WinApplication::getResource(x86::reg32 resource)
{
    GenericResource* result = nullptr;
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it != m_resources.end())
    {
        result = it->second;
    }
    else
    {
        result = nullptr;
    }
    m_resourceContext->unlock();
    return result;
}

// Deleted outside the resource lock: a destructor may wait for a thread that needs it.
bool WinApplication::freeResource(x86::reg32 resource)
{
    m_resourceContext->lock();
    std::map<x86::reg32, GenericResource*>::iterator it = m_resources.find(resource);
    if (it == m_resources.end())
    {
        m_resourceContext->unlock();
        return false;
    }
    GenericResource* object = it->second;
    m_resources.erase(it);
    m_resourceContext->unlock();
    object->setResourceIndex(0);
    delete object;
    return true;
}

void WinApplication::reserveMethods(x86::reg32 count)
{
    while (count * 4 > methodSlotCount())
        growMethodTable();
}

void WinApplication::registerMethod(x86::reg32 pointer, Method method)
{
    RC_ASSERT(pointer != 0);
    RC_ASSERT(m_methodNames.find(pointer) == m_methodNames.end());
    m_methodNames[pointer] = std::move(method.name);
    if (m_methodNames.size() * 2 > methodSlotCount())
        growMethodTable();
    insertMethod(pointer, method.method);
}

MethodPtr WinApplication::replaceMethod(x86::reg32 pointer, MethodPtr method)
{
    const MethodPtr previous = findMethod(pointer);
    insertMethod(pointer, method);
    return previous;
}

const char* WinApplication::getMethodName(x86::reg32 address) const
{
    std::unordered_map<x86::reg32, std::string>::const_iterator it = m_methodNames.find(address);
    return it != m_methodNames.end() ? it->second.c_str() : nullptr;
}

MethodPtr WinApplication::findMethod(x86::reg32 address) const
{
    const x86::reg32 mask = methodSlotCount() - 1;
    for (x86::reg32 i = methodSlot(address); ; i = (i + 1) & mask)
    {
        const MethodSlot& slot = m_methodSlots[i];
        if (slot.address == address)
            return slot.method;
        if (slot.address == 0)
        {
            // Not cached: foreign code comes and goes with the data that holds it.
            if (const MethodPtr foreign = s_foreignCodeResolver ? s_foreignCodeResolver(address) : nullptr)
                return foreign;
            missingMethod(address);
        }
    }
}

// Stores at the first slot holding the address or empty.
void WinApplication::insertMethod(x86::reg32 address, MethodPtr method)
{
    const x86::reg32 mask = methodSlotCount() - 1;
    x86::reg32 i = methodSlot(address);
    while (m_methodSlots[i].address != 0 && m_methodSlots[i].address != address)
        i = (i + 1) & mask;
    m_methodSlots[i] = MethodSlot{address, method};
}

void WinApplication::growMethodTable()
{
    std::vector<MethodSlot> slots(methodSlotCount() * 2 + 1, MethodSlot{0, &callAddressZero});
    slots.swap(m_methodSlots);
    --m_methodShift;
    for (std::vector<MethodSlot>::const_iterator it = slots.begin(); it != slots.end(); ++it)
    {
        if (it->address)
            insertMethod(it->address, it->method);
    }
}

int WinApplication::runThread(x86::CPU& cpu, x86::reg32 entryPoint, std::initializer_list<x86::reg32> arguments,
                              bool threadLock, bool preempts)
{
    if (!m_cpu.terminate)
    {
        const ThreadMemoryLease memory;
        // A new thread block is zeroed, as on Windows.
        const x86::reg32 block = memory.block().getBlockStart();
        const x86::reg32 stackTop = memory.stack().getBlockStart() + memory.stack().getBlockSize();
        memset(&getMemory<x86::reg8>(block), 0, teb::Size);
        cpu.init(block, entryPoint);
        getMemory<x86::reg32>(block + teb::ExceptionList) = 0xffffffff;
        getMemory<x86::reg32>(block + teb::StackBase) = stackTop;
        getMemory<x86::reg32>(block + teb::StackLimit) = memory.stack().getBlockStart();
        getMemory<x86::reg32>(block + teb::Self) = block;
        getMemory<x86::reg32>(block + teb::ProcessId) = teb::GuestProcessId;
        getMemory<x86::reg32>(block + teb::ThreadId) = Thread::currentThreadId();
        getMemory<Mutex*>(block + ThreadLockOffset) = threadLock ? m_executionContext : nullptr;
        if (preempts)
            scheduler::raiseCurrentLevel(scheduler::LevelAboveNormal);
        getMemory<ThreadControl*>(block + ThreadControlOffset) = scheduler::currentControl();
        LockContext ctx(*this, cpu);
        // A return address slot with the arguments above it, below 4 spare bytes.
        cpu.esp = stackTop - 8 - 4 * x86::reg32(arguments.size());
        x86::reg32 slot = cpu.esp + 4;
        for (x86::reg32 argument : arguments)
        {
            getMemory<x86::reg32>(slot) = argument;
            slot += 4;
        }
        getMemory<x86::reg32>(cpu.esp) = 0;
        dynamic_call(entryPoint, cpu);
        return int(cpu.eax);
    }
    else
    {
        return 0;
    }
}

// Takes the lock, counting a waiter above the normal level so the owner yields to it (allowPreemption).
static void acquire(Mutex* executionContext, scheduler::Level level, std::atomic<int>& preemptWaiters)
{
    if (level == scheduler::LevelNormal)
    {
        executionContext->lock();
        return;
    }
    const bool timed = pf::perf::timed();
    const uint32_t start = timed ? pf::perf::now() : 0;
    s_levelWaiters[level].fetch_add(1, std::memory_order_relaxed);
    preemptWaiters.fetch_add(1, std::memory_order_relaxed);
    // The owner gives the lock up at its next preemption point, usually within microseconds: a waiter that stays
    // awake takes it without a wake-up.
    const uint32_t spinStart = pf::ticksUsLow();
    bool owned = executionContext->tryLock();
    for (uint32_t spin = 1; !owned; ++spin)
    {
        if (!s_lockHeld.load(std::memory_order_relaxed))
            owned = executionContext->tryLock();
        if (!owned && (spin & SpinClockMask) == 0 && pf::ticksUsLow() - spinStart >= PreemptSpinUs)
            break;
    }
    if (!owned)
        executionContext->lock();
    preemptWaiters.fetch_sub(1, std::memory_order_relaxed);
    s_levelWaiters[level].fetch_sub(1, std::memory_order_relaxed);
    if (timed)
        pf::perf::preemptWait(pf::perf::now() - start);
}

// Timing and hitch log accounting of the execution lock. The holder fields change only with the lock held, the game fields only
// on the game thread.
static uint32_t s_heldSince = 0;
static bool     s_gameReleased = false;
static uint32_t s_gameReleasedAt = 0;
static int      s_gameBlock = pf::perf::BlockOther;

// A thread suspended by SuspendThread stops here, without the lock, each time it would take it.
void WinApplication::lockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    RC_ASSERT(executionContext);
    if (!executionContext)
        return;
    ThreadControl* control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
    const scheduler::Level level = scheduler::level(control);
    const bool timed = pf::perf::timed();
    const uint32_t start = timed ? pf::perf::now() : 0;
    acquire(executionContext, level, m_preemptWaiters);
    ++m_contextDepth;
    s_lockHeld.store(true, std::memory_order_relaxed);
    s_holderCpu.store(&cpu, std::memory_order_relaxed);
    while (scheduler::anySuspended() && m_contextDepth == 1 && scheduler::mustPark(control) && !cpu.terminate)
    {
        --m_contextDepth;
        s_lockHeld.store(false, std::memory_order_relaxed);
        executionContext->unlock();
        scheduler::park(control, cpu);
        acquire(executionContext, level, m_preemptWaiters);
        ++m_contextDepth;
        s_lockHeld.store(true, std::memory_order_relaxed);
    }
    if (timed && m_contextDepth == 1)
    {
        const uint32_t acquired = pf::perf::now();
        s_heldSince = acquired;
        if (s_gameReleased && pf::perf::gameHolder(scheduler::holder(control)))
        {
            s_gameReleased = false;
            pf::perf::gameBlocked(pf::perf::Block(s_gameBlock), acquired - s_gameReleasedAt, acquired - start);
        }
    }
}

void WinApplication::unlockContext(const x86::CPU& cpu)
{
    Mutex* executionContext = getMemory<Mutex*>(cpu.efs + ThreadLockOffset);
    RC_ASSERT(executionContext);
    if (executionContext)
    {
        if (pf::perf::timed() && m_contextDepth == 1)
        {
            const uint32_t released = pf::perf::now();
            ThreadControl* control = getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset);
            const pf::perf::Holder holder = scheduler::holder(control);
            pf::perf::held(holder, scheduler::holderSlot(control), released - s_heldSince);
            if (pf::perf::gameHolder(holder))
            {
                s_gameReleased = true;
                s_gameReleasedAt = released;
                s_gameBlock = pf::perf::g_nextBlock;
            }
        }
        pf::perf::g_nextBlock = pf::perf::BlockOther;
        if (--m_contextDepth == 0)
        {
            s_lockHeld.store(false, std::memory_order_relaxed);
            pf::perf::count(pf::perf::CountRelease);
        }
        executionContext->unlock();
    }
}

// Releases the execution lock and queues for it again when a thread of a higher level waits. The lock orders its
// waiters by host priority, which follows the level, so the waiting thread of the highest priority runs next, until
// it blocks or yields itself. Threads without the lock and a lock held more than once do not yield.
void WinApplication::yieldContext(const x86::CPU& cpu)
{
    if (getMemory<Mutex*>(cpu.efs + ThreadLockOffset) != m_executionContext || m_contextDepth != 1)
        return;
    const int level = scheduler::level(getMemory<ThreadControl*>(cpu.efs + ThreadControlOffset));
    auto higherWaits = [level]()
    {
        for (int waiting = level + 1; waiting < scheduler::LevelCount; ++waiting)
        {
            if (s_levelWaiters[waiting].load(std::memory_order_relaxed) != 0)
                return true;
        }
        return false;
    };
    if (!higherWaits())
        return;
    // Setting debug_no_yield (development): never yield; waiters get the lock when the holder blocks or presents.
    // Run 206: the game hangs at boot without it (guest code spin-waits on a higher-level thread).
    static pf::LazyBool s_noYield_setting;
    const bool s_noYield = s_noYield_setting.get("debug_no_yield", false);
    if (s_noYield)
        return;
    int top = level + 1;
    for (int waiting = scheduler::LevelCount - 1; waiting > level; --waiting)
    {
        if (s_levelWaiters[waiting].load(std::memory_order_relaxed) != 0)
        {
            top = waiting;
            break;
        }
    }
    const uint32_t yieldStart = pf::perf::now();
    const bool timed = pf::perf::timed();
    const uint32_t start = timed ? pf::perf::now() : 0;
    pf::perf::count(pf::perf::CountYield);
    pf::perf::blocking(pf::perf::BlockPreempt);
    unlockContext(cpu);
    // A waiter that spins for the lock takes it now; this thread must not take it back first.
    const uint32_t spinStart = pf::ticksUsLow();
    for (uint32_t spin = 1; higherWaits(); ++spin)
    {
        if ((spin & SpinClockMask) == 0 && pf::ticksUsLow() - spinStart >= YieldSpinUs)
            break;
    }
    lockContext(cpu);
    if (timed)
        pf::perf::add(pf::perf::Preempted, pf::perf::now() - start);
    // timing_log: yields per waiting level (count, total and longest hand-back), every 10 s.
    if (timed)
    {
        static uint32_t s_count[scheduler::LevelCount], s_us[scheduler::LevelCount], s_max[scheduler::LevelCount];
        static uint32_t s_since = 0;
        const uint32_t now = pf::perf::now(), took = now - yieldStart;
        ++s_count[top];
        s_us[top] += took;
        s_max[top] = std::max(s_max[top], took);
        if (now - s_since >= 10000000)
        {
            pf::logInfo("yield: by waiting level (count, total ms, max ms) above-normal %u %.1f %.1f, highest %u %.1f %.1f, "
                        "time-critical %u %.1f %.1f",
                        s_count[1], s_us[1] / 1000.0, s_max[1] / 1000.0, s_count[2], s_us[2] / 1000.0,
                        s_max[2] / 1000.0, s_count[3], s_us[3] / 1000.0, s_max[3] / 1000.0);
            for (int i = 0; i < scheduler::LevelCount; ++i)
                s_count[i] = s_us[i] = s_max[i] = 0;
            s_since = now;
        }
    }
}

bool WinApplication::holdsContext(const x86::CPU& cpu)
{
    return getMemory<Mutex*>(cpu.efs + ThreadLockOffset) != nullptr;
}

// Runtime functions the unlocked thread calls without the execution lock: they keep no guest-visible state of
// their own beyond host-synchronised objects (critical sections are host mutexes; the interlocked functions use
// x86::AtomicSection like the generated lock-prefixed instructions), or they are pure. Enter/TryEnter
// CriticalSection and Sleep release the lock only when the caller holds it (holdsContext). Found in run 85: these
// were ~3,500 of the audio thread's ~5,300 lock grabs per second.
static bool lockFreeName(const char* name)
{
    static const char* const Names[] =
    {
        "<kernel32>EnterCriticalSection", "<kernel32>LeaveCriticalSection", "<kernel32>TryEnterCriticalSection",
        "<kernel32>InterlockedIncrement", "<kernel32>InterlockedDecrement", "<kernel32>InterlockedExchange",
        "<kernel32>InterlockedExchangeAdd", "<kernel32>InterlockedCompareExchange",
        "<kernel32>QueryPerformanceCounter", "<kernel32>GetTickCount", "<kernel32>Sleep",
        "<winmm>timeGetTime",
        "<msvcr71>floor", "<msvcr71>ceil", "<msvcr71>memmove", "<msvcr71>memcpy",
        // AudioBuffer takes the audio device mutex; the COM object is in guest memory (no shared lookup). The Mac
        // port's free-running mixer calls the same three ~200/s (docs/REFERENCE-CLUES.md).
        "IDirectSoundBuffer::GetCurrentPosition", "IDirectSoundBuffer::Lock", "IDirectSoundBuffer::Unlock",
    };
    if (!name)
        return false;
    for (const char* lockFree : Names)
    {
        if (strcmp(name, lockFree) == 0)
            return true;
    }
    return false;
}

bool WinApplication::takeContext(x86::CPU& cpu)
{
    if (holdsContext(cpu))
        return false;
    getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = m_executionContext;
    lockContext(cpu);
    return true;
}

void WinApplication::releaseContext(x86::CPU& cpu)
{
    unlockContext(cpu);
    getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = nullptr;
}

void WinApplication::lockedCall(x86::reg32 address, x86::CPU& cpu)
{
    if (!holdsContext(cpu) && m_lockFreeMethod)
    {
        const MethodSlot* entry = &m_methodSlots[methodSlot(address)];
        const MethodPtr method = entry[0].address == address ? entry[0].method
                               : entry[1].address == address ? entry[1].method : findMethod(address);
        if (method == m_lockFreeMethod)
        {
            method(this, cpu);
            return;
        }
    }
    if (!holdsContext(cpu))
    {
        bool lockFree = false;
        unsigned i = 0;
        while (i < m_lockFreeCount && m_lockFree[i].address != address)
            ++i;
        if (i < m_lockFreeCount)
            lockFree = m_lockFree[i].lockFree;
        else
        {
            lockFree = pf::Config::getBool("audio_lock_free", true) && lockFreeName(getMethodName(address));
            if (m_lockFreeCount < sizeof(m_lockFree) / sizeof(m_lockFree[0]))
                m_lockFree[m_lockFreeCount++] = {address, lockFree};
            if (lockFree)
                pf::logInfo("perf: the unlocked thread calls 0x%08x %s without the lock", unsigned(address),
                            getMethodName(address));
        }
        if (lockFree)
        {
            const MethodSlot* entry = &m_methodSlots[methodSlot(address)];
            const MethodPtr method = entry[0].address == address ? entry[0].method
                                   : entry[1].address == address ? entry[1].method : findMethod(address);
            method(this, cpu);
            return;
        }
    }
    const bool take = static_cast<Mutex*>(getMemory<Mutex*>(cpu.efs + ThreadLockOffset)) == nullptr;
    const bool timed = take && pf::perf::timed();
    const uint32_t start = timed ? pf::perf::now() : 0;
    uint32_t locked = start;
    if (take)
    {
        getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = m_executionContext;
        const uint32_t waitStart = pf::ticksUsLow();
        lockContext(cpu);
        if (timed)
            locked = pf::perf::now();
        // Long waits of the unlocked (audio) thread for the execution lock: it may hold EA's sound lock meanwhile.
        const uint32_t waited = pf::ticksUsLow() - waitStart;
        static uint32_t s_longWaits = 0;
        if (waited >= 20000 && s_longWaits < 200)
        {
            ++s_longWaits;
            pf::logInfo("perf: unlocked thread waited %.1f ms for the execution lock to call 0x%08x %s",
                        waited / 1000.0, unsigned(address), getMethodName(address) ? getMethodName(address) : "");
        }
    }
    allowPreemption(cpu);
    const MethodSlot* entry = &m_methodSlots[methodSlot(address)];
    const MethodPtr method = entry[0].address == address ? entry[0].method
                           : entry[1].address == address ? entry[1].method : findMethod(address);
    method(this, cpu);
    if (take)
    {
        unlockContext(cpu);
        getMemory<Mutex*>(cpu.efs + ThreadLockOffset) = nullptr;
        if (timed)
        {
            pf::perf::lockedTarget(address, locked - start, pf::perf::now() - locked);
            // Names each runtime target once (the unlocked thread is the only caller).
            static x86::reg32 s_named[256];
            static unsigned s_namedCount = 0;
            if (s_namedCount < 256 && std::find(s_named, s_named + s_namedCount, address) == s_named + s_namedCount)
            {
                s_named[s_namedCount++] = address;
                if (const char* name = getMethodName(address))
                    pf::logInfo("perf: unlocked thread's locked call target 0x%08x = %s", unsigned(address), name);
            }
        }
    }
}

void WinApplication::terminate()
{
    m_cpu.terminate = true;
}

}

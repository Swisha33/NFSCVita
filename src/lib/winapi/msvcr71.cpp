// msvcr71.dll for NFSC.exe: start-up, memory, strings, characters, math, time, threads, setjmp/longjmp and the C++
// exception entry points. Standard I/O and the printf/scanf formatting are in msvcr71_stdio.cpp.
// Conventions: include/winapi/msvcr71.h.

#include <winapi/msvcr71.h>
#include "crt.h"

#include <lib/heap.h>
#include <lib/teb.h>
#include <winapi/kernel32.h>
#include <platform/config.h>
#include <platform/log.h>
#include <platform/sync.h>
#include <recomp.h>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>
#include <unordered_map>
#include <vector>

namespace win32 { namespace crt
{

namespace
{

pf::LwMutex s_mutex("msvcr71");
std::map<std::string, reg32> s_statics;
std::unordered_map<reg32, reg32> s_aligned;     // aligned block -> heap block, for _aligned_malloc above 16
std::vector<reg32> s_onexit;                    // _onexit / __dllonexit functions, called last first
bool s_exited = false;

}

reg32 callGuest(WinApplication* app, x86::CPU& cpu, reg32 function, std::initializer_list<reg32> arguments)
{
    const reg32 esp = cpu.esp, ebx = cpu.ebx, esi = cpu.esi, edi = cpu.edi, ebp = cpu.ebp;
    cpu.esp -= 4 * reg32(arguments.size()) + 4;
    reg32 at = cpu.esp;
    st32(at, 0);                    // return address: generated code returns to its host caller
    for (reg32 a : arguments)
        st32(at += 4, a);
    cpu.ip = function;
    app->dynamic_call(function, cpu);
    const reg32 result = cpu.eax;
    cpu.esp = esp;
    cpu.ebx = ebx;
    cpu.esi = esi;
    cpu.edi = edi;
    cpu.ebp = ebp;
    return result;
}

reg32 allocate(reg32 size, bool zero)
{
    return heap::allocate(size ? size : 1, zero);
}

void release(reg32 address)
{
    if (address && !heap::release(address))
        pf::logError("msvcr71: free of 0x%08x, not a heap block", unsigned(address));
}

reg32 staticBlock(const char* name, reg32 size)
{
    pf::LwLockGuard guard(s_mutex);
    reg32& block = s_statics[name];
    if (!block)
        block = heap::allocate(size, true);
    return block;
}

void setErrno(int value)
{
    st32(staticBlock("errno", 4), reg32(value));
}

void traceText(const char* fn, reg32 dst, const x86::CPU& cpu)
{
    static const std::string s_text = pf::Config::getString("debug_find", "");
    if (s_text.empty() || !dst || !strstr(ptr(dst), s_text.c_str()))
        return;
    char words[200];
    int at = 0;
    for (int i = -8; i < 40 && at < int(sizeof(words)) - 12; ++i)
    {
        const reg32 w = ld32(cpu.esp + 4 * i);
        if (w >= 0x401000 && w < 0x9c1000)
            at += snprintf(words + at, sizeof(words) - at, " %d:%06x", i, unsigned(w));
    }
    words[at] = 0;
    pf::logInfo("debug_find: %s wrote \"%.80s\" at 0x%08x (args 0x%08x 0x%08x 0x%08x); code on stack:%s", fn,
                ptr(dst), unsigned(dst), unsigned(arg(cpu, 0)), unsigned(arg(cpu, 1)), unsigned(arg(cpu, 2)), words);
}

void traceWide(const char* fn, reg32 dst, const x86::CPU& cpu)
{
    static const std::string s_text = pf::Config::getString("debug_find", "");
    if (s_text.empty() || !dst)
        return;
    std::string narrow;
    for (reg32 i = 0; i < 200; ++i)
    {
        const uint16_t c = uint16_t(ld32(dst + 2 * i) & 0xffff);
        if (!c)
            break;
        narrow += c < 128 ? char(c) : '?';
    }
    if (narrow.find(s_text) != std::string::npos)
        pf::logInfo("debug_find: %s wrote L\"%.80s\" at 0x%08x", fn, narrow.c_str(), unsigned(dst));
}

}}

namespace win32 { namespace msvcr71
{

using namespace crt;

namespace
{

// std::exception: vtable, what, own copy of what. Its vtable lives in guest memory with native entries at fake
// addresses in the unused part of the executable's header page.
const reg32 ExceptionDeletingDtor = 0x00400f00;
const reg32 ExceptionWhat = 0x00400f04;
reg32 s_exceptionVtable = 0;

void exceptionDestroy(reg32 self)
{
    if (ld32(self + 8))
        release(ld32(self + 4));
    st32(self + 4, 0);
    st32(self + 8, 0);
}

// thiscall (flags): destroy, free when flags bit 0; returns this, pops its argument.
void exceptionDeletingDtor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx;
    const reg32 flags = arg(cpu, 0);
    exceptionDestroy(self);
    if ((flags & 1) && heap::size(self) != 0xffffffff)
        heap::release(self);
    cpu.eax = self;
    cpu.esp += 8;
}

// thiscall what(): the message or a fixed text.
void exceptionWhat(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 what = ld32(cpu.ecx + 4);
    if (what)
        cpu.eax = what;
    else
    {
        const reg32 text = staticBlock("exception.what", 32);
        strcpy(ptr(text), "Unknown exception");
        cpu.eax = text;
    }
    cpu.esp += 4;
}

reg32 s_argv = 0, s_envp = 0;

void runOnExit(WinApplication* app, x86::CPU& cpu)
{
    std::vector<reg32> functions;
    {
        pf::LwLockGuard guard(s_mutex);
        if (s_exited)
            return;
        s_exited = true;
        functions.swap(s_onexit);
    }
    for (auto it = functions.rbegin(); it != functions.rend(); ++it)
        callGuest(app, cpu, *it, {});
}

// Pushes a double result on the guest x87 stack; eax stays.
reg32 pushResult(x86::CPU& cpu, double value)
{
    cpu.fpu.push(x86::Float(value));
    return cpu.eax;
}

// ---- time --------------------------------------------------------------------------------------------------------

// Seconds since 1970 (UTC) from the guest's clock.
int64_t now(WinApplication* app, x86::CPU& cpu, uint32_t* milliseconds = nullptr)
{
    FILETIME ft;
    kernel32::GetSystemTimeAsFileTime(app, cpu, &ft);
    const int64_t ticks = (int64_t(ft.dwHighDateTime) << 32 | ft.dwLowDateTime) - 116444736000000000LL;
    if (milliseconds)
        *milliseconds = uint32_t(ticks / 10000 % 1000);
    return ticks / 10000000;
}

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + int64_t(doe) - 719468;
}

// struct tm of the C runtime: nine ints (sec, min, hour, mday, mon, year - 1900, wday, yday, isdst). Local time is
// UTC: the Vita's time zone is not passed to the guest.
reg32 brokenDown(int64_t t, const char* name)
{
    if (t < 0)
        return 0;
    int64_t days = t / 86400;
    const int64_t secs = t % 86400;
    const int64_t z = days + 719468;
    const int64_t era = z / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int64_t y = int64_t(yoe) + era * 400 + (m <= 2);
    const reg32 tm = staticBlock(name, 36);
    const int32_t fields[9] = { int32_t(secs % 60), int32_t(secs / 60 % 60), int32_t(secs / 3600), int32_t(d),
                                int32_t(m - 1), int32_t(y - 1900), int32_t((days + 4) % 7),
                                int32_t(days - daysFromCivil(y, 1, 1)), 0 };
    memcpy(ptr(tm), fields, sizeof(fields));
    return tm;
}

// mktime: normalises the guest struct tm and returns its time.
int64_t fromBrokenDown(reg32 tm)
{
    int32_t f[9];
    memcpy(f, ptr(tm), sizeof(f));
    int64_t months = int64_t(f[5]) * 12 + f[4];
    const int64_t year = 1900 + (months >= 0 ? months / 12 : (months - 11) / 12);
    const unsigned month = unsigned(months - (year - 1900) * 12) + 1;
    const int64_t t = (daysFromCivil(year, month, 1) + f[3] - 1) * 86400 + int64_t(f[2]) * 3600 + int64_t(f[1]) * 60
                    + f[0];
    const reg32 normal = brokenDown(t, "tm.mktime");
    if (normal)
        memcpy(ptr(tm), ptr(normal), 36);
    return t;
}

// ---- x87 control word <-> _controlfp bits -------------------------------------------------------------------------

// _controlfp bits: exception masks _MCW_EM 0x8001f, rounding _MCW_RC 0x300, precision _MCW_PC 0x30000.
reg32 toAbstract(uint16_t cw)
{
    reg32 r = 0;
    if (cw & 0x01) r |= 0x10;       // invalid
    if (cw & 0x02) r |= 0x80000;    // denormal
    if (cw & 0x04) r |= 0x08;       // zero divide
    if (cw & 0x08) r |= 0x04;       // overflow
    if (cw & 0x10) r |= 0x02;       // underflow
    if (cw & 0x20) r |= 0x01;       // inexact
    r |= reg32((cw >> 10) & 3) << 8;
    switch ((cw >> 8) & 3)
    {
    case 0:  r |= 0x20000; break;   // 24 bits
    case 2:  r |= 0x10000; break;   // 53 bits
    default: break;                 // 64 bits
    }
    return r;
}

uint16_t toControlWord(reg32 a, uint16_t cw)
{
    uint16_t r = cw & ~0x0f3f;
    if (a & 0x10) r |= 0x01;
    if (a & 0x80000) r |= 0x02;
    if (a & 0x08) r |= 0x04;
    if (a & 0x04) r |= 0x08;
    if (a & 0x02) r |= 0x10;
    if (a & 0x01) r |= 0x20;
    r |= uint16_t(((a >> 8) & 3) << 10);
    r |= uint16_t((a & 0x20000) ? 0 : (a & 0x10000) ? 2 << 8 : 3 << 8);
    return r;
}

reg32 controlFp(x86::CPU& cpu, reg32 value, reg32 mask)
{
    const reg32 current = toAbstract(cpu.fpu.control.word);
    if (mask)
        cpu.fpu.control.word = toControlWord((current & ~mask) | (value & mask), cpu.fpu.control.word);
    return toAbstract(cpu.fpu.control.word);
}

uint32_t s_rand = 1;

}

// ---- data exports and start-up -------------------------------------------------------------------------------------

x86::reg32 data_crt__iob(WinApplication* app)
{
    // FILE: _ptr, _cnt, _base, _flag, _file, _charbuf, _bufsiz, _tmpfname (32 bytes); stdin, stdout, stderr.
    const reg32 iob = staticBlock("_iob", 20 * 32);
    for (reg32 i = 0; i < 3; ++i)
        st32(iob + 32 * i + 16, i);
    s_exceptionVtable = staticBlock("exception.vftable", 8);
    st32(s_exceptionVtable, ExceptionDeletingDtor);
    st32(s_exceptionVtable + 4, ExceptionWhat);
    app->registerMethod(ExceptionDeletingDtor, {"<msvcr71>exception::`scalar deleting destructor'",
                                                &exceptionDeletingDtor});
    app->registerMethod(ExceptionWhat, {"<msvcr71>exception::what", &exceptionWhat});
    return iob;
}

x86::reg32 data_crt__adjust_fdiv(WinApplication* app)
{
    RC_USE(app);
    return staticBlock("_adjust_fdiv", 4);
}

// int __getmainargs(int* argc, char*** argv, char*** envp, int doWildCard, _startupinfo*)
x86::reg32 crt___getmainargs(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    if (!s_argv)
    {
        const reg32 text = staticBlock("argv0", 16);
        strcpy(ptr(text), "C:\\NFSC.exe");
        s_argv = staticBlock("argv", 8);
        st32(s_argv, text);
        s_envp = staticBlock("envp", 4);
    }
    if (arg(cpu, 0)) st32(arg(cpu, 0), 1);
    if (arg(cpu, 1)) st32(arg(cpu, 1), s_argv);
    if (arg(cpu, 2)) st32(arg(cpu, 2), s_envp);
    return 0;
}

x86::reg32 crt___p___initenv(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    const reg32 initenv = staticBlock("__initenv", 4);
    st32(initenv, s_envp);
    return initenv;
}

x86::reg32 crt___p__commode(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return staticBlock("_commode", 4);
}

x86::reg32 crt___p__fmode(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return staticBlock("_fmode", 4);
}

x86::reg32 crt___set_app_type(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return cpu.eax;
}

x86::reg32 crt___setusermatherr(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return cpu.eax;
}

// void _initterm(PVFV* begin, PVFV* end): calls every non-null entry.
x86::reg32 crt__initterm(WinApplication* app, x86::CPU& cpu)
{
    for (reg32 at = arg(cpu, 0), end = arg(cpu, 1); at < end; at += 4)
    {
        const reg32 function = ld32(at);
        if (function)
            callGuest(app, cpu, function, {});
    }
    return 0;
}

x86::reg32 crt__onexit(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    pf::LwLockGuard guard(s_mutex);
    s_onexit.push_back(arg(cpu, 0));
    return arg(cpu, 0);
}

// _onexit_t __dllonexit(_onexit_t, PVFV** begin, PVFV** end): the table pointers are the caller's; one list here.
x86::reg32 crt___dllonexit(WinApplication* app, x86::CPU& cpu)
{
    return crt__onexit(app, cpu);
}

x86::reg32 crt_exit(WinApplication* app, x86::CPU& cpu)
{
    const reg32 code = arg(cpu, 0);
    pf::logInfo("msvcr71: exit(%d)", int(code));
    runOnExit(app, cpu);
    kernel32::ExitProcess(app, cpu, code);
    return 0;
}

x86::reg32 crt__exit(WinApplication* app, x86::CPU& cpu)
{
    pf::logInfo("msvcr71: _exit(%d)", int(arg(cpu, 0)));
    kernel32::ExitProcess(app, cpu, arg(cpu, 0));
    return 0;
}

x86::reg32 crt__cexit(WinApplication* app, x86::CPU& cpu)
{
    runOnExit(app, cpu);
    return 0;
}

x86::reg32 crt__c_exit(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

x86::reg32 crt__amsg_exit(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    pf::fatal("msvcr71: runtime error R60%02u", unsigned(arg(cpu, 0)));
}

x86::reg32 crt___security_error_handler(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    pf::fatal("msvcr71: buffer overrun detected (security error %d)", int(arg(cpu, 0)));
}

x86::reg32 crt__purecall(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    pf::fatal("msvcr71: pure virtual function call (return address 0x%08x)", unsigned(ld32(cpu.esp)));
}

x86::reg32 crt_terminate(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    pf::fatal("msvcr71: terminate() called");
}

x86::reg32 crt_raise(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int signal = int(arg(cpu, 0));
    if (signal == 22)
        pf::fatal("msvcr71: abort() (SIGABRT)");
    pf::logError("msvcr71: raise(%d) ignored", signal);
    return 0;
}

// ---- C++ exceptions and SEH ------------------------------------------------------------------------------------------

// __stdcall _CxxThrowException(object, ThrowInfo*). Throwing through recompiled code is not supported yet: catching
// needs MSVC's frame handler to resume inside a translated function. The log names the type and the message.
x86::reg32 crt__CxxThrowException(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 object = arg(cpu, 0), info = arg(cpu, 1);
    std::string types;
    if (info && ld32(info + 12))
    {
        const reg32 array = ld32(info + 12);
        for (reg32 i = 0, n = std::min<reg32>(ld32(array), 8); i < n; ++i)
        {
            const reg32 catchable = ld32(array + 4 + 4 * i);
            const reg32 type = catchable ? ld32(catchable + 4) : 0;
            if (type)
                types += std::string(i ? ", " : "") + ptr(type + 8);
        }
    }
    const reg32 what = object && ld32(object) == s_exceptionVtable ? ld32(object + 4) : 0;
    pf::fatal("msvcr71: C++ exception thrown from 0x%08x: %s%s%s (not supported yet)", unsigned(ld32(cpu.esp)),
              types.empty() ? "unknown type" : types.c_str(), what ? ": " : "", what ? ptr(what) : "");
}

// SEH handlers of C++ and __try frames: nothing is caught (ExceptionContinueSearch).
x86::reg32 crt___CxxFrameHandler(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

x86::reg32 crt__except_handler3(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 1;
}

x86::reg32 crt__XcptFilter(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

// exception::exception() (thiscall)
x86::reg32 crt_exception_ctor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx;
    st32(self, s_exceptionVtable);
    st32(self + 4, 0);
    st32(self + 8, 0);
    return self;
}

// exception::exception(const exception&) (thiscall, pops its argument)
x86::reg32 crt_exception_ctor_copy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx, other = arg(cpu, 0);
    cpu.esp += 4;
    st32(self, s_exceptionVtable);
    const reg32 what = other ? ld32(other + 4) : 0;
    if (other && ld32(other + 8) && what)
    {
        const reg32 copy = allocate(reg32(strlen(ptr(what)) + 1));
        strcpy(ptr(copy), ptr(what));
        st32(self + 4, copy);
        st32(self + 8, 1);
    }
    else
    {
        st32(self + 4, what);
        st32(self + 8, 0);
    }
    return self;
}

x86::reg32 crt_exception_dtor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    st32(cpu.ecx, s_exceptionVtable);
    exceptionDestroy(cpu.ecx);
    return cpu.eax;
}

x86::reg32 crt_type_info_dtor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return cpu.eax;
}

// ---- setjmp / longjmp ------------------------------------------------------------------------------------------------

// int _setjmp3(jmp_buf, int count, ...): MSVC's _JUMP_BUFFER (Ebp, Ebx, Edi, Esi, Esp, Eip, Registration, TryLevel).
// The call site keeps a host jump buffer (tools/recompiler/generate.py SETJMP); this records the guest registers.
x86::reg32 crt__setjmp3(WinApplication* app, x86::CPU& cpu)
{
    const reg32 buffer = arg(cpu, 0);
    st32(buffer + 0, cpu.ebp);
    st32(buffer + 4, cpu.ebx);
    st32(buffer + 8, cpu.edi);
    st32(buffer + 12, cpu.esi);
    st32(buffer + 16, cpu.esp + 4);     // the caller's esp after the call returns
    st32(buffer + 20, ld32(cpu.esp));
    st32(buffer + 24, app->getMemory<x86::reg32>(cpu.efs + teb::ExceptionList));
    st32(buffer + 28, 0xffffffff);
    return 0;
}

// void longjmp(jmp_buf, int): the registers setjmp saved, then the host jump back to the setjmp call.
x86::reg32 crt_longjmp(WinApplication* app, x86::CPU& cpu)
{
    const reg32 buffer = arg(cpu, 0), value = arg(cpu, 1);
    app->getMemory<x86::reg32>(cpu.efs + teb::ExceptionList) = ld32(buffer + 24);
    cpu.ebp = ld32(buffer + 0);
    cpu.ebx = ld32(buffer + 4);
    cpu.edi = ld32(buffer + 8);
    cpu.esi = ld32(buffer + 12);
    cpu.esp = ld32(buffer + 16);
    cpu.eax = value ? value : 1;
    rc::longJump(app, cpu, buffer);
}

// ---- threads -------------------------------------------------------------------------------------------------------

// uintptr_t _beginthreadex(security, stack size, start, argument, flags, unsigned* thread id)
x86::reg32 crt__beginthreadex(WinApplication* app, x86::CPU& cpu)
{
    const reg32 idAddress = arg(cpu, 5);
    return kernel32::CreateThread(app, cpu, nullptr, arg(cpu, 1), arg(cpu, 2), arg(cpu, 3), arg(cpu, 4),
                                  ptr<DWORD>(idAddress));
}

x86::reg32 crt__endthreadex(WinApplication* app, x86::CPU& cpu)
{
    kernel32::ExitThread(app, cpu, arg(cpu, 0));
    return 0;
}

// ---- memory --------------------------------------------------------------------------------------------------------

x86::reg32 crt_malloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 block = heap::allocate(std::max<reg32>(arg(cpu, 0), 1), false);
    if (!block)
        setErrno(12);
    return block;
}

x86::reg32 crt_calloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const uint64_t size = uint64_t(arg(cpu, 0)) * arg(cpu, 1);
    if (size > 0x7fffffff)
        return 0;
    return heap::allocate(std::max<reg32>(reg32(size), 1), true);
}

static reg32 s_foreignFree = 0;

void setForeignFree(x86::reg32 function)
{
    s_foreignFree = function;
}

x86::reg32 crt_free(WinApplication* app, x86::CPU& cpu)
{
    const reg32 block = arg(cpu, 0);
    if (block && s_foreignFree && heap::size(block) == 0xffffffff)
        return callGuest(app, cpu, s_foreignFree, {block});
    release(block);
    return 0;
}

static reg32 reallocate(reg32 block, reg32 size)
{
    if (!block)
        return heap::allocate(std::max<reg32>(size, 1), false);
    if (!size)
    {
        release(block);
        return 0;
    }
    const reg32 old = heap::size(block);
    if (old == 0xffffffff)
    {
        pf::logError("msvcr71: realloc of 0x%08x, not a heap block", unsigned(block));
        return 0;
    }
    const reg32 grown = heap::allocate(size, false);
    if (!grown)
        return 0;
    memcpy(ptr(grown), ptr(block), std::min(old, size));
    heap::release(block);
    return grown;
}

x86::reg32 crt_realloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reallocate(arg(cpu, 0), arg(cpu, 1));
}

static reg32 alignedAllocate(reg32 size, reg32 alignment)
{
    if (alignment <= 16)
        return heap::allocate(std::max<reg32>(size, 1), false);
    const reg32 block = heap::allocate(size + alignment, false);
    if (!block)
        return 0;
    const reg32 aligned = (block + alignment - 1) & ~(alignment - 1);
    pf::LwLockGuard guard(s_mutex);
    s_aligned[aligned] = block;
    return aligned;
}

static void alignedRelease(reg32 aligned)
{
    if (!aligned)
        return;
    reg32 block = aligned;
    {
        pf::LwLockGuard guard(s_mutex);
        const auto it = s_aligned.find(aligned);
        if (it != s_aligned.end())
        {
            block = it->second;
            s_aligned.erase(it);
        }
    }
    release(block);
}

x86::reg32 crt__aligned_malloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return alignedAllocate(arg(cpu, 0), arg(cpu, 1));
}

x86::reg32 crt__aligned_free(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    alignedRelease(arg(cpu, 0));
    return 0;
}

// void* _aligned_realloc(void*, size_t, size_t alignment)
x86::reg32 crt__aligned_realloc(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 old = arg(cpu, 0), size = arg(cpu, 1), alignment = arg(cpu, 2);
    if (!old)
        return alignedAllocate(size, alignment);
    if (!size)
    {
        alignedRelease(old);
        return 0;
    }
    reg32 block = old;
    {
        pf::LwLockGuard guard(s_mutex);
        const auto it = s_aligned.find(old);
        if (it != s_aligned.end())
            block = it->second;
    }
    const reg32 blockSize = heap::size(block);
    const reg32 oldSize = blockSize == 0xffffffff ? 0 : blockSize - (old - block);
    const reg32 grown = alignedAllocate(size, alignment);
    if (grown)
    {
        memcpy(ptr(grown), ptr(old), std::min(oldSize, size));
        alignedRelease(old);
    }
    return grown;
}

x86::reg32 crt_memcpy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    if (arg(cpu, 2))
        memmove(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2));
    return arg(cpu, 0);
}

x86::reg32 crt_memmove(WinApplication* app, x86::CPU& cpu)
{
    return crt_memcpy(app, cpu);
}

x86::reg32 crt_memset(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    if (arg(cpu, 2))
        memset(ptr(arg(cpu, 0)), int(arg(cpu, 1) & 0xff), arg(cpu, 2));
    return arg(cpu, 0);
}

x86::reg32 crt_memcmp(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return arg(cpu, 2) ? reg32(memcmp(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2))) : 0;
}

// ---- strings -------------------------------------------------------------------------------------------------------

x86::reg32 crt_strlen(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reg32(strlen(ptr(arg(cpu, 0))));
}

x86::reg32 crt_strcmp(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int r = strcmp(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)));
    return reg32(r < 0 ? -1 : r > 0 ? 1 : 0);
}

x86::reg32 crt_strcoll(WinApplication* app, x86::CPU& cpu)
{
    return crt_strcmp(app, cpu);
}

x86::reg32 crt_strncmp(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int r = strncmp(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2));
    return reg32(r < 0 ? -1 : r > 0 ? 1 : 0);
}

x86::reg32 crt__strcmpi(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int r = strcasecmp(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)));
    return reg32(r < 0 ? -1 : r > 0 ? 1 : 0);
}

x86::reg32 crt__strnicmp(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int r = strncasecmp(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2));
    return reg32(r < 0 ? -1 : r > 0 ? 1 : 0);
}


x86::reg32 crt_strncpy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    strncpy(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2));
    traceText("strncpy", arg(cpu, 0), cpu);
    return arg(cpu, 0);
}

x86::reg32 crt_strncat(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    strncat(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), arg(cpu, 2));
    traceText("strncat", arg(cpu, 0), cpu);
    return arg(cpu, 0);
}

x86::reg32 crt__mbscpy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    memmove(ptr(arg(cpu, 0)), ptr(arg(cpu, 1)), strlen(ptr(arg(cpu, 1))) + 1);
    traceText("_mbscpy", arg(cpu, 0), cpu);
    return arg(cpu, 0);
}

x86::reg32 crt__mbscat(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    char* dst = ptr(arg(cpu, 0));
    memmove(dst + strlen(dst), ptr(arg(cpu, 1)), strlen(ptr(arg(cpu, 1))) + 1);
    traceText("_mbscat", arg(cpu, 0), cpu);
    return arg(cpu, 0);
}

x86::reg32 crt_strchr(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return guest(strchr(ptr(arg(cpu, 0)), int(arg(cpu, 1) & 0xff)));
}

x86::reg32 crt_strrchr(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return guest(strrchr(ptr(arg(cpu, 0)), int(arg(cpu, 1) & 0xff)));
}

x86::reg32 crt_strstr(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return guest(strstr(ptr(arg(cpu, 0)), ptr(arg(cpu, 1))));
}

x86::reg32 crt_strcspn(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reg32(strcspn(ptr(arg(cpu, 0)), ptr(arg(cpu, 1))));
}

// char* strtok(char*, const char*): the position is a static of the runtime (one for all threads, as here).
x86::reg32 crt_strtok(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 state = staticBlock("strtok", 4);
    reg32 s = arg(cpu, 0) ? arg(cpu, 0) : ld32(state);
    const char* delimiters = ptr(arg(cpu, 1));
    if (!s)
        return 0;
    s += reg32(strspn(ptr(s), delimiters));
    if (!*ptr(s))
    {
        st32(state, 0);
        return 0;
    }
    const reg32 end = s + reg32(strcspn(ptr(s), delimiters));
    if (*ptr(end))
    {
        *ptr(end) = 0;
        st32(state, end + 1);
    }
    else
        st32(state, 0);
    return s;
}

x86::reg32 crt_strerror(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 text = staticBlock("strerror", 96);
    snprintf(ptr(text), 96, "%s", strerror(int(arg(cpu, 0))));
    return text;
}

// No environment variables are set for the guest.
x86::reg32 crt_getenv(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return 0;
}

x86::reg32 crt_wcslen(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    reg32 n = 0;
    for (reg32 s = arg(cpu, 0); app->getMemory<x86::reg16>(s + 2 * n); ++n)
        ;
    return n;
}

x86::reg32 crt_wcscpy(WinApplication* app, x86::CPU& cpu)
{
    const reg32 dst = arg(cpu, 0), src = arg(cpu, 1);
    reg32 i = 0;
    for (;; ++i)
    {
        const x86::reg16 c = app->getMemory<x86::reg16>(src + 2 * i);
        app->getMemory<x86::reg16>(dst + 2 * i) = c;
        if (!c)
            break;
    }
    traceWide("wcscpy", dst, cpu);
    return dst;
}

// ---- numbers and characters ------------------------------------------------------------------------------------------

x86::reg32 crt_atoi(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reg32(atoi(ptr(arg(cpu, 0))));
}

x86::reg32 crt_strtol(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    char* end = nullptr;
    const long value = strtol(ptr(arg(cpu, 0)), &end, int(arg(cpu, 2)));
    if (arg(cpu, 1))
        st32(arg(cpu, 1), guest(end));
    return reg32(int32_t(std::max<long>(std::min<long>(value, INT32_MAX), INT32_MIN)));
}

x86::reg32 crt_strtoul(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    char* end = nullptr;
    const unsigned long value = strtoul(ptr(arg(cpu, 0)), &end, int(arg(cpu, 2)));
    if (arg(cpu, 1))
        st32(arg(cpu, 1), guest(end));
    return reg32(std::min<unsigned long>(value, UINT32_MAX));
}

x86::reg32 crt_abs(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int32_t v = int32_t(arg(cpu, 0));
    return reg32(v < 0 ? -v : v);
}

// The C locale's classes; EOF and characters above 127 are in none.
x86::reg32 crt_isdigit(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int(arg(cpu, 0));
    return c >= '0' && c <= '9' ? 4 : 0;
}

x86::reg32 crt_isspace(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int(arg(cpu, 0));
    return c == ' ' || (c >= 9 && c <= 13) ? 8 : 0;
}

x86::reg32 crt_isprint(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int(arg(cpu, 0));
    return c >= 0x20 && c < 0x7f ? 0x157 : 0;
}

x86::reg32 crt_tolower(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int(arg(cpu, 0));
    return reg32(c >= 'A' && c <= 'Z' ? c + 32 : c);
}

x86::reg32 crt_toupper(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int c = int(arg(cpu, 0));
    return reg32(c >= 'a' && c <= 'z' ? c - 32 : c);
}

// MSVC's generator, so sequences match the PC game's.
x86::reg32 crt_srand(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    s_rand = arg(cpu, 0);
    return 0;
}

x86::reg32 crt_rand(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    s_rand = s_rand * 214013u + 2531011u;
    return (s_rand >> 16) & 0x7fff;
}

// void qsort(void* base, size_t count, size_t width, int (*compare)(const void*, const void*)): a stable merge sort of
// indices with the guest comparator on the untouched array (a comparator that is not a strict order cannot make it
// run off the array), then the elements are moved into that order.
x86::reg32 crt_qsort(WinApplication* app, x86::CPU& cpu)
{
    const reg32 base = arg(cpu, 0), count = arg(cpu, 1), width = arg(cpu, 2), compare = arg(cpu, 3);
    if (count < 2 || !width)
        return 0;
    std::vector<reg32> order(count);
    for (reg32 i = 0; i < count; ++i)
        order[i] = i;
    std::stable_sort(order.begin(), order.end(), [&](reg32 a, reg32 b) {
        return int32_t(callGuest(app, cpu, compare, { base + a * width, base + b * width })) < 0;
    });
    std::vector<uint8_t> sorted(size_t(count) * width);
    for (reg32 i = 0; i < count; ++i)
        memcpy(&sorted[size_t(i) * width], ptr(base + order[i] * width), width);
    memcpy(ptr(base), sorted.data(), sorted.size());
    return 0;
}

// void* bsearch(const void* key, const void* base, size_t count, size_t width, compare)
x86::reg32 crt_bsearch(WinApplication* app, x86::CPU& cpu)
{
    const reg32 key = arg(cpu, 0), base = arg(cpu, 1), width = arg(cpu, 3), compare = arg(cpu, 4);
    reg32 low = 0, high = arg(cpu, 2);
    while (low < high)
    {
        const reg32 mid = low + (high - low) / 2;
        const int32_t r = int32_t(callGuest(app, cpu, compare, { key, base + mid * width }));
        if (r == 0)
            return base + mid * width;
        if (r < 0)
            high = mid;
        else
            low = mid + 1;
    }
    return 0;
}

// ---- math ------------------------------------------------------------------------------------------------------------

x86::reg32 crt_floor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return pushResult(cpu, floor(argDouble(cpu, 0)));
}

x86::reg32 crt_ceil(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return pushResult(cpu, ceil(argDouble(cpu, 0)));
}

x86::reg32 crt_sqrt(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return pushResult(cpu, sqrt(argDouble(cpu, 0)));
}

x86::reg32 crt_cos(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return pushResult(cpu, cos(argDouble(cpu, 0)));
}

x86::reg32 crt__isnan(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return std::isnan(argDouble(cpu, 0)) ? 1 : 0;
}

// The _CI intrinsics take their operands on the x87 stack: st0 (unary), or st1 op st0 (pow, fmod; one is popped).
#define CRT_X87_UNARY(name, expression)                     \
x86::reg32 name(WinApplication* app, x86::CPU& cpu)         \
{                                                           \
    RC_USE(app);                                            \
    const double x = double(cpu.fpu.st(0));                 \
    cpu.fpu.st(0) = x86::Float(expression);                 \
    return cpu.eax;                                         \
}

CRT_X87_UNARY(crt__CIacos, acos(x))
CRT_X87_UNARY(crt__CIasin, asin(x))
CRT_X87_UNARY(crt__CIcosh, cosh(x))
CRT_X87_UNARY(crt__CIsinh, sinh(x))
CRT_X87_UNARY(crt__CItanh, tanh(x))
#undef CRT_X87_UNARY

x86::reg32 crt__CIpow(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const double y = double(cpu.fpu.pop());
    cpu.fpu.st(0) = x86::Float(pow(double(cpu.fpu.st(0)), y));
    return cpu.eax;
}

x86::reg32 crt__CIfmod(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const double y = double(cpu.fpu.pop());
    cpu.fpu.st(0) = x86::Float(fmod(double(cpu.fpu.st(0)), y));
    return cpu.eax;
}

x86::reg32 crt__control87(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return controlFp(cpu, arg(cpu, 0), arg(cpu, 1));
}

// _controlfp: as _control87, without the denormal mask.
x86::reg32 crt__controlfp(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return controlFp(cpu, arg(cpu, 0), arg(cpu, 1) & ~0x80000u);
}

x86::reg32 crt__errno(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    RC_USE(cpu);
    return staticBlock("errno", 4);
}

// ---- time ------------------------------------------------------------------------------------------------------------

// time_t is 32-bit in msvcr71; the _64 functions use __time64_t (edx:eax).
x86::reg32 crt_time(WinApplication* app, x86::CPU& cpu)
{
    const reg32 t = reg32(now(app, cpu));
    if (arg(cpu, 0))
        st32(arg(cpu, 0), t);
    return t;
}

x86::reg32 crt__time64(WinApplication* app, x86::CPU& cpu)
{
    const int64_t t = now(app, cpu);
    if (arg(cpu, 0))
    {
        st32(arg(cpu, 0), reg32(t));
        st32(arg(cpu, 0) + 4, reg32(t >> 32));
    }
    cpu.edx = reg32(t >> 32);
    return reg32(t);
}

x86::reg32 crt_localtime(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return brokenDown(int32_t(ld32(arg(cpu, 0))), "tm");
}

x86::reg32 crt_gmtime(WinApplication* app, x86::CPU& cpu)
{
    return crt_localtime(app, cpu);
}

x86::reg32 crt__localtime64(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 a = arg(cpu, 0);
    return brokenDown(int64_t(uint64_t(ld32(a)) | uint64_t(ld32(a + 4)) << 32), "tm");
}

x86::reg32 crt__gmtime64(WinApplication* app, x86::CPU& cpu)
{
    return crt__localtime64(app, cpu);
}

x86::reg32 crt_mktime(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    return reg32(fromBrokenDown(arg(cpu, 0)));
}

x86::reg32 crt__mktime64(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const int64_t t = fromBrokenDown(arg(cpu, 0));
    cpu.edx = reg32(t >> 32);
    return reg32(t);
}

// clock(): milliseconds since the first call (CLOCKS_PER_SEC 1000).
x86::reg32 crt_clock(WinApplication* app, x86::CPU& cpu)
{
    static const reg32 start = kernel32::GetTickCount(app, cpu);
    return kernel32::GetTickCount(app, cpu) - start;
}

// void _ftime(struct _timeb*): time_t time, unsigned short millitm, short timezone, short dstflag.
x86::reg32 crt__ftime(WinApplication* app, x86::CPU& cpu)
{
    uint32_t ms = 0;
    const reg32 t = reg32(now(app, cpu, &ms));
    const reg32 b = arg(cpu, 0);
    st32(b, t);
    app->getMemory<x86::reg16>(b + 4) = x86::reg16(ms);
    app->getMemory<x86::reg16>(b + 6) = 0;
    app->getMemory<x86::reg16>(b + 8) = 0;
    return 0;
}

}}

// Helpers of the msvcr71/msvcp71 shims (src/lib/winapi/msvcr71*.cpp): guest stack arguments, guest pointers, calls
// into guest code and the guest-side state of the runtime.
#ifndef WINAPI_CRT_H_
#define WINAPI_CRT_H_

#include <lib/memmap.h>
#include <lib/winapp.h>
#include <x86.h>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <string>

namespace win32 { namespace crt
{

using x86::reg32;

inline reg32 ld32(reg32 address)
{
    reg32 v;
    memcpy(&v, g_guestBase + address, 4);
    return v;
}

inline void st32(reg32 address, reg32 value)
{
    memcpy(g_guestBase + address, &value, 4);
}

inline double ld64f(reg32 address)
{
    double v;
    memcpy(&v, g_guestBase + address, 8);
    return v;
}

// The i-th 32-bit cdecl argument (0 is the first) and a double occupying arguments i and i + 1.
inline reg32 arg(const x86::CPU& cpu, unsigned i)
{
    return ld32(cpu.esp + 4 + 4 * i);
}

inline double argDouble(const x86::CPU& cpu, unsigned i)
{
    return ld64f(cpu.esp + 4 + 4 * i);
}

// Host pointer of a guest address (null for 0), and back.
template< typename T = char >
inline T* ptr(reg32 address)
{
    return address ? reinterpret_cast<T*>(g_guestBase + address) : nullptr;
}

inline reg32 guest(const void* p)
{
    return p ? reg32(static_cast<const x86::reg8*>(p) - g_guestBase) : 0;
}

// Calls a cdecl guest function with 32-bit arguments; returns eax. The caller's registers and stack pointer are kept.
reg32 callGuest(WinApplication* app, x86::CPU& cpu, reg32 function, std::initializer_list<reg32> arguments);

// Setting debug_find: logs fn and the guest return address when the text at dst contains the setting's text.
void traceText(const char* fn, reg32 dst, const x86::CPU& cpu);
void traceWide(const char* fn, reg32 dst, const x86::CPU& cpu);

// Guest heap blocks (16-byte aligned); 0 when guest memory is exhausted.
reg32 allocate(reg32 size, bool zero = false);
void release(reg32 address);

// A zeroed block of guest memory for the runtime's own variables, allocated on first use and kept (a static of the
// guest's C runtime: the strtok position, the struct tm of localtime, errno, ...).
reg32 staticBlock(const char* name, reg32 size);

// errno of the C runtime (one for all guest threads).
void setErrno(int value);

// printf-family formatting of guest arguments: fmt and the arguments are guest memory, args the address of the first
// argument (a va_list is that address). Wide variant for vswprintf (16-bit characters).
std::string format(const char* fmt, reg32 args);
std::u16string formatWide(const char16_t* fmt, reg32 args);

// sscanf over guest memory: the pointers to store to start at args. Returns the number of assigned fields, or -1
// (EOF) when the input ended before the first conversion.
int scan(const char* input, const char* fmt, reg32 args);

}}

#endif

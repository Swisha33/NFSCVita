// msvcp71.dll for NFSC.exe: std::basic_string<char> construction and destruction in Visual C++ 7.1's layout, which
// the game's inline string code reads and changes directly:
//   +0x00 allocator (unused), +0x04 union { char buffer[16]; char* pointer; }, +0x14 size, +0x18 capacity.
// A string whose capacity is 16 or more keeps its text in a heap block (msvcr71 malloc's heap, as the game's own
// operator new uses).

#include <winapi/msvcp71.h>
#include "crt.h"

#include <cstring>

namespace win32 { namespace msvcp71
{

using namespace crt;

namespace
{

const reg32 Buffer = 4, Size = 0x14, Capacity = 0x18, Inline = 15;

const char* text(reg32 self)
{
    return ld32(self + Capacity) > Inline ? ptr(ld32(self + Buffer)) : ptr(self + Buffer);
}

void assign(reg32 self, const char* s, reg32 n)
{
    if (n <= Inline)
    {
        memcpy(ptr(self + Buffer), s, n);
        ptr(self + Buffer)[n] = 0;
        st32(self + Capacity, Inline);
    }
    else
    {
        const reg32 capacity = n | Inline;
        const reg32 block = allocate(capacity + 1);
        memcpy(ptr(block), s, n);
        ptr(block)[n] = 0;
        st32(self + Buffer, block);
        st32(self + Capacity, capacity);
    }
    st32(self + Size, n);
}

}

// basic_string(const char*)
x86::reg32 cpp_string_ctor_cstr(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx;
    const char* s = ptr(arg(cpu, 0));
    cpu.esp += 4;
    assign(self, s ? s : "", s ? reg32(strlen(s)) : 0);
    return self;
}

// basic_string(const basic_string&)
x86::reg32 cpp_string_ctor_copy(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx, other = arg(cpu, 0);
    cpu.esp += 4;
    // A temporary copy: other's text may be inline in a block assign is about to fill when self == other.
    const std::string copy(text(other), ld32(other + Size));
    assign(self, copy.data(), reg32(copy.size()));
    return self;
}

// ~basic_string()
x86::reg32 cpp_string_dtor(WinApplication* app, x86::CPU& cpu)
{
    RC_USE(app);
    const reg32 self = cpu.ecx;
    if (ld32(self + Capacity) > Inline)
        release(ld32(self + Buffer));
    st32(self + Capacity, Inline);
    st32(self + Size, 0);
    ptr(self + Buffer)[0] = 0;
    return cpu.eax;
}

}}

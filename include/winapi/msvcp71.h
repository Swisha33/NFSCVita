#ifndef msvcp71_H_
#define msvcp71_H_

// The three std::basic_string<char> members NFSC.exe imports from msvcp71.dll (thiscall: this in ecx, the callee pops
// its arguments). Names: tools/recompiler/disasm/writer.py MANGLED.

#include <x86.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace msvcp71
{

x86::reg32 cpp_string_ctor_cstr(WinApplication* app, x86::CPU& cpu);
x86::reg32 cpp_string_ctor_copy(WinApplication* app, x86::CPU& cpu);
x86::reg32 cpp_string_dtor(WinApplication* app, x86::CPU& cpu);

}}

#endif

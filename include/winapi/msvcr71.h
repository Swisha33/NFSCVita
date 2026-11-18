#ifndef msvcr71_H_
#define msvcr71_H_

// The Visual C++ 7.1 runtime (msvcr71.dll) that NFSC.exe imports, implemented natively over guest memory.
//
// Every export is a function crt_<exact export name>(app, cpu) with no declared parameters: it reads its arguments
// from the guest stack (cpu.esp + 4 is the first) and the import wrapper pops only the return address, which is the
// cdecl convention. Results go to eax (edx:eax for 64-bit ones); double results are pushed on the guest x87 stack.
// The few stdcall and thiscall exports pop their own arguments (CRT_POP). Names follow tools/recompiler
// disasm/writer.py cpp_name; the C++-mangled exports have readable names there (writer.MANGLED).

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace msvcr71
{
// Guest function free() hands a pointer to when it is not a block of the guest heap (0: log an error). For call
// sites patched from the game's own allocator to malloc / free, which also see the game allocator's blocks.
void setForeignFree(x86::reg32 function);
} }

#define MSVCR71_EXPORTS(X) \
    X(crt_exception_ctor) X(crt_exception_ctor_copy) X(crt_exception_dtor) X(crt_type_info_dtor) X(crt_terminate) \
    X(crt_vswprintf_count) \
    X(crt__CIacos) X(crt__CIasin) X(crt__CIcosh) X(crt__CIfmod) X(crt__CIpow) X(crt__CIsinh) X(crt__CItanh) \
    X(crt__CxxThrowException) X(crt__XcptFilter) X(crt___CxxFrameHandler) X(crt___dllonexit) X(crt___getmainargs) \
    X(crt___p___initenv) X(crt___p__commode) X(crt___p__fmode) X(crt___security_error_handler) \
    X(crt___set_app_type) X(crt___setusermatherr) X(crt__aligned_free) X(crt__aligned_malloc) \
    X(crt__aligned_realloc) X(crt__amsg_exit) X(crt__beginthreadex) X(crt__c_exit) X(crt__cexit) X(crt__close) \
    X(crt__control87) X(crt__controlfp) X(crt__endthreadex) X(crt__errno) X(crt__except_handler3) X(crt__exit) \
    X(crt__filelength) X(crt__findclose) X(crt__findfirst) X(crt__findnext) X(crt__ftime) X(crt__fullpath) \
    X(crt__getcwd) X(crt__getdiskfree) X(crt__getdrive) X(crt__gmtime64) X(crt__initterm) X(crt__isnan) \
    X(crt__localtime64) X(crt__lseek) X(crt__mbscat) X(crt__mbscpy) X(crt__mkdir) X(crt__mktime64) X(crt__onexit) \
    X(crt__open) X(crt__purecall) X(crt__read) X(crt__rmdir) X(crt__setjmp3) X(crt__snprintf) X(crt__strcmpi) \
    X(crt__strnicmp) X(crt__time64) X(crt__vsnprintf) X(crt__write) \
    X(crt_abs) X(crt_atoi) X(crt_bsearch) X(crt_calloc) X(crt_ceil) X(crt_clock) X(crt_cos) X(crt_exit) \
    X(crt_fclose) X(crt_fflush) X(crt_fgetc) X(crt_floor) X(crt_fopen) X(crt_fprintf) X(crt_fputc) X(crt_fread) \
    X(crt_free) X(crt_fwrite) X(crt_getenv) X(crt_gmtime) X(crt_isdigit) X(crt_isprint) X(crt_isspace) \
    X(crt_localtime) X(crt_longjmp) X(crt_malloc) X(crt_memcmp) X(crt_memcpy) X(crt_memmove) X(crt_memset) \
    X(crt_mktime) X(crt_printf) X(crt_qsort) X(crt_raise) X(crt_rand) X(crt_realloc) X(crt_remove) X(crt_sprintf) \
    X(crt_sqrt) X(crt_srand) X(crt_sscanf) X(crt_strchr) X(crt_strcmp) X(crt_strcoll) X(crt_strcspn) \
    X(crt_strerror) X(crt_strlen) X(crt_strncat) X(crt_strncmp) X(crt_strncpy) X(crt_strrchr) X(crt_strstr) \
    X(crt_strtok) X(crt_strtol) X(crt_strtoul) X(crt_time) X(crt_tolower) X(crt_toupper) X(crt_ungetc) \
    X(crt_vfprintf) X(crt_vsprintf) X(crt_wcscpy) X(crt_wcslen)

namespace win32 { namespace msvcr71
{

#define MSVCR71_DECLARE(name) x86::reg32 name(WinApplication* app, x86::CPU& cpu);
MSVCR71_EXPORTS(MSVCR71_DECLARE)
#undef MSVCR71_DECLARE

// Data exports: the guest address of the variable (FILE _iob[20]; int _adjust_fdiv). The first call also sets up
// the runtime's guest-side state (static buffers, the std::exception vtable); it runs in the Application constructor,
// before guest code.
x86::reg32 data_crt__iob(WinApplication* app);
x86::reg32 data_crt__adjust_fdiv(WinApplication* app);

}}

#endif

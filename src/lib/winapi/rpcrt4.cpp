#include <winapi/rpcrt4.h>
#include "crt.h"

#include <cstdio>

namespace win32 { namespace rpcrt4
{

LONG UuidToStringA(WinApplication* app, x86::CPU& cpu, const GUID* uuid, x86::reg32* string)
{
    RC_USE(app);
    RC_USE(cpu);
    if (!uuid || !string)
        return 1702;    // RPC_S_INVALID_ARG
    const x86::reg32 text = crt::allocate(40);
    snprintf(crt::ptr(text), 40, "%08lx-%04x-%04x-%02x%02x-%02x%02x%02x%02x%02x%02x", (unsigned long)uuid->Data1,
             unsigned(uuid->Data2), unsigned(uuid->Data3), uuid->Data4[0], uuid->Data4[1], uuid->Data4[2],
             uuid->Data4[3], uuid->Data4[4], uuid->Data4[5], uuid->Data4[6], uuid->Data4[7]);
    *string = text;
    return 0;
}

LONG RpcStringFreeA(WinApplication* app, x86::CPU& cpu, x86::reg32* string)
{
    RC_USE(app);
    RC_USE(cpu);
    if (string && *string)
    {
        crt::release(*string);
        *string = 0;
    }
    return 0;
}

}}

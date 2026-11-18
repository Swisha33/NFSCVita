#ifndef rpcrt4_H_
#define rpcrt4_H_

#include <x86.h>
#include <winapi/types.h>

namespace win32 { class WinApplication; }

namespace win32 { namespace rpcrt4
{

// RPC_STATUS UuidToStringA(const UUID*, RPC_CSTR* string): the string is a guest heap block RpcStringFreeA frees.
LONG UuidToStringA(WinApplication* app, x86::CPU& cpu, const GUID* uuid, x86::reg32* string);
LONG RpcStringFreeA(WinApplication* app, x86::CPU& cpu, x86::reg32* string);

}}

#endif

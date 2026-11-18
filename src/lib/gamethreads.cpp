#include <lib/scheduler.h>

// The guest threads of NFSC.exe (Carbon v1.4). UG1VITA's table was Speed.exe's; Carbon's are mostly started through
// the EA thread library's trampoline 008daf40, which reads the real entry and argument at +0x10/+0x14 of its block
// (run 56). Known entries: 008a800c EA audio service thread (timer-mode mixer, paced clock: src/nfsc/main.cpp),
// "RWAudioCore Dac" (also through the trampoline).
namespace win32 { namespace scheduler
{

void configureGameThreads()
{
    setStartThunk(0x008DAF40, 0x10, 0x14);
}

}}

#ifndef NFSC_BANKCODE_H_
#define NFSC_BANKCODE_H_

// x86 routines inside the audio banks (SOUND/**/*.abk), interpreted when the sound library calls them.
namespace win32 { class WinApplication; }

namespace nfsc { namespace bankcode {

// Registers the dynamic_call resolver for bank code (and, with bank_unlocked, lets the unlocked audio thread run
// routines without the execution lock); called once before the game runs.
void install(win32::WinApplication& app);

}}

#endif

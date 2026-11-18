// The Vita pad's default bindings in Carbon: NFSUG2_VITA's layout (its README, "Controls").
//
// Carbon has no layout for our joystick and used its generic PC one (Triangle gas, R pause). The bindings are 72
// rows at 0xa98700 {name, triples*}; triples 0/1 are the keyboard, 2p+2 / 2p+3 controller port p's primary /
// secondary, each {kind, port, control}. Kinds: 2 button, 4/5 axis -/+. 00689550 writes a port's rows 0..46 with
// the game's defaults (device creation, Controls -> Restore Defaults, a full reset). Layout of the table and the row
// meanings from the Mac port (refs/nfs-carbon-mac, mods/core/nfsc/pad.h, MIT).
//
// The UG2 layout is the DEFAULT, not a rule (as in the Mac port): the first time a port's device exists, its rows
// (the game's own defaults) are remembered; on any frame where rows 0, 1, 4, 5, 6 are exactly those again (a fresh
// profile, Restore Defaults, a profile saved before this layout), the port gets the UG2 layout. A remap on the
// Controls screen makes the rows differ, so it is kept and saved as usual.
//
// Our joystick (src/lib/controls.cpp): buttons 0 Triangle, 1 Circle, 2 Cross, 3 Square, 4 / 5 rear touch upper
// left / right, 6 L, 7 R, 8 Select (tap), 9 Start, 10 / 11 rear touch lower left / right, 12-15 d-pad up, right,
// down, left; axes 0 X / 1 Y = left stick.
//
// Setting ug2_layout (default on).

#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <x86.h>

#include <cstring>

namespace nfsc
{

namespace
{

const x86::reg32 PadTable = 0x00a98704;     // rows, stride 8: triples pointer
const x86::reg32 PadDevices = 0x00a986d0;   // device per port
const x86::reg32 PadPortCount = 0x00a98948;
const unsigned DefaultRows = 47;
const unsigned MaxPorts = 4;

enum : x86::reg32 { Button = 2, AxisMinus = 4, AxisPlus = 5 };
enum : x86::reg32
{
    Triangle = 0, Circle = 1, Cross = 2, Square = 3, RearUpperLeft = 4, RearUpperRight = 5, L = 6, R = 7,
    Select = 8, Start = 9, RearLowerLeft = 10, RearLowerRight = 11, DpadUp = 12, DpadRight = 13, DpadDown = 14,
    DpadLeft = 15
};
const x86::reg32 StickX = 0, StickY = 1;

struct Binding
{
    uint8_t row;
    x86::reg32 kind, control, kind2, control2;
};

// UG2 Vita: R gas, L brake / reverse, left stick steer, Cross handbrake, Circle nitrous, Triangle / Square shift
// up / down (UG2 also shifts on the rear upper corners: left out, the user's choice), rear lower corners look back / camera, Select reset car, Start pause;
// menus: d-pad and left stick move, Cross select, Circle back. Carbon only: Speedbreaker on d-pad down, crew on
// d-pad left (UG2's free-roam d-pad shortcuts have no Carbon rows), engage event on d-pad up. Rows 21/22 ("HUD pad
// left / right" in the Mac port) stay unbound: their use is not known and d-pad left is the crew.
const Binding Layout[] =
{
    { 0, Button, R, 0, 0 },                             // gas
    { 1, Button, L, 0, 0 },                             // brake / reverse
    { 2, AxisMinus, StickX, 0, 0 },                     // steer left
    { 3, AxisPlus, StickX, 0, 0 },                      // steer right
    { 4, Button, Cross, 0, 0 },                         // handbrake
    { 5, Button, DpadDown, 0, 0 },                      // Speedbreaker
    { 6, Button, Circle, 0, 0 },                        // nitrous
    { 7, Button, Square, 0, 0 },                        // shift down
    { 8, Button, Triangle, 0, 0 },                      // shift up
    { 18, Button, Select, 0, 0 },                       // reset car
    { 19, Button, DpadLeft, 0, 0 },                     // crew
    { 20, Button, DpadUp, 0, 0 },                       // engage event
    { 24, Button, RearLowerRight, 0, 0 },               // camera
    { 25, Button, RearLowerLeft, 0, 0 },                // look back
    { 28, Button, Start, 0, 0 },                        // pause
    { 29, Button, DpadLeft, AxisMinus, StickX },        // menu left
    { 30, Button, DpadRight, AxisPlus, StickX },        // menu right
    { 31, Button, DpadUp, AxisMinus, StickY },          // menu up
    { 32, Button, DpadDown, AxisPlus, StickY },         // menu down
    { 37, Button, Cross, 0, 0 },                        // accept
    { 38, Button, Circle, 0, 0 },                       // back
    { 39, Button, Start, 0, 0 },                        // start
    { 40, Button, L, 0, 0 },                            // menu L
    { 41, Button, R, 0, 0 },                            // menu R
    { 42, Button, R, 0, 0 },                            // FE button 0
    { 43, Button, L, 0, 0 },                            // FE button 1
    { 45, Button, Square, 0, 0 },                       // FE button 4
    { 46, Button, Triangle, 0, 0 },                     // FE button 5
};

const unsigned SignatureRows[] = { 0, 1, 4, 5, 6 };

bool s_enabled = true;
bool s_haveDefaults[MaxPorts] = {};
x86::reg32 s_defaults[MaxPorts][DefaultRows][2][3];

x86::reg32 triple(win32::WinApplication* app, unsigned row, unsigned slot)
{
    const x86::reg32 rows = app->getMemory<x86::reg32>(PadTable + row * 8);
    return rows ? rows + slot * 12 : 0;
}

void set(win32::WinApplication* app, unsigned port, unsigned row, unsigned secondary, x86::reg32 kind,
         x86::reg32 control)
{
    const x86::reg32 at = triple(app, row, 2 * port + 2 + secondary);
    if (!at)
        return;
    app->getMemory<x86::reg32>(at) = kind;
    app->getMemory<x86::reg32>(at + 4) = kind ? port : 0;
    app->getMemory<x86::reg32>(at + 8) = kind ? control : 0;
}

void apply(win32::WinApplication* app, unsigned port)
{
    for (unsigned row = 0; row < DefaultRows; ++row)
    {
        set(app, port, row, 0, 0, 0);
        set(app, port, row, 1, 0, 0);
    }
    for (const Binding& b : Layout)
    {
        set(app, port, b.row, 0, b.kind, b.control);
        set(app, port, b.row, 1, b.kind2, b.control2);
    }
}

bool snapshot(win32::WinApplication* app, unsigned port, x86::reg32 (&out)[DefaultRows][2][3])
{
    for (unsigned row = 0; row < DefaultRows; ++row)
        for (unsigned s = 0; s < 2; ++s)
        {
            const x86::reg32 at = triple(app, row, 2 * port + 2 + s);
            if (!at)
                return false;
            for (unsigned k = 0; k < 3; ++k)
                out[row][s][k] = app->getMemory<x86::reg32>(at + 4 * k);
        }
    return true;
}

// Rows 0 1 4 5 6 (kind and control of both slots) as the game's defaults put them.
bool isDefault(win32::WinApplication* app, unsigned port)
{
    for (unsigned row : SignatureRows)
        for (unsigned s = 0; s < 2; ++s)
        {
            const x86::reg32 at = triple(app, row, 2 * port + 2 + s);
            if (!at || app->getMemory<x86::reg32>(at) != s_defaults[port][row][s][0]
                || app->getMemory<x86::reg32>(at + 8) != s_defaults[port][row][s][2])
                return false;
        }
    return true;
}

// The first version of this layout (2026-11-18) also shifted on the rear upper corners (rows 7 / 8 secondary); a
// profile saved with it gets them removed, as long as both are still exactly so.
void dropRearShift(win32::WinApplication* app, unsigned port)
{
    const x86::reg32 down = triple(app, 7, 2 * port + 3), up = triple(app, 8, 2 * port + 3);
    if (!down || !up || app->getMemory<x86::reg32>(down) != Button || app->getMemory<x86::reg32>(up) != Button
        || app->getMemory<x86::reg32>(down + 8) != RearUpperLeft || app->getMemory<x86::reg32>(up + 8) != RearUpperRight)
        return;
    set(app, port, 7, 1, 0, 0);
    set(app, port, 8, 1, 0, 0);
    pf::logInfo("pad: controller %u: rear-touch shifting removed", port);
}

}

void installPad()
{
    s_enabled = pf::Config::getBool("ug2_layout", true);
}

// Per Present.
void padPresent(win32::WinApplication* app, x86::CPU&)
{
    if (!s_enabled)
        return;
    x86::reg32 ports = app->getMemory<x86::reg32>(PadPortCount);
    if (ports > MaxPorts)
        ports = MaxPorts;
    for (unsigned port = 0; port < ports; ++port)
    {
        if (!app->getMemory<x86::reg32>(PadDevices + port * 4))
            continue;
        if (!s_haveDefaults[port])
        {
            // The first frame with the device: the game's own defaults (a joystick has a gas binding then).
            if (!snapshot(app, port, s_defaults[port]) || s_defaults[port][0][0][0] == 0)
                continue;
            s_haveDefaults[port] = true;
            pf::logInfo("pad: controller %u: game defaults gas {%u %u}, nitrous {%u %u}", port,
                        unsigned(s_defaults[port][0][0][0]), unsigned(s_defaults[port][0][0][2]),
                        unsigned(s_defaults[port][6][0][0]), unsigned(s_defaults[port][6][0][2]));
        }
        if (isDefault(app, port))
        {
            apply(app, port);
            pf::logInfo("pad: controller %u set to the UG2 Vita layout", port);
        }
        else
            dropRearShift(app, port);
    }
}

}

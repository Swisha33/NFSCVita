// Widescreen for the Vita's 960x544, after ThirteenAG's NFSCarbon.WidescreenFix (MIT) as the macOS port applies it
// (refs/nfs-carbon-mac mods/core/nfsc/nfsc.c; its addresses are for the same NFSC.exe).
//
// The game keeps its back buffer (640x480 by default), which the Vita shows stretched to 960x544. Instead of a wider
// back buffer (more pixels to draw), everything is laid out for the display's shape and squeezed into the buffer, so
// the stretch gives the right proportions:
// - 3D: the projection 0071b780 reads its aspect term and three constants from slots (generate.py
//   WIDESCREEN_PATCHES), filled here per view with the fix's hor+ values: the player view sees more to the sides.
// - HUD and menus: the FE renderer's x scale and centres (.rdata/.data floats the code reads at run time) are set for
//   a 480 * aspect wide canvas, the game's own widescreen HUD layout is forced on (patches), and when it switches on
//   the left and right HUD groups move out (the fix's SetWideScreenMode hook).
// - Rear-view mirror and picture-in-picture: their quads (00750b30) are narrowed by (4/3) / aspect.
// The aspect is the display's, whatever the game's resolution option, since the display is always 960x544.

#include <lib/guest.h>
#include <lib/winapp.h>
#include <platform/config.h>
#include <platform/log.h>
#include <recomp.h>
#include <x86.h>

#include <set>

namespace nfsc
{

void sub_71b780_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_5d5280_guest(win32::WinApplication* app, x86::CPU& cpu);
void sub_750b30_guest(win32::WinApplication* app, x86::CPU& cpu);

namespace
{

using x86::reg32;

const double Aspect = 960.0 / 544.0;

// Slots in the zero padding after .rsrc (generate.py WIDESCREEN_PATCHES point the game's operands at them).
const reg32 SlotAspectNum = 0x00c2b440;     // int, FILD at 0071b858 (game: [0xab0acc], back buffer height)
const reg32 SlotAspectDen = 0x00c2b444;     // int, FIDIV at 0071b864 (game: [0xab0ac8], back buffer width)
const reg32 SlotFovH = 0x00c2b448;          // float, FDIVR at 0071b8da (game: 1.0)
const reg32 SlotFovMul = 0x00c2b44c;        // float, FMUL at 0071b8ec (game: 0.5)
const reg32 SlotFovV = 0x00c2b450;          // float, FDIVR at 0071b923 (game: 1.0)
const reg32 SlotHudOffset = 0x00c2b454;     // float, the widescreen HUD's minimap offset (game: 120.0)
const reg32 SlotMenuWidth = 0x00c2b458;     // float, menu cursor / Autosculpt line width (game: 640.0)
const reg32 Scratch = 0x00c2b460;           // 8 bytes for GetCenter

const reg32 BackBufferHeight = 0x00ab0acc, BackBufferWidth = 0x00ab0ac8;

// FE floats the code reads in place (the fix's ProtectedGameRefs).
const reg32 HudScaleX = 0x009e8f8c;         // 1/320
const reg32 HudWorldMapX = 0x00a604ac;      // 320
const reg32 HudFeX = 0x009c778c;            // 320
const reg32 HudRaceRouteX = 0x009d5f3c;     // 384
const reg32 ArrestBlurStep = 0x009d0584;    // 1/640

// FEngHud::SetWideScreenMode helpers and groups.
const reg32 FeFindObject = 0x005a0250;      // cdecl (package, hash)
const reg32 FeGetCenter = 0x00597900;       // cdecl (object, float* x, float* y)
const reg32 FeSetCenter = 0x00597a70;       // cdecl (object, float x, float y)
const reg32 HudLeftGroup = 0x1603009e, HudRightGroup = 0x5d0101f1;

double s_offset = 0.0;          // 240 * aspect - 320
int s_fovScaling = 1;

reg32 floatBits(float f)
{
    reg32 u;
    memcpy(&u, &f, 4);
    return u;
}

reg32 call(win32::WinApplication* app, x86::CPU& cpu, reg32 function, const reg32* args, int count)
{
    const x86::CPU saved = cpu;
    for (int i = count - 1; i >= 0; --i)
        rc::st32(cpu.esp -= 4, args[i]);
    ::native::callGuest(app, cpu, function);
    const reg32 result = cpu.eax;
    cpu = saved;
    return result;
}

struct Group
{
    reg32 object = 0;
    float x = 0.0f, y = 0.0f;
} s_group[2];

// Moves a HUD group by dx from where the game laid it out (remembered per object, so it never adds up).
void moveGroup(win32::WinApplication* app, x86::CPU& cpu, reg32 package, reg32 hash, int slot, float dx)
{
    const reg32 find[2] = { package, hash };
    const reg32 object = call(app, cpu, FeFindObject, find, 2);
    if (!object)
        return;
    Group& group = s_group[slot];
    if (group.object != object)
    {
        const reg32 get[3] = { object, Scratch, Scratch + 4 };
        call(app, cpu, FeGetCenter, get, 3);
        group.object = object;
        group.x = rc::ldf32(Scratch);
        group.y = rc::ldf32(Scratch + 4);
    }
    const reg32 set[3] = { object, floatBits(group.x + dx), floatBits(group.y) };
    call(app, cpu, FeSetCenter, set, 3);
}

}

void installWidescreen(win32::WinApplication& app)
{
    (void)app;
    s_fovScaling = pf::Config::getInt("widescreen_fov", 1);
    if (s_fovScaling < 0 || s_fovScaling > 2)
        s_fovScaling = 1;
    const double scale = 2.0 / (480.0 * Aspect);
    const double centre = 1.0 / scale;          // 320 at 4:3
    s_offset = centre - 320.0;
    rc::stf32(HudScaleX, float(scale));
    rc::stf32(HudWorldMapX, float(centre));
    rc::stf32(HudFeX, float(centre));
    rc::stf32(HudRaceRouteX, float(centre - 320.0 + 384.0));
    rc::stf32(ArrestBlurStep, float(1.0 / 640.0 * (4.0 / 3.0) / Aspect));
    rc::stf32(SlotHudOffset, float(s_offset));
    rc::stf32(SlotMenuWidth, float(2.0 * centre));
    // Projection slots: the game's own values until the first view is set up.
    rc::st32(SlotAspectNum, 3);
    rc::st32(SlotAspectDen, 4);
    rc::stf32(SlotFovH, 1.0f);
    rc::stf32(SlotFovMul, 0.5f);
    rc::stf32(SlotFovV, 1.0f);
    pf::logInfo("screen: widescreen for %.4f, HUD scale %.6f, centre %.1f, side offset %.1f, view scaling %d",
                Aspect, scale, centre, s_offset, s_fovScaling);
}

namespace native
{

// Projection set-up (thiscall; view, ...; ret 0x14): the view's id at [view + 8].
void sub_71b780(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 view = rc::ld32(cpu.esp + 4);
    const reg32 id = view ? rc::ld32(view + 8) : 0;
    if (id == 1 || id == 4)
    {
        // The main view and its reflection: hor+ (the fix's FOVHook).
        double hor = (4.0 / 3.0) / Aspect;
        double vertical = 1.205;
        if (s_fovScaling)
        {
            hor /= 1.034482718;
            vertical = s_fovScaling == 2 ? 1.28 : 1.225;
        }
        rc::st32(SlotAspectNum, 1);
        rc::st32(SlotAspectDen, 1);
        rc::stf32(SlotFovH, float(hor));
        rc::stf32(SlotFovMul, 0.434665f);
        rc::stf32(SlotFovV, float(vertical));
    }
    else if (id == 3)
    {
        // Rear-view mirror: the fix's 0.925 aspect term.
        rc::st32(SlotAspectNum, 37);
        rc::st32(SlotAspectDen, 40);
        rc::stf32(SlotFovH, 1.0f);
        rc::stf32(SlotFovMul, 0.375f);
        rc::stf32(SlotFovV, 1.0f);
    }
    else
    {
        // Every other view (reflections, shadows, menus' own views): the game's values.
        rc::st32(SlotAspectNum, rc::ld32(BackBufferHeight));
        rc::st32(SlotAspectDen, rc::ld32(BackBufferWidth));
        rc::stf32(SlotFovH, 1.0f);
        rc::stf32(SlotFovMul, 0.5f);
        rc::stf32(SlotFovV, 1.0f);
    }
    static std::set<reg32> s_seen;
    if (s_seen.size() < 32 && s_seen.insert(id).second)
        pf::logInfo("screen: view %u projection: aspect term %u/%u, x %.4f, half angle %.4f, y %.4f", unsigned(id),
                    unsigned(rc::ld32(SlotAspectNum)), unsigned(rc::ld32(SlotAspectDen)), rc::ldf32(SlotFovH),
                    rc::ldf32(SlotFovMul), rc::ldf32(SlotFovV));
    sub_71b780_guest(app, cpu);
}

// FEngHud::SetWideScreenMode (thiscall, plain ret): when its layout byte [this + 0x341] turns on, the left and right
// HUD groups of its package [this + 0x2c] move out to the screen's sides.
void sub_5d5280(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 hud = cpu.ecx;
    const uint8_t before = hud ? rc::ld8(hud + 0x341) : 0;
    sub_5d5280_guest(app, cpu);
    if (!hud || before || !rc::ld8(hud + 0x341))
        return;
    const reg32 package = rc::ld32(hud + 0x2c);
    if (!package)
        return;
    moveGroup(app, cpu, package, HudLeftGroup, 0, float(-s_offset));
    moveGroup(app, cpu, package, HudRightGroup, 1, float(s_offset));
    static bool s_logged = false;
    if (!s_logged)
    {
        s_logged = true;
        pf::logInfo("screen: widescreen HUD on, side groups moved by %.1f", s_offset);
    }
}

// Screen quad builder (cdecl; quad*, x, y, w, h; 4 vertices 0x18 bytes apart, x at +0) of the rear-view mirror and
// picture-in-picture: x narrowed by (4/3) / aspect (the fix's MirrorFix).
void sub_750b30(win32::WinApplication* app, x86::CPU& cpu)
{
    const reg32 quad = rc::ld32(cpu.esp + 4);
    sub_750b30_guest(app, cpu);
    if (!quad)
        return;
    const float narrow = float((4.0 / 3.0) / Aspect);
    for (reg32 v = 0; v < 4; ++v)
        rc::stf32(quad + 0x18 * v, rc::ldf32(quad + 0x18 * v) * narrow);
}

}

}

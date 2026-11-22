"""Generates the C++ recompilation of Need for Speed Carbon (NFSC.exe, PC v1.4) into one directory.

Usage: generate.py --binaries <dir> --out <dir> [--analysis <file.json>]

<dir> must hold NFSC.exe. Its SHA-256 is checked: the tables below fit only this file. (UG1VITA's original tables for
Underground's Speed.exe are in its repository; Carbon's addresses come from the macOS port's game.toml/CLAUDE.md.)
The audio bank routines (SOUND/**/*.abk) are translated by bankcode.py into nfsc_bank.cpp; --bank-only rewrites just
that file.

Files in the output directory are replaced only when their content changes; generated files that are no longer
produced are deleted, other files are left alone.
"""

import argparse
import collections
import filecmp
import hashlib
import json
import os
import re
import shutil
import struct
import sys
import tempfile
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
from capstone import x86 as CX  # noqa: E402
from disasm import analysis, codegen, discover, fpustack, frames, image as pe_image, writer  # noqa: E402
import bankcode  # noqa: E402

EXE_NAME = 'NFSC.exe'
EXE_SHA256 = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3'
MODULE = 'nfsc'

# Audio bank routines (bankcode.py): call operands index the library function table the loader 0x898A82 patches
# through (Android port, scripts/abk_module.py).
BANK_LIBRARY_TABLE = 0xA7D6C0
BANK_DIR = 'SOUND'

# setjmp/longjmp come from msvcr71.dll. The only setjmp call (0x64db13) goes through the import thunk 0x87ef70
# (jmp [_setjmp3]): calls to it keep a host jump buffer. The longjmp import shim (src/lib/winapi/msvcr71.cpp) restores
# the registers _setjmp3 saved and resumes there (rc::longJump), so no guest longjmp jump is listed.
SETJMP = 0x87ef70
LONGJMP_JUMPS = {}                      # jump address -> register holding the guest jmp_buf

# Functions that never return although their code has a path to ret. The C runtime's exits are msvcr71 imports
# (disasm/discover.py NO_RETURN_IMPORTS).
NO_RETURN = []

# Guest functions replaced by hand-written native code (namespace nfsc::native); the translated body stays as
# sub_<address>_guest. EA audio kernels (src/nfsc/audio_kernels.cpp): mix 0x8AD305, scale 0x8AD12E, filter 0x89F363.
# Hot x87 leaves (src/nfsc/native_math.cpp, after NFSUG2_VITA's native _ftol2): _ftol2 0x87EAC0, box transform
# 0x70F0B0, frustum test 0x71B630, square overlap 0x46FCC0. Widescreen hooks (src/nfsc/widescreen.cpp): projection
# 0x71B780, FEngHud::SetWideScreenMode 0x5D5280, mirror/PiP quad 0x750B30.
NATIVE_OVERRIDES = [0x8AD305, 0x8AD12E, 0x89F363, 0x87EAC0, 0x70F0B0, 0x71B630, 0x46FCC0, 0x79F9C0,
                    0x71B780, 0x5D5280, 0x750B30, 0x6A83A0,
                    0x6997A0, 0x6B68D0, 0x7D7440, 0x7E6E00, 0x7E4B30]  # load_trace.cpp (diagnostic)

# Entries no static analysis finds: reached only through pointers, vtables or as thread starts. Found during the
# macOS port's bring-up as "call to unknown target" (github.com/vasist15-source/nfs-carbon-mac game.toml).
EXTRA_ENTRIES = [
    0x004c5450, 0x0089122d,
    0x0089f604, 0x0089f07c, 0x0089f494,     # constructors called through the vector helper 00403a60
    0x008999ee,                             # audio thread
    0x0054bac0,                             # after int3 padding
    0x0088510e, 0x00884a1d,                 # MMX routines of the VideoDecodeThread (EA splash)
    0x0059d6d0,                             # vtable +0x1c, called from 00589093 at press start
    0x0089777e,                             # called from 00895d3d after the press-start key press
    0x008ab884,                             # audio decoder (thread from 008b65fe)
    0x00555450, 0x005507a0, 0x005a69b0,     # main menu / career / Audio options
    0x00868cc0,                             # vtable +0x1c after the first Cross chase cutscene
    0x00583d70,                             # FEng object callback (vtable 009d0680)
    # Pointed at from .rdata/.data, after padding, and not discovered (work/missing_entries.py, 2026-11-15):
    0x008a2dda,                             # sound player table 00a03ec0 [0]; run 30 stopped there after press start
    0x005b2bd0, 0x005d3120, 0x006db840, 0x0080f8c0, 0x00906f90,
    # Registered by push 0x668a10 at 00669d83 (callback table built by 00664e90); discovery rejected it as a code
    # pointer that "runs into an entry". Run 31 stopped there at "Start Career". It was the only rejected candidate
    # in the analysis JSON with a function prologue after padding.
    0x00668a10,
    # Vtable / table dwords that discovery took for text (0x00530030 after 0x00530020 reads as UTF-16; a dword after
    # four printable bytes). Run 32 stopped at 0x00530030 (vtable 009cde88, called from 004a777c) ~50 s into the
    # front end. All start after padding and decode as functions (work/nfsc_look.py missing, 2026-11-16).
    0x00530030, 0x00530060,                 # vtable 009cde88 / 009cde8c
    0x00670060,                             # 009e1784, after 0x00670050
    0x0050fe30,                             # deleting destructor, vtable 009cb6b4 (after a float constant)
    0x00500040, 0x00736f70,                 # only "text" references (00a04528..., 00a1a3d0); harmless if never called
]

# Widescreen (src/nfsc/widescreen.cpp), after ThirteenAG's NFSCarbon.WidescreenFix (MIT) as the macOS port applies it
# (refs/nfs-carbon-mac game.toml operand_redirects / instruction_patches, mods/core/nfsc/nfsc.c). The game keeps its
# 640x480 back buffer, which the Vita shows stretched to 960x544; the picture is drawn for the display's shape
# (anamorphic), so the stretch gives the right proportions without the cost of more pixels. Reads of shared constants
# go to slots in the zero padding after .rsrc that widescreen.cpp fills; immediates that cannot read a slot get the
# 960x544 values here.
def _f32(value):
    return struct.pack('<f', value).hex()

WS_ASPECT = 960.0 / 544.0
WS_CENTRE = 240.0 * WS_ASPECT                       # HUD centre: 320 at 4:3
WS_OFFSET = WS_CENTRE - 320.0                       # side groups move out by this (the game's 120.0)
WIDESCREEN_PATCHES = [
    # Projection 0071b780: the aspect term [0xab0acc] / [0xab0ac8] (back buffer height / width), the horizontal
    # 1.0, the vertical half angle factor 0.5 (second use) and the vertical 1.0 read slots 0xc2b440..0xc2b450.
    (0x0071b858, 'db05cc0aab00', 'db0540b4c200'),
    (0x0071b864, 'da35c80aab00', 'da3544b4c200'),
    (0x0071b8da, 'd83d78249c00', 'd83d48b4c200'),
    (0x0071b8ec, 'd80d88289c00', 'd80d4cb4c200'),
    (0x0071b923, 'd83d78249c00', 'd83d50b4c200'),
    # FEngHud::SetWideScreenMode 005d5280: the minimap's 120.0 (shared constant 0x9cbb94) -> slot 0xc2b454.
    (0x005d52f9, 'd82594bb9c00', 'd82554b4c200'),
    (0x005d5356, 'd80594bb9c00', 'd80554b4c200'),
    # Menu cursor 00731665 and the Autosculpt zone line 007c9214 / 007c925b: * 640.0 -> the wide menu's width.
    (0x00731665, 'd80d689b9e00', 'd80d58b4c200'),
    (0x007c9214, 'd80d689b9e00', 'd80d58b4c200'),
    (0x007c925b, 'd80d689b9e00', 'd80d58b4c200'),
    # Six HUD set-ups store the HUD centre 320.0.
    (0x00598db9, 'c78424840000000000a043', 'c7842484000000' + _f32(WS_CENTRE)),
    (0x00598fb0, 'c78424c40000000000a043', 'c78424c4000000' + _f32(WS_CENTRE)),
    (0x0059940f, 'c78424840000000000a043', 'c7842484000000' + _f32(WS_CENTRE)),
    (0x005996a7, 'c78424d40000000000a043', 'c78424d4000000' + _f32(WS_CENTRE)),
    (0x005a18ba, 'c74424640000a043', 'c7442464' + _f32(WS_CENTRE)),
    (0x005d2b42, 'c74424640000a043', 'c7442464' + _f32(WS_CENTRE)),
    # The game's own widescreen HUD layout always on (the fix's SETE and JNZ bytes).
    (0x005dc507, '0f95c1', '0f94c1'),
    (0x005d52b2, '0f95c0', '0f94c0'),
    (0x005b6bae, '7407', '7507'),
    (0x005b6b5b, '7407', '7507'),
    # Picture-in-picture (00750d40 -> quad builder 00750b30): width 0.275 -> 0.28, x for the wide screen (the fix's
    # fPiPUCAPPosition); widescreen.cpp narrows the mirror and PiP quads.
    (0x00750de6, '68cdcc8c3e', '68' + _f32(0.28)),
    (0x00750df4, '68f4fdd43c', '68' + _f32(((320.0 + WS_OFFSET) / 320.0 - 1.042) / -2.0)),
]

# Byte patches applied to the code before discovery: (address, original bytes, new bytes) in hex. Generation stops
# when the executable's bytes differ from the original ones.
PATCHES = [
    # Track part of the streaming pool (pool 7, 006b6e00 / 00699a60): 0x441c000 or 0x445c000 (+0x80000) = 71.9 MB.
    # The career intro canyon (STREAML5RB) has one 74,368,384-byte section (number 2500); 0079ebc8 puts a section
    # 2500 that does not fit pool 7 into the main heap instead, a second 74 MB the Vita arena does not have
    # (run 33-35); that is how the PC game works too: pool 7 is not even registered at that point (run 36), so
    # the city's permanent section (L5RA.BUN: section 2500, 0x046ec580 bytes) always comes from the main heap.
    # The Vita has to pay for it with the pool, so the track part becomes a run-time value: both sites read the
    # slot 0x00c2b400 (zero padding after .rsrc, mapped), which main.cpp fills from the setting stream_track_kb.
    #   mov ebx, 0x445c000 / je +5 / mov ebx, 0x441c000  ->  mov ebx, [0xc2b400] / 6 nop
    (0x006b6ef2, 'bb00c045047405bb00c04104', '8b1d00b4c200909090909090'),
    #   mov eax, 0x445c000 / je +5 / mov eax, 0x441c000  ->  mov eax, [0xc2b400] / 7 nop
    (0x00699a68, 'b800c045047405b800c04104', 'a100b4c20090909090909090'),
    # Oversized track sections (number 2500 etc. that do not fit pool 7, 0079ebf4) come from the game's main pool,
    # which grows by malloc and never gives a block back: after the first load the freed 74 MB of the city's
    # section 2500 stay in the pool, get nibbled by small allocations, and the next load mallocs 74 MB more
    # (run 41/42: three loads during Start Career, each read 6.9 s). The allocation becomes malloc (thunk
    # 006a1560) and the three frees of section buffers become free (thunk 006a1590): unload 0079e75a, cancel
    # 0079a2b5, and the old buffer after compaction 006adc6d. Those sites also free blocks of the game's
    # allocator; the free shim forwards what is not a guest heap block to bFree (main.cpp setForeignFree).
    (0x0079ebfa, 'e83186cdff', 'e86129f0ff'),
    (0x0079a2b5, 'e8b6cacdff', 'e8d672f0ff'),
    (0x0079e75a, 'e81186cdff', 'e8312ef0ff'),
    (0x006adc6d, 'e8fe90dcff', 'e81e39ffff'),
    # Audio pool (0051f780, [0xa8c2b8] / size [0xa8c2c0]; the sound banks live there): 0x2000000 bytes when
    # [0xa631b8] is set, else 0x4000. Run-time value from the slot 0x00c2b404 (main.cpp, setting audio_pool_kb).
    #   mov eax, [0xa631b8] / neg eax / sbb eax, eax / and eax, 0x1ffc000 / add eax, 0x4000
    #   ->  mov eax, [0xc2b404] / 14 nop
    (0x0051f780, 'a1b831a600f7d81bc02500c0ff010500400000', 'a104b4c200' + '90' * 14),
] + WIDESCREEN_PATCHES

# Busy-wait loops: the generated code releases the execution lock there so other guest threads can run.
THREAD_SEGMENTS = []

# Net x87 pushes of functions the depth analysis cannot follow (they leave through a computed jump).
FPU_DELTAS = {}

# Net x87 pushes of msvcr71 imports that use the x87 stack: the double-returning functions push their result, the
# _CI* intrinsics take their operands in st0 (and st1) and leave the result in st0. Applied to calls through the
# import table and to the jmp [slot] thunks the linker placed in .text (their entries get FPU_DELTAS).
IMPORT_FPU_DELTAS = {
    'floor': 1, 'ceil': 1, 'sqrt': 1, 'cos': 1,
    '_CIpow': -1, '_CIfmod': -1,
    '_CIacos': 0, '_CIasin': 0, '_CIcosh': 0, '_CIsinh': 0, '_CItanh': 0,
}

# Functions that save the caller's flags on entry (pushfd) and restore them before returning (popfd).
FLAGS_PRESERVED = []

# Entries that no indirect call reaches although a data dword points at them. These were taken from dwords that only
# look like code addresses (0x6e6f4d is the text "Mon"; others start mid-instruction) and read flags at entry.
NOT_INDIRECT = [
    0x437000, 0x440053, 0x446881, 0x49fffe, 0x4e5854, 0x5aca06, 0x648f4a, 0x6683f0, 0x668574, 0x670029, 0x6e6f4d,
]

# Targets of indirect jumps whose callers read flags (site -> entries).
JUMP_TARGETS = {}

# SIMD code that exists only behind CPU feature tests (the runtime reports no SSE or 3DNow!): these ranges trap if
# reached. A SIMD instruction outside them counts as a problem. MMX (codegen/mmx.py) and the SSE float instructions
# (codegen/sse.py) are translated wherever they are. The first hardware run (2026-11-15) trapped at 0x8d7f29: EA's
# audio library 0x8a0000-0x8d9000 was compiled for SSE and runs it unconditionally (static initializer 0x9bdb70),
# so its instructions are translated; the range stays listed for any it uses that sse.py lacks.
SIMD_GATED = [
    (0x884000, 0x889000, 'VP6 video decoder SSE2 paths (VideoDecodeThread)'),
    (0x8a0000, 0x8d9000, 'EA audio library SSE code (translated; listed for unhandled ones)'),
]

# Names of the generated files: <module>.cpp/.h, <module>.<bucket>.cpp, the layout header and the bank routines.
GENERATED_FILE = re.compile(r'^%s(\.[0-9a-f]+)?\.(cpp|h)$|^%s_layout\.h$|^%s_bank\.cpp$' % (MODULE, MODULE, MODULE))


class GameModule:
    """The executable as the analyses and the writer see it."""
    def __init__(self, image, subroutines):
        self.application_name = MODULE
        self.image = image
        self.entry_point = image.entry_point
        self.exported_symbols = []
        self.subroutines = subroutines


def check_binary(directory):
    path = os.path.join(directory, EXE_NAME)
    if not os.path.isfile(path):
        sys.exit('missing %s' % path)
    # Checked before parsing: other files are refused whatever they contain.
    with open(path, 'rb') as f:
        sha256 = hashlib.sha256(f.read()).hexdigest()
    if sha256 != EXE_SHA256:
        sys.exit('%s: SHA-256 %s, expected %s' % (path, sha256, EXE_SHA256))
    return pe_image.Image(path)


def apply_patches(image):
    for address, original, patched in PATCHES:
        original, patched = bytes.fromhex(original), bytes.fromhex(patched)
        s = image.section_of(address)
        if s is None or len(original) != len(patched):
            sys.exit('patch 0x%x: no section or lengths differ' % address)
        offset = address - s.address
        if s.data[offset:offset + len(original)] != original:
            sys.exit('patch 0x%x: bytes %s, expected %s'
                     % (address, s.data[offset:offset + len(original)].hex(), original.hex()))
        s.data = s.data[:offset] + patched + s.data[offset + len(patched):]


def import_fpu_deltas(image, discovery):
    """IAT slot -> x87 delta of the IMPORT_FPU_DELTAS imports, and entry -> delta of their jmp [slot] thunks."""
    slots = dict((i.iat_address, IMPORT_FPU_DELTAS[i.name]) for i in image.imports if i.name in IMPORT_FPU_DELTAS)
    thunks = {}
    for entry in discovery.entries:
        ins = discovery.decoded.get(entry)
        if ins is None or ins.mnemonic != 'jmp' or len(ins.operands) != 1:
            continue
        op = ins.operands[0]
        if op.type == CX.X86_OP_MEM and op.mem.base == 0 and op.mem.index == 0 \
                and op.mem.disp & 0xffffffff in slots:
            thunks[entry] = slots[op.mem.disp & 0xffffffff]
    print('x87 imports: %d slots, %d thunks (%s)' % (len(slots), len(thunks),
                                                    ' '.join('%x:%+d' % kv for kv in sorted(thunks.items()))))
    return slots, thunks


def simd_range(address):
    return next((r for r in SIMD_GATED if r[0] <= address < r[1]), None)


def write_analysis(path, program, module, discovery):
    functions = []
    for sub in module.subroutines:
        a = sub.get_entry_point()
        info = program.stack.get(a)
        functions.append({'entry': a, 'origin': discovery.origin[a], 'ret_live': program.ret_live[a],
                          'live_in': program.live_in[a], 'preserved': info.preserved if info else 0,
                          'native': a in program.native_overrides, 'instructions': len(sub.instructions),
                          'no_return': a in discovery.no_return, 'address_taken': a in program.address_taken})
    sections = [{'name': s.name, 'address': s.address, 'length': s.virtual_size} for s in module.image.sections]
    tables = [{'address': t.address, 'targets': t.targets, 'bounded': t.bounded} for t in discovery.tables.values()]
    rejected = [{'address': a, 'origin': o, 'reason': r} for a, (o, r) in sorted(discovery.rejected.items())]
    sources = {'code_refs': {a: sorted(s) for a, s in discovery.code_refs.items()},
               'data_pointers': {a: sorted(s) for a, s in discovery.data_pointers.items()},
               'string_pointers': {a: sorted(s) for a, s in discovery.string_pointers.items()}}
    traps = [{'address': a, 'mnemonic': m, 'class': k} for a, m, k in sorted(set(codegen.simd_sites))]
    code_ranges = []
    for a in sorted(discovery.code):
        end = a + discovery.decoded[a].size
        if code_ranges and a <= code_ranges[-1][1]:
            code_ranges[-1][1] = max(code_ranges[-1][1], end)
        else:
            code_ranges.append([a, end])
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    with open(path, 'w') as f:
        json.dump({'functions': functions, 'sections': sections, 'tables': tables, 'gaps': discovery.gaps(),
                   'rejected': rejected, 'sources': sources, 'simd_traps': traps,
                   'unsupported': sorted(set(codegen.unhandled_sites)), 'rounds': discovery.rounds,
                   'index_tables': discovery.index_tables, 'code_ranges': code_ranges,
                   'mnemonics': dict(mnemonics), 'no_return': sorted(discovery.no_return)}, f)


def sync_output(stage, out_dir):
    """Copies the files generated in stage whose content changed (unchanged files keep their timestamps, so the build
    recompiles just the changed ones) and deletes generated files of earlier runs; other files are left alone."""
    names = set(os.listdir(stage))
    changed = removed = 0
    for name in sorted(names):
        assert GENERATED_FILE.match(name), name
        target = os.path.join(out_dir, name)
        if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
            shutil.copyfile(os.path.join(stage, name), target)
            changed += 1
    for name in os.listdir(out_dir):
        if name not in names and GENERATED_FILE.match(name) and os.path.isfile(os.path.join(out_dir, name)):
            os.remove(os.path.join(out_dir, name))
            removed += 1
    print('generated %d files into %s (%d changed, %d removed)' % (len(names), out_dir, changed, removed))


# .rdata the port writes at run time: --fold-rdata leaves these loads alone (widescreen.cpp ProtectedGameRefs,
# main.cpp sim_hz).
RDATA_WRITTEN = [0x009e8f8c, 0x009c778c, 0x009d5f3c, 0x009d0584, 0x009ec0a8, 0x009ebb6c, 0x009db360]

# Functions whose stack frame stays in guest memory with --promote-stack-slots / --separate-stack (frames.py):
# 008995ff (EA audio) reaches its own frame through a heap pointer (run 329: alias check at 0089962a).
FRAMES_EXCLUDE = [0x008995ff]


def promote_stack_slots(program, promote, separate):
    """--promote-stack-slots / --separate-stack (disasm/frames.py, from NFSUG2_VITA)."""
    started = time.time()
    analysis_ = frames.Analysis(program)
    result = analysis_.run(promote, separate)
    reasons = collections.Counter()
    accesses = promoted = saves = slots = functions = loads = separated = 0
    for a, f in result.items():
        if f.reason:
            reasons[f.reason.split(' 0x')[0]] += 1
            continue
        accesses += len(f.accesses)
        separated += len(f.stack_sites)
        if f.slots or f.stack_sites:
            program.functions[a].frame = f
        if f.slots:
            functions += 1
            slots += len(f.slots)
            loads += sum(s.entry_load for s in f.slots.values())
            promoted += len(f.sites)
            saves += sum(1 for s in f.sites.values() if s.save)
    print('stack slots: %d functions, %d slots (%d loaded at entry), %d of %d resolved stack accesses (%d register '
          'saves); %d shared esp changes solved, %d conflicting; %d indirect calls settled by choice (%.0f s)'
          % (functions, slots, loads, promoted, accesses, saves, len(analysis_.known), len(analysis_.conflicts),
             sum(len(v) for v in analysis_.chosen.values()), time.time() - started))
    print('stack slots: %d accesses marked as not aliasing other guest memory' % separated)
    print('stack slots: functions left as they are: %s' % ', '.join('%s %d' % kv for kv in reasons.most_common()))


def report(discovery, module, program):
    """Statistics and every place the generated code traps. Returns the number of problems: unsupported
    instructions in reachable code, SIMD outside the gated ranges and sites breaking the flag convention."""
    image = module.image
    origins = collections.Counter(discovery.origin[s.get_entry_point()] for s in module.subroutines)
    instructions = sum(len(s.instructions) for s in module.subroutines)
    code_bytes = sum(discovery.decoded[a].size for a in discovery.code)
    text = sum(s.virtual_size for s in image.code_sections)
    print('functions: %d (%s); %d no-return, %d address-taken; %d discovery rounds'
          % (len(module.subroutines), ', '.join('%s %d' % kv for kv in sorted(origins.items())),
             len(discovery.no_return), len(program.address_taken), discovery.rounds))
    print('instructions: %d emitted, %d distinct (%d of %d code-section bytes)'
          % (instructions, len(discovery.code), code_bytes, text))
    print('jump tables: %d (%d bounded by their switch), byte index tables: %d'
          % (len(discovery.tables), sum(t.bounded for t in discovery.tables.values()), len(discovery.index_tables)))
    reasons = collections.Counter(discovery.rejected.values())
    print('pointer candidates rejected: %d (%s); text dwords skipped: %d'
          % (len(discovery.rejected), ', '.join('%s/%s %d' % (o, r, n) for (o, r), n in sorted(reasons.items())),
             len(discovery.string_pointers)))
    gaps = discovery.gaps()
    print('code-section bytes outside functions and tables (not padding): %d in %d ranges'
          % (sum(b - a for a, b in gaps), len(gaps)))
    mnemonics = collections.Counter(discovery.decoded[a].mnemonic for a in discovery.code)
    print('mnemonics: %d distinct' % len(mnemonics))
    calls = fpustack.audit(program)
    print('indirect calls: %d; import table %d (host, no check); float result inferred %d, none inferred %d '
          '(checked: %s); open %d and no static x87 depth %d (stack in cpu.fpu, no check)'
          % (sum(calls.values()), calls.get(('import', 0), 0), calls.get(('inferred', 1), 0),
             calls.get(('inferred', 0), 0), 'yes' if program.indirect_fpu_checked else 'no',
             sum(n for (k, _), n in calls.items() if k == 'open'),
             sum(n for (k, _), n in calls.items() if k == 'unknown')))
    problems = 0
    if program.abi_flags:
        flags = analysis.C.ALL_FLAGS
        readers = sorted(a for a, m in program.live_in.items() if m & flags)
        producers = sorted(a for a, m in program.ret_live.items() if m & flags)
        contract = program.flag_contract()
        print('flag convention: %d functions read flags at entry, %d return flags; indirect jumps with flags: %d '
              '(%d functions write them back)' % (len(readers), len(producers), len(contract['indirect_tails']),
                                                  len(program.indirect_returners)))
        for kind, sites in contract.items():
            if kind == 'indirect_tails':
                continue
            for a, m in sites:
                print('FLAG CONVENTION BROKEN (%s): 0x%x %s' % (kind, a, analysis.state_names(m)))
                problems += 1
    traps = collections.defaultdict(set)
    for fn in program.functions.values():
        for i in fn.reachable:
            ins = fn.instructions[i]
            if fn.effects[i].boundary == 'trap':
                traps[getattr(ins, 'reason', None) or ins.mnemonic].add(ins.address)
    simd = set(a for a, _, _ in codegen.simd_sites)
    for name, sites in sorted(traps.items()):
        if sites & simd:
            continue
        sites = sorted(sites)
        print('trap: %-28s %4d site(s): %s' % (name, len(sites), ' '.join('0x%x' % a for a in sites[:12])))
    by_range = collections.Counter()
    ungated = collections.defaultdict(list)     # 64 KB region -> sites
    for a, m, kind in sorted(set(codegen.simd_sites)):
        r = simd_range(a)
        if r is None:
            ungated[a & ~0xffff].append((a, m, kind))
            problems += 1
        else:
            by_range[(r[2], kind)] += 1
    for region, sites in sorted(ungated.items()):
        kinds = collections.Counter(k for _, _, k in sites)
        print('UNGATED SIMD 0x%06x-0x%06x: %4d site(s) %s, first 0x%x %s, last 0x%x %s'
              % (region, region + 0xffff, len(sites), dict(kinds), sites[0][0], sites[0][1], sites[-1][0], sites[-1][1]))
    for (where, kind), n in sorted(by_range.items()):
        print('gated SIMD trap: %-6s %5d in %s' % (kind, n, where))
    if codegen.unhandled_sites:
        unhandled = collections.Counter(m for _, m in set(codegen.unhandled_sites))
        print('UNSUPPORTED instructions (trap at run time): %s' % ', '.join('%s x%d' % kv for kv in unhandled.items()))
        problems += len(set(codegen.unhandled_sites))
    return problems


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True, help='directory with %s' % EXE_NAME)
    parser.add_argument('--out', required=True, help='output directory for the generated sources')
    parser.add_argument('--analysis', help='also write the analysis summary (JSON) for test tools')
    parser.add_argument('--memory-x87', action='store_true',
                        help='keep the x87 stack in cpu.fpu instead of C++ locals (for comparisons)')
    parser.add_argument('--memory-registers', action='store_true',
                        help='keep x86 registers in cpu instead of C++ locals (for comparisons)')
    parser.add_argument('--flags-across-calls', action='store_true',
                        help='pass flags through calls and returns as any code may read them (for comparisons)')
    parser.add_argument('--flag-audit', action='store_true',
                        help='check at run time that every flag read from cpu was stored there (logs the reader)')
    parser.add_argument('--fpu-check', action='store_true',
                        help='assert the x87 depth after every call whose depth is known (finds leaking callees)')
    parser.add_argument('--promote-stack-slots', action='store_true',
                        help='keep stack slots no other code can see in C++ locals (disasm/frames.py)')
    parser.add_argument('--separate-stack', action='store_true',
                        help='tell clang that frame accesses and pushes never alias other guest accesses')
    parser.add_argument('--fold-rdata', action='store_true',
                        help='loads from .rdata (read-only on the PC) become constants, except RDATA_WRITTEN')
    parser.add_argument('--promote-bisect', action='store_true',
                        help='keep slots in locals only in functions from promote_lo to promote_hi (settings file)')
    parser.add_argument('--check-poison', action='store_true',
                        help='with --check-assumptions: promoted slots write a poison pattern to guest memory instead')
    parser.add_argument('--check-assumptions', action='store_true',
                        help='log at run time where the facts behind the three options above fail')
    parser.add_argument('--bank-only', action='store_true',
                        help='only (re)write %s_bank.cpp, the audio bank routines (seconds, no regeneration)' % MODULE)
    args = parser.parse_args()
    started = time.time()
    image = check_binary(args.binaries)
    apply_patches(image)
    print('%s: SHA-256 ok, %d sections, %d relocations, %d imports, %d patches'
          % (EXE_NAME, len(image.sections), len(image.relocations), len(image.imports), len(PATCHES)))
    os.makedirs(args.out, exist_ok=True)

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    if args.bank_only:
        with tempfile.TemporaryDirectory(prefix='nfscbank-') as stage:
            bankcode.write(args.binaries, image, capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32), stage,
                           BANK_LIBRARY_TABLE, BANK_DIR, MODULE)
            name = '%s_bank.cpp' % MODULE
            target = os.path.join(args.out, name)
            if not os.path.isfile(target) or not filecmp.cmp(os.path.join(stage, name), target, shallow=False):
                shutil.copyfile(os.path.join(stage, name), target)
                print('%s written' % target)
            else:
                print('%s unchanged' % target)
        return
    discovery = discover.Discovery(image, cs, no_return=NO_RETURN)
    discovery.discover(extra_entries=EXTRA_ENTRIES + NATIVE_OVERRIDES + ([SETJMP] if SETJMP is not None else []))
    print('discovery took %.0f s: %d functions' % (time.time() - started, len(discovery.entries)))
    subroutines = [discovery.subroutine(e) for e in sorted(discovery.entries)]
    print('subroutines built at %.0f s' % (time.time() - started))
    module = GameModule(image, subroutines)
    import_deltas, thunk_deltas = import_fpu_deltas(image, discovery)
    program = analysis.Program([module], THREAD_SEGMENTS, [], {}, FLAGS_PRESERVED, NOT_INDIRECT, NATIVE_OVERRIDES,
                               local_registers=not args.memory_registers, local_x87=not args.memory_x87,
                               setjmp=SETJMP, longjmp_jumps=LONGJMP_JUMPS, fpu_check=args.fpu_check,
                               fpu_deltas=dict(thunk_deltas, **FPU_DELTAS), preempt_loops=True,
                               address_taken=discovery.address_taken,
                               abi_flags=not args.flags_across_calls, jump_targets=JUMP_TARGETS,
                               flag_audit=args.flag_audit)
    program.import_fpu_deltas = import_deltas
    program.analyze()
    program.check_assumptions = args.check_assumptions
    program.fold_rdata = args.fold_rdata
    program.rdata_written = set(RDATA_WRITTEN)
    program.promote_bisect = args.promote_bisect
    program.check_poison = args.check_poison
    program.frames_exclude = set(FRAMES_EXCLUDE)
    if args.promote_stack_slots or args.separate_stack:
        promote_stack_slots(program, args.promote_stack_slots, args.separate_stack)
    print('analysis done at %.0f s' % (time.time() - started))
    # Every direct call and tail call must reach a generated function.
    missing = sorted(set(x.target for fn in program.functions.values() for i in fn.reachable for x in fn.exits[i]
                         if x.kind in ('call', 'tail') and x.target not in program.functions))
    if missing:
        sys.exit('calls to addresses that are no function: %s' % ' '.join('0x%x' % a for a in missing[:20]))
    with tempfile.TemporaryDirectory(prefix='nfscgen-') as stage:
        writer.Writer(module, program, stage).write()
        bankcode.write(args.binaries, image, capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32), stage,
                       BANK_LIBRARY_TABLE, BANK_DIR, MODULE)
        sync_output(stage, args.out)
    print('code written at %.0f s' % (time.time() - started))
    if args.analysis:
        write_analysis(args.analysis, program, module, discovery)
    problems = report(discovery, module, program)
    print('generation took %.0f s' % (time.time() - started))
    if problems:
        sys.exit('%d unsupported or ungated instruction(s) or flag convention break(s) in reachable code' % problems)


if __name__ == '__main__':
    main()

"""Spike: runs the UG1VITA function discovery over NFSC.exe (Carbon PC v1.4) and reports how big the
recompilation would be. No code is generated.

Usage: python -I tools/recompiler/spike_nfsc.py --binaries "<folder with NFSC.exe>" [--json out.json]
"""
import argparse
import collections
import hashlib
import json
import os
import sys
import time

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), 'disasm'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
import capstone  # noqa: E402
from disasm import discover, image as pe_image  # noqa: E402

EXE_NAME = 'NFSC.exe'
EXE_SHA256 = 'baaaaec33873d4ba7e4dbae6a5212049ffdcf9679a35d865d7625b4189952fd3'

# Entries only reached through pointers or threads, found by the macOS port's bring-up
# (github.com/vasist15-source/nfs-carbon-mac game.toml / CLAUDE.md).
EXTRA_ENTRIES = [
    0x004c5450, 0x0089122d, 0x0089f604, 0x0089f07c, 0x0089f494, 0x008999ee,
    0x0054bac0, 0x0088510e, 0x00884a1d, 0x0059d6d0, 0x0089777e, 0x008ab884,
    0x00555450, 0x005507a0, 0x005a69b0, 0x00868cc0, 0x00583d70,
]


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--binaries', required=True)
    p.add_argument('--json')
    args = p.parse_args()
    path = os.path.join(args.binaries, EXE_NAME)
    data = open(path, 'rb').read()
    digest = hashlib.sha256(data).hexdigest()
    print('%s sha256 %s (%s)' % (EXE_NAME, digest, 'v1.4 OK' if digest == EXE_SHA256 else 'UNEXPECTED'))
    image = pe_image.Image(path)
    print('entry 0x%x, %d sections, %d imports, %d relocations'
          % (image.entry_point, len(image.sections), len(image.imports), len(image.relocations)))
    dlls = collections.Counter(i.dll.lower() if hasattr(i, 'dll') else '?' for i in image.imports)
    print('imports by dll:', dict(dlls))

    cs = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    cs.detail = True
    t = time.time()
    d = discover.Discovery(image, cs)
    d.discover(extra_entries=EXTRA_ENTRIES)
    print('discovery: %.0f s, %d rounds' % (time.time() - t, d.rounds))

    origins = collections.Counter(d.origin[e] for e in d.entries)
    code_bytes = sum(d.decoded[a].size for a in d.code if d.decoded[a] is not None)
    text = sum(s.virtual_size for s in image.code_sections)
    gaps = d.gaps()
    mnem = collections.Counter(d.decoded[a].mnemonic for a in d.code if d.decoded[a] is not None)
    simd = {m: n for m, n in mnem.items()
            if m.startswith(('p', 'mov' + 'q', 'movd', 'movaps', 'movups', 'xorps', 'addps', 'mulps', 'cvt', 'sqrt',
                             'shufps', 'emms', 'femms'))}
    report = {
        'functions': len(d.entries),
        'origins': dict(origins),
        'instructions': len(d.code),
        'code_bytes': code_bytes,
        'text_bytes': text,
        'coverage': round(code_bytes / text, 3),
        'jump_tables': len(d.tables),
        'gap_bytes': sum(b - a for a, b in gaps),
        'gap_ranges': len(gaps),
        'mnemonics': len(mnem),
        'simd_like': dict(sorted(simd.items(), key=lambda kv: -kv[1])[:30]),
        'rejected_pointers': len(d.rejected),
    }
    for k, v in report.items():
        print('%-18s %s' % (k, v))
    # Rough C++ size estimate: UG1VITA emits about 60-120 bytes of ARM code per x86 instruction.
    print('estimated ARM code: %.0f-%.0f MB' % (len(d.code) * 60 / 2**20, len(d.code) * 120 / 2**20))
    if args.json:
        with open(args.json, 'w') as f:
            json.dump(report, f, indent=1)


if __name__ == '__main__':
    main()

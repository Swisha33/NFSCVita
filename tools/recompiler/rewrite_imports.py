"""Rewrites the import registration block of a generated <module>.cpp from the executable, without a new generation.

Usage: python tools/recompiler/rewrite_imports.py --binaries <dir with NFSC.exe> --out src/nfsc/generated

The block runs from the "// Imports:" comment that writer.py emits to the first guest function's registration.
Use it after changing how imports are named or bound (disasm/writer.py import_registration); the guest functions and
their registration are untouched.
"""
import argparse
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, 'disasm'))
sys.path.insert(0, HERE)
import bootstrap  # noqa: E402
bootstrap.ensure_capstone()
from disasm import image as pe_image, writer  # noqa: E402
import generate  # noqa: E402

BEGIN = '    // Imports:'


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument('--binaries', required=True)
    parser.add_argument('--out', required=True)
    args = parser.parse_args()
    image = generate.check_binary(args.binaries)
    path = os.path.join(args.out, '%s.cpp' % generate.MODULE)
    with open(path, encoding='utf-8') as f:
        lines = f.read().split('\n')
    begin = next(i for i, l in enumerate(lines) if l.startswith(BEGIN))
    # The first guest function's registration ends the block: its name has no "<dll>" prefix.
    end = next(i for i, l in enumerate(lines) if i > begin and l.startswith('    registerMethod(')
               and '{"<' not in l)
    lines[begin + 1:end] = writer.import_registration(image.imports)
    with open(path, 'w', newline='\n', encoding='utf-8') as f:
        f.write('\n'.join(lines))
    print('%s: %d imports rewritten' % (path, len(image.imports)))


if __name__ == '__main__':
    main()

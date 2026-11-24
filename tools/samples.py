"""Reads samples.bin from the sampling profiler (src/platform/sampler.cpp, setting sampler=on).

    python tools/samples.py samples.bin build-release-clang/NFSCVita.elf [--top N] [--callers]

Header: 6 words (magic "SMP2", pairs, interval us, pc context word, address of pf::sampler::start, failures), then
(pc, lr) pairs; (0xffffffff, value) is a mark, (0xff000000 | wait type, wait id) a sample of the thread waiting.
Functions are named with llvm-nm (NFSU_LLVM or the pinned LLVM in .deps); generated guest functions carry their
x86 address in the name. Prints the hottest functions by own samples and, with --callers, the lr of the top ones.
"""
import argparse, bisect, collections, glob, os, re, struct, subprocess, sys

REPO = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))


def find_nm():
    cands = []
    if os.environ.get('NFSU_LLVM'):
        cands.append(os.path.join(os.environ['NFSU_LLVM'], 'bin', 'llvm-nm.exe'))
        cands.append(os.path.join(os.environ['NFSU_LLVM'], 'bin', 'llvm-nm'))
    cands += glob.glob(os.path.join(REPO, '.deps', '**', 'bin', 'llvm-nm*'), recursive=True)
    for c in cands:
        if os.path.isfile(c):
            return c
    sys.exit('llvm-nm not found (set NFSU_LLVM)')


def symbols(elf):
    out = subprocess.run([find_nm(), '--numeric-sort', '--defined-only', '-C', elf], capture_output=True,
                         text=True, errors='replace').stdout
    addrs, names = [], []
    for line in out.splitlines():
        parts = line.split(' ', 2)
        if len(parts) < 3 or parts[1] not in 'tTwW':
            continue
        try:
            a = int(parts[0], 16) & ~1
        except ValueError:
            continue
        addrs.append(a)
        names.append(parts[2])
    return addrs, names


def short(name):
    m = re.search(r'sub_([0-9a-fA-F]{8})', name)
    return ('guest ' + m.group(1).lower()) if m else name[:110]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=45)
    ap.add_argument('--callers', action='store_true')
    ap.add_argument('--mark', type=lambda s: int(s, 0), default=None,
                    help='count only samples taken while the mark had this value (sampler::mark)')
    ap.add_argument('--bias', type=lambda s: int(s, 0), default=None)
    ap.add_argument('--pcs', metavar='NAME', help='also list the hottest ELF addresses inside functions whose '
                    'name contains NAME (for llvm-objdump --start-address)')
    a = ap.parse_args()
    data = open(a.samples, 'rb').read()
    magic, pairs, interval, pcword, start_addr, failures = struct.unpack_from('<6I', data, 0)
    words = struct.unpack_from('<%dI' % ((len(data) - 24) // 4), data, 24)
    addrs, names = symbols(a.elf)
    bias = 0
    if 'pf::sampler::start' in ' '.join(names[:0]) or True:
        for i, n in enumerate(names):
            if n.startswith('pf::sampler::start('):
                bias = (start_addr & ~1) - addrs[i]
                break
    own = collections.Counter()
    pcs = collections.Counter()
    callers = collections.defaultdict(collections.Counter)
    waits = collections.Counter()
    total = running = 0
    mark = 0
    i = -2
    while i + 3 < len(words):
        i += 2
        pc, lr = words[i], words[i + 1]
        if pc >> 24 == 0xfe:            # sampler_stack record: skip its return addresses (samples_stack.py reads them)
            i += ((pc & 0xffffff) + 1) & ~1
            continue
        if pc == 0xffffffff:
            mark = lr
            continue
        if a.mark is not None and mark != a.mark:
            continue
        total += 1
        if pc >= 0xff000000:
            waits[(pc & 0xffffff, lr)] += 1
            continue
        running += 1
        k = bisect.bisect_right(addrs, (pc - bias) & ~1) - 1
        name = names[k] if k >= 0 else '?'
        own[name] += 1
        if a.pcs and a.pcs in name:
            pcs[(pc - bias) & ~1] += 1
        if a.callers:
            kl = bisect.bisect_right(addrs, (lr - bias) & ~1) - 1
            callers[name][names[kl] if kl >= 0 else '?'] += 1
    print('%d samples every %u us (%.1f s), %d running, %d waiting, %u failures, load bias 0x%x' % (
        total, interval, total * interval / 1e6, running, total - running, failures, bias))
    if a.bias is not None:
        bias = a.bias
    for (wtype, wid), n in waits.most_common(8):
        print('  waiting type 0x%x id 0x%08x: %.1f%%' % (wtype, wid, 100.0 * n / max(total, 1)))
    print('%-8s %-6s %s' % ('samples', '%', 'function'))
    cum = 0
    for name, n in own.most_common(a.top):
        cum += n
        print('%-8d %5.2f%% %5.1f%% %s' % (n, 100.0 * n / max(running, 1), 100.0 * cum / max(running, 1), short(name)))
        if a.callers:
            for c, m in callers[name].most_common(3):
                print('             <- %5.1f%% %s' % (100.0 * m / n, short(c)))
    # Guest code vs runtime
    guest = sum(n for name, n in own.items() if 'sub_' in name)
    print('guest functions: %.1f%% of running samples' % (100.0 * guest / max(running, 1)))
    if a.pcs:
        hot = sum(pcs.values())
        print('hottest addresses in functions matching %r (%d samples):' % (a.pcs, hot))
        for pc, n in pcs.most_common(40):
            print('  0x%08x %5.2f%%' % (pc, 100.0 * n / max(hot, 1)))


if __name__ == '__main__':
    main()


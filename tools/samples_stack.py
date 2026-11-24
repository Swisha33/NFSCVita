"""Inclusive (call tree) profile from a sampler_stack=on run: python tools/samples_stack.py samples.bin ELF [options]

Each running sample carries the return addresses the sampler found on the game thread's stack (src/platform/
sampler.cpp, sampler_stack). A function counts once per sample if the pc or any return address lies in it, so its
share is the time spent in it and everything it called. Stale words on the stack can add a little noise.
With sampler_frame_marks=on the samples are also split by the length of the frame they belong to (--slow MS):
the table then shows each function's ms per frame in fast and slow frames, which is what grows when fps drops.
"""
import argparse, bisect, collections, os, re, struct, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from samples import symbols, short


class ElfCode:
    """Reads instruction bytes of an ARM ELF by virtual address."""
    def __init__(self, path):
        self.data = open(path, 'rb').read()
        d = self.data
        phoff, = struct.unpack_from('<I', d, 0x1c)
        phentsize, phnum = struct.unpack_from('<HH', d, 0x2a)
        self.segs = []
        for k in range(phnum):
            t, off, va, pa, fsz, msz = struct.unpack_from('<6I', d, phoff + k * phentsize)
            if t == 1:
                self.segs.append((va, off, fsz))

    def half(self, va):
        for base, off, sz in self.segs:
            if 0 <= va - base and va - base + 2 <= sz:
                return struct.unpack_from('<H', self.data, off + va - base)[0]
        return None

    def call_before(self, ret):
        """('direct', target) / ('indirect', None) / (None, None) for the instruction before return address ret."""
        if ret & 1:
            a = ret & ~1
            h1, h2 = self.half(a - 4), self.half(a - 2)
            if h1 is not None and h2 is not None and (h1 & 0xf800) == 0xf000 and (h2 & 0xc000) == 0xc000:
                s_ = (h1 >> 10) & 1
                j1, j2 = (h2 >> 13) & 1, (h2 >> 11) & 1
                i1, i2 = 1 - (j1 ^ s_), 1 - (j2 ^ s_)
                off = (s_ << 24) | (i1 << 23) | (i2 << 22) | ((h1 & 0x3ff) << 12) | ((h2 & 0x7ff) << 1)
                if s_:
                    off -= 1 << 25
                if h2 & 0x1000:
                    return 'direct', (a + off) & ~1          # BL
                return 'direct', ((a + off) & ~3)            # BLX immediate (to ARM)
            if h2 is not None and (h2 & 0xff87) == 0x4780:
                return 'indirect', None
            return None, None
        lo, hi = self.half(ret - 4), self.half(ret - 2)
        if lo is None or hi is None:
            return None, None
        w = lo | (hi << 16)
        if (w & 0x0f000000) == 0x0b000000:
            off = (w & 0xffffff) << 2
            if off & 0x2000000:
                off -= 1 << 26
            return 'direct', ret + off + 4
        if (w & 0x0ffffff0) == 0x012fff30:
            return 'indirect', None
        return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('samples')
    ap.add_argument('elf')
    ap.add_argument('--top', type=int, default=60)
    ap.add_argument('--slow', type=int, default=55, help='frames at least this long (ms) are slow')
    ap.add_argument('--fast', type=int, default=42, help='frames at most this long (ms) are fast')
    ap.add_argument('--guest', action='store_true', help='list only translated guest functions')
    ap.add_argument('--skip', type=int, default=0, help='ignore the first N frames (loading)')
    ap.add_argument('--raw', action='store_true', help='count every return address found (no chain check)')
    ap.add_argument('--under', help='only samples with this function (name substring) on the stack')
    a = ap.parse_args()
    data = open(a.samples, 'rb').read()
    magic, pairs, interval, pcword, start_addr, failures = struct.unpack_from('<6I', data, 0)
    words = struct.unpack_from('<%dI' % ((len(data) - 24) // 4), data, 24)
    addrs, names = symbols(a.elf)
    bias = 0
    for k, n in enumerate(names):
        if n.startswith('pf::sampler::start('):
            bias = (start_addr - addrs[k]) & ~1
            break
    cache = {}
    elf = ElfCode(a.elf)
    starts = {}
    for k, n in enumerate(names):
        starts.setdefault(n, addrs[k])

    def fstart(x):
        k = bisect.bisect_right(addrs, x) - 1
        return addrs[k] if k >= 0 else -1

    def chain(pc, lr, st):
        """Names of the verified call chain: pc's function, then each return address whose call instruction calls the
        function of the frame below it (BL / BLX immediate decoded; indirect calls and long-branch thunks accepted)."""
        cur = fstart((pc - bias) & ~1)
        out = {fn(pc)}
        for w in [lr] + list(st):
            r = (w - bias) & ~1
            kind, target = elf.call_before(w - bias)
            if kind is None:
                continue
            if kind == 'direct':
                t = fstart(target)
                tn = names[bisect.bisect_right(addrs, target) - 1] if t >= 0 else ''
                if t != cur and 'Thunk' not in tn and 'veneer' not in tn:
                    continue
            out.add(fn(w))
            cur = fstart(r)
        return out

    def fn(x):
        x = (x - bias) & ~1
        r = cache.get(x)
        if r is None:
            k = bisect.bisect_right(addrs, x) - 1
            r = cache[x] = names[k] if k >= 0 else '?'
        return r

    # Collect per-frame sample sets.
    frames = []          # (ms, [set of names per running sample], waiting count)
    cur, curwait = [], 0
    pending = None       # (pc, lr) waiting for its stack record
    frame_no = 0
    i = 0
    n = len(words)

    def flush_pending():
        nonlocal pending
        if pending is not None:
            cur.append({fn(pending[0])})
            pending = None

    while i + 1 < n:
        pc, lr = words[i], words[i + 1]
        if pc >> 24 == 0xfe:
            k = pc & 0xffffff
            st = words[i + 2:i + 2 + k]
            if pending is not None:
                cur.append(chain(pending[0], pending[1], st) if not a.raw else
                           {fn(pending[0])} | {fn(w) for w in st})
                pending = None
            i += 2 + ((k + 1) & ~1)
            continue
        flush_pending()
        i += 2
        if pc == 0xffffffff:
            if lr & 0x80000000:
                frame_no += 1
                frames.append((lr & 0xff, cur, curwait))
                cur, curwait = [], 0
            continue
        if pc >= 0xff000000:
            curwait += 1
            continue
        pending = (pc, lr)
    flush_pending()
    if not frames:
        frames.append((0, cur, curwait))
    frames = frames[1 + a.skip:]   # the first chunk ends at the first mark: no frame length known
    groups = {'all': [], 'fast': [], 'slow': []}
    for f in frames:
        groups['all'].append(f)
        if f[0] >= a.slow:
            groups['slow'].append(f)
        elif f[0] <= a.fast:
            groups['fast'].append(f)

    def stats(fs):
        cnt = collections.Counter()
        total = sum(len(f[1]) + f[2] for f in fs)
        ms = sum(f[0] for f in fs)
        for f in fs:
            for s in f[1]:
                if a.under and not any(a.under in x for x in s):
                    continue
                cnt.update(s)
        return cnt, total, ms, len(fs)

    res = {g: stats(fs) for g, fs in groups.items()}
    cnt, total, ms, nf = res['all']
    print('%d frames, %d samples every %u us; fast (<= %d ms) %d frames, slow (>= %d ms) %d frames' % (
        nf, total, interval, a.fast, res['fast'][3], a.slow, res['slow'][3]))
    for g in ('all', 'fast', 'slow'):
        c, t, m, f = res[g]
        if f:
            print('  %-4s mean frame %.1f ms, %.1f samples per frame' % (g, m / f, t / f))
    print('%-7s %8s %8s %8s %8s  %s' % ('incl %', 'ms/frm', 'fast', 'slow', 'grow', 'function'))

    def per_frame(g, name):
        c, t, m, f = res[g]
        if not f or not t:
            return 0.0
        return c[name] / t * (m / f)   # share of samples x mean frame length

    rows = []
    for name, c in cnt.items():
        if a.guest and 'sub_' not in name:
            continue
        rows.append((c, name))
    rows.sort(reverse=True)
    for c, name in rows[:a.top]:
        fa, sl = per_frame('fast', name), per_frame('slow', name)
        print('%6.1f%% %8.2f %8.2f %8.2f %+8.2f  %s' % (100.0 * c / max(total, 1), per_frame('all', name), fa, sl,
                                                       sl - fa, short(name)))
    print('\nbiggest growth from fast to slow frames (ms per frame):')
    grow = sorted(((per_frame('slow', n) - per_frame('fast', n), n) for c, n in rows), reverse=True)
    for d, name in grow[:a.top // 2]:
        print('%+8.2f  fast %6.2f slow %6.2f  %s' % (d, per_frame('fast', name), per_frame('slow', name), short(name)))


if __name__ == '__main__':
    main()

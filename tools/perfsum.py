"""Mean frame rate of a run log's driving stretch: python tools/perfsum.py LOG [LOG...] [--skip N]

Averages the 'perf: N fps' lines after the first N (default 7: loading and the start of the race) as frames per
second over time (total frames / total seconds, so slow stretches weigh by their length), plus the mean draw calls.
Meant for ai_drive runs (src/nfsc/main.cpp), whose route repeats.
"""
import re, sys

args = [a for a in sys.argv[1:] if not a.startswith('--')]
skip = 7
if '--skip' in sys.argv:
    skip = int(sys.argv[sys.argv.index('--skip') + 1])
    args = [a for a in args if a != str(skip)]
for path in args:
    text = open(path, encoding='utf-8', errors='replace').read()
    rows = re.findall(r'perf: ([0-9.]+) fps \| frame ([0-9.]+) ms.*?\| (\d+) calls', text)[skip:]
    if not rows:
        print(path, 'no perf lines')
        continue
    frame_ms = sum(float(r[1]) for r in rows) / len(rows)
    fps = 1000.0 / frame_ms
    calls = sum(int(r[2]) for r in rows) / len(rows)
    print('%s: %.1f fps (mean frame %.1f ms) over %d lines, %.0f draw calls, min %.1f max %.1f fps' % (
        path, fps, frame_ms, len(rows), calls, min(float(r[0]) for r in rows), max(float(r[0]) for r in rows)))

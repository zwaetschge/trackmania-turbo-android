#!/usr/bin/env python3
"""Summarise the bridge's occlusion-query log (TM_QUERY_LOG=4) per settings phase.

    tools/occ_log.py <log>

A phase starts at every line that reports a settings change ("... is now ...", "query
results wait ...", "flush on a pending query ..."). For each phase: the number of results,
the distribution of values, and the largest ones with their frames and callers.
"""
import collections
import re
import sys

phases = [['start', []]]
stats = {}
for line in open(sys.argv[1], errors='replace'):
    m = re.search(r'occ (\w+) frame (\d+) pass (-?\d+) = (\d+) \(from guest (\w+)\)', line)
    if m:
        phases[-1][1].append((m.group(1), int(m.group(2)), int(m.group(3)), int(m.group(4)), m.group(5)))
        continue
    if 'is now' in line or 'wait for their' in line or 'flush on a pending' in line:
        t = re.sub(r'.*\] ', '', line.strip())
        if phases[-1][1]:
            phases.append([t, []])
        else:
            phases[-1][0] = (phases[-1][0] + ' | ' + t) if phases[-1][0] != 'start' else t
        continue
    m = re.search(r'queries per frame: (.*)', line)
    if m:
        stats[len(phases) - 1] = m.group(1)
for i, (name, rows) in enumerate(phases):
    if not rows:
        continue
    vals = [r[3] for r in rows]
    frames = sorted({r[1] for r in rows})
    dist = collections.Counter(vals)
    small = sum(c for v, c in dist.items() if v <= 16)
    print('== %s' % name[-150:])
    print('   %d results in %d frames (%d queries); values <= 16: %d; larger: %d; distinct values: %s' % (
        len(vals), len(frames), len({r[0] for r in rows}), small, len(vals) - small,
        ', '.join('%d x%d' % (v, c) for v, c in sorted(dist.items())[:14])))
    big = sorted(rows, key=lambda r: -r[3])[:5]
    print('   largest: ' + '; '.join('%d (frame %d, query %s, pass %d, guest %s)' % (r[3], r[1], r[0][-4:], r[2], r[4]) for r in big))
    if i in stats:
        print('   last stats: ' + stats[i][:150])

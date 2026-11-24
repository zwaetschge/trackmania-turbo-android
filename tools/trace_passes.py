#!/usr/bin/env python3
"""Summarise the bridge's one-frame traces (F10) from a log: one line per render pass.

    tools/trace_passes.py <log> [frame_index=all]

Targets are given short names by size/format in order of first appearance (A, B, ...), so
two frames can be compared side by side; copies, clears and feedback hits are appended.
"""
import re
import sys


def frames(path):
    cur = None
    for line in open(path, errors='replace'):
        m = re.search(r'trace (begin|end|p\d+)(.*)', line)
        if not m:
            continue
        if m.group(1) == 'begin':
            cur = []
        elif m.group(1) == 'end':
            if cur is not None:
                yield cur
            cur = None
        elif cur is not None:
            cur.append((int(m.group(1)[1:]), m.group(2).strip()))


def summarise(fr):
    names = {}
    passes = {}

    def name(tid, desc):
        if tid not in names:
            names[tid] = '%s%d' % (chr(ord('A') + len(names) % 26), len(names) // 26) if len(names) >= 26 else chr(ord('A') + len(names))
            names[tid] += '(' + desc + ')'
        return names[tid].split('(')[0]
    out = []
    for p, text in fr:
        e = passes.setdefault(p, {'targets': '', 'draws': 0, 'ev': []})
        if text.startswith('targets:'):
            ts = re.findall(r'\[(\w+) (\d+x\d+) fmt (\d+)[^\]]*\]', text)
            d = re.search(r'depth (\w+)', text)
            e['targets'] = '{' + ','.join(name(t, '%s f%s' % (sz, f)) for t, sz, f in ts) + '}' + (' +D' if d and d.group(1) != '00000000' else '')
        elif text.startswith('='):
            m = re.match(r'= (\d+) draws', text)
            if m:
                passes.setdefault(p, e)['draws'] = int(m.group(1))
        elif text.startswith('('):
            continue
        else:
            e['ev'].append(text.split(':')[0][:60])
    for p in sorted(passes):
        e = passes[p]
        ev = {}
        for x in e['ev']:
            k = x.split(' ')[0]
            ev[k] = ev.get(k, 0) + 1
        out.append('p%02d %-22s %4d draws  %s' % (p, e['targets'], e['draws'], ' '.join('%s x%d' % kv for kv in ev.items())))
    return out, names


def main():
    fs = list(frames(sys.argv[1]))
    which = int(sys.argv[2]) if len(sys.argv) > 2 else None
    for i, fr in enumerate(fs):
        if which is not None and i != which:
            continue
        out, names = summarise(fr)
        print('== frame %d: %d passes' % (i, len(out)))
        print('   targets: ' + '  '.join(sorted(names.values())))
        for l in out:
            print('   ' + l)


if __name__ == '__main__':
    main()

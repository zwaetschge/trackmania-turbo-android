#!/usr/bin/env python3
"""Where do the submission boundaries fall in a traced frame (F10) for each TM_RP_SPLIT mode?

    tools/trace_splits.py <log> [frame_index=0] [from_pass to_pass]

Replays rp_split()'s rules on the pass list of a one-frame trace and prints, per pass, the
targets, the recorded work and a mark for every mode that flushes BEFORE that pass.
"""
import re
import sys

sys.path.insert(0, __file__.rsplit('/', 1)[0])
import trace_passes as tp


def passes(fr):
    P = {}
    for p, text in fr:
        e = P.setdefault(p, {'t': None, 'd': '0', 'work': 0, 'ev': [], 'desc': ''})
        if text.startswith('targets:'):
            body = text.split('targets:')[1]
            e['t'] = [tok if tok else None for tok in re.findall(r'\[(\w+) [^\]]*\]|-', body.split('depth')[0])]
            e['desc'] = ' '.join('%sx%s/f%s' % (a, b, c) for a, b, c in re.findall(r'\[\w+ (\d+)x(\d+) fmt (\d+)', body))
            e['d'] = re.search(r'depth (\w+)', body).group(1)
        elif text.startswith('='):
            e['draws'] = int(re.match(r'= (\d+) draws', text).group(1))
            e['work'] += e['draws']
        elif text.startswith('('):
            pass
        else:
            k = text.split(' ')[0]
            if k == 'query':
                k = 'query' + text.split(' ')[1]
            e['ev'].append(k)
            if k in ('ClearRenderTargetView', 'ClearDepthStencilView', 'CopyResource', 'CopySubresourceRegion', 'GenerateMips', 'UpdateSubresource'):
                e['work'] += 1
    return [(k, P[k]) for k in sorted(P)]


def sim(ps, mode):
    seen, sets, cur, work, prev, out = {}, {}, None, 0, None, set()
    for i, p in ps:
        if p['t'] is None:
            continue
        key = (tuple(p['t']), p['d'])
        if key == prev:
            work += p['work']
            continue
        if cur is not None:
            sets[cur] = work
        ncol = sum(1 for t in p['t'] if t)
        split = False
        if mode == 3:
            split = True
        elif mode == 4:
            split = work > 0
        elif mode:
            split = any(t and t in seen and seen[t] != sl for sl, t in enumerate(p['t'])) or ncol > 1
            if not split and mode == 1 and key in sets and work > sets[key]:
                split = True
        if split:
            out.add(i)
            seen, sets, work = {}, {}, 0
        sets.setdefault(key, work)
        cur = key
        for sl, t in enumerate(p['t']):
            if t:
                seen[t] = sl
        work += p['work']
        prev = key
    return out


def main():
    fs = list(tp.frames(sys.argv[1]))
    fi = int(sys.argv[2]) if len(sys.argv) > 2 else 0
    lo = int(sys.argv[3]) if len(sys.argv) > 3 else 0
    hi = int(sys.argv[4]) if len(sys.argv) > 4 else 999
    ps = passes(fs[fi])
    m = {k: sim(ps, k) for k in (1, 4, 5)}
    print('frame %d: %d passes; flushes: mode 1 %d, mode 4 %d, mode 5 %d' % (fi, len(ps), len(m[1]), len(m[4]), len(m[5])))
    print('  pass  flush-before(1 4 5)  targets                         draws  events')
    for i, p in ps:
        if not lo <= i <= hi:
            continue
        ev = {}
        for x in p['ev']:
            ev[x] = ev.get(x, 0) + 1
        print('  p%02d   %s %s %s   %-34s %4d  %s' % (i, '1' if i in m[1] else '.', '4' if i in m[4] else '.', '5' if i in m[5] else '.',
              (p['desc'] + (' +D' if p['d'] != '00000000' else ''))[:34], p.get('draws', 0), ' '.join('%s x%d' % kv for kv in ev.items())))


if __name__ == '__main__':
    main()

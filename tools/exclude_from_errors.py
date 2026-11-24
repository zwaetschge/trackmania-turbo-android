"""Map clang errors in gen/extra/*.c (work/extra_syntax.log) to their enclosing
lifted function and append those VAs to config/exclude.txt (garbage seeds: a
branch into the middle of an instruction has no label). Prints the count."""
import re, sys, collections, os
errs = collections.defaultdict(list)
for line in open('work/extra_syntax.log'):
    m = re.match(r'(?:/w/)?(gen/extra/[^:]+):(\d+):\d+: error: (.*)', line)
    if m: errs[m.group(1)].append((int(m.group(2)), m.group(3).strip()))
bad = {}
for path, items in errs.items():
    starts = []  # (line, va)
    for n, l in enumerate(open(path), 1):
        if l.startswith('void sub_'):
            starts.append((n, int(l[9:17], 16)))
    import bisect
    ls = [s[0] for s in starts]
    for ln, msg in items:
        i = bisect.bisect_right(ls, ln) - 1
        if i >= 0: bad.setdefault(starts[i][1], msg)
have = set()
if os.path.exists('config/exclude.txt'):
    for l in open('config/exclude.txt'):
        l = l.split('#')[0].strip()
        if l: have.add(int(l.split()[0], 16))
new = sorted(set(bad) - have)
with open('config/exclude.txt', 'a') as fh:
    for va in new:
        fh.write(f"{va:08X} garbage seed (not a function start): {bad[va][:60]}\n")
print(f"functions with errors: {len(bad)}, newly excluded: {len(new)}, total excluded: {len(have) + len(new)}")

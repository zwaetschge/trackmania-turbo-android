"""Convert a frame capture (cap_NNN_*.bin from the D3D11 bridge, F12) to PNGs.
usage: cap2png.py <capture dir> <out dir>
Float targets are tone-mapped (x/(1+x)), depth and single-channel floats are
range-normalised, so the pictures show structure, not exact colours.
stats.txt lists per target: size, format, mean colour and value range."""
import os, sys, struct, zlib, glob
import numpy as np

src, out = sys.argv[1], sys.argv[2]
os.makedirs(out, exist_ok=True)


def png(path, a):
    a = np.ascontiguousarray(np.clip(a, 0, 255).astype(np.uint8))
    h, w, c = a.shape
    raw = b''.join(b'\x00' + a[y].tobytes() for y in range(h))

    def ch(t, d):
        return struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ch(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + ch(b'IDAT', zlib.compress(raw, 6)) + ch(b'IEND', b''))


def tone(x):
    x = np.nan_to_num(x.astype(np.float32), nan=0.0, posinf=1e6, neginf=0.0)
    x = np.maximum(x, 0)
    return 255.0 * np.power(x / (1.0 + x), 1 / 2.2)


def norm(x):
    x = x.astype(np.float64)
    fin = np.isfinite(x)
    if not fin.any():
        return np.zeros(x.shape)
    lo, hi = np.percentile(x[fin], 1), np.percentile(x[fin], 99)
    return 255.0 * np.clip((np.nan_to_num(x, nan=lo, posinf=hi, neginf=lo) - lo) / (hi - lo if hi > lo else 1.0), 0, 1)


def uf(v, mb):     # unsigned small float: mb mantissa bits, 5 exponent bits
    m = (v & ((1 << mb) - 1)).astype(np.float32)
    e = (v >> mb).astype(np.int32)
    return np.where(e == 0, m / (1 << mb) * 2.0 ** -14,
                    np.where(e == 31, 65504.0, (1 + m / (1 << mb)) * np.exp2(e - 15.0)))


def gray(g):
    return np.stack([g, g, g], -1)


stats = []
for f in sorted(glob.glob(os.path.join(src, 'cap_*.bin'))):
    d = open(f, 'rb').read()
    nl = d.index(b'\n')
    _, w, h, bpp, fmt = d[:nl].split()
    w, h, bpp, fmt = int(w), int(h), int(bpp), int(fmt)
    b = np.frombuffer(d[nl + 1:nl + 1 + w * h * bpp], np.uint8)
    note = ''
    try:
        b = b.reshape(h, w, bpp)
        if fmt in (27, 28, 29, 30, 31):
            img = b[:, :, :3].astype(np.float32)
            note = f"alpha mean {b[:, :, 3].mean():.1f}"
        elif fmt in (87, 88, 90, 91):
            img = b[:, :, 2::-1].astype(np.float32)
        elif fmt == 26:
            v = b.copy().view('<u4')[:, :, 0]
            f3 = np.stack([uf(v & 0x7FF, 6), uf((v >> 11) & 0x7FF, 6), uf(v >> 22, 5)], -1)
            img = tone(f3)
            note = f"max {f3.max():.3g} mean {f3.mean():.3g}"
        elif fmt in (9, 10):
            f4 = b.copy().view('<f2').astype(np.float32)
            img = tone(f4[:, :, :3])
            note = f"max {np.nanmax(f4[:, :, :3]):.3g} mean {np.nanmean(f4[:, :, :3]):.3g} nan {int(np.isnan(f4).sum())}"
        elif fmt in (1, 2):
            f4 = b.copy().view('<f4')
            img = tone(f4[:, :, :3])
            note = f"max {np.nanmax(f4[:, :, :3]):.3g} nan {int(np.isnan(f4).sum())}"
        elif fmt in (39, 40, 41):
            f1 = b.copy().view('<f4')[:, :, 0]
            img = gray(norm(f1))
            note = f"min {np.nanmin(f1):.5g} max {np.nanmax(f1):.5g} nan {int(np.isnan(f1).sum())}"
        elif fmt in (19, 20, 21, 22):
            f1 = b[:, :, :4].copy().view('<f4')[:, :, 0]
            img = gray(norm(f1))
            note = f"depth min {np.nanmin(f1):.6g} max {np.nanmax(f1):.6g} distinct {len(np.unique(f1))}"
        elif fmt in (44, 45, 46, 47):
            f1 = (b.copy().view('<u4')[:, :, 0] & 0xFFFFFF) / float(0xFFFFFF)
            img = gray(norm(f1))
            note = f"depth min {f1.min():.6g} max {f1.max():.6g} distinct {len(np.unique(f1))}"
        elif fmt in (33, 34):
            f2 = b.copy().view('<f2').astype(np.float32)
            img = np.stack([norm(f2[:, :, 0]), norm(f2[:, :, 1]), np.zeros((h, w))], -1)
            note = f"absmax {np.nanmax(np.abs(f2)):.3g}"
        elif fmt in (53, 54):
            f1 = b.copy().view('<f2')[:, :, 0].astype(np.float32)
            img = gray(norm(f1))
            note = f"min {np.nanmin(f1):.5g} max {np.nanmax(f1):.5g}"
        elif fmt in (11, 12):
            img = (b.copy().view('<u2')[:, :, :3] >> 8).astype(np.float32)
        elif bpp >= 3:
            img = b[:, :, :3].astype(np.float32)
            note = '(raw bytes)'
        else:
            img = gray(b[:, :, 0].astype(np.float32))
            note = '(raw bytes)'
    except Exception as e:  # noqa: BLE001
        img = np.zeros((max(h, 1), max(w, 1), 3))
        note = f'decode error {e!r}'
    name = os.path.basename(f)[:-4]
    png(os.path.join(out, name + '.png'), img)
    stats.append(f"{name}: {w}x{h} fmt {fmt} bpp {bpp} mean {img.reshape(-1, 3).mean(0).round(1).tolist()} {note}")
open(os.path.join(out, 'stats.txt'), 'w').write('\n'.join(stats) + '\n')
print(len(stats), 'images ->', out)

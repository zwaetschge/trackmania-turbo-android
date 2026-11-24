"""Sanity statistics of a WAV capture of the game's audio (OpenAL Soft "wave" backend; the
header's length fields may be zero when the process was killed, so the data chunk is read
to the end of the file). usage: wav_check.py <file.wav>
Reports clipping, silent gaps inside sound, sample-to-sample jumps (clicks) and how the
level is distributed over time -- enough to tell a healthy mix from one with NaN bursts,
drop-outs or crackle."""
import sys, struct, numpy as np
d = open(sys.argv[1], 'rb').read()
assert d[:4] == b'RIFF' and d[8:12] == b'WAVE', 'not a WAV file'
pos = 12; fmt = None; data = None
while pos + 8 <= len(d):
    tag, size = d[pos:pos + 4], struct.unpack_from('<I', d, pos + 4)[0]
    if tag == b'fmt ': fmt = struct.unpack_from('<HHIIHH', d, pos + 8)
    if tag == b'data': data = d[pos + 8:] if size == 0 or pos + 8 + size > len(d) else d[pos + 8:pos + 8 + size]; break
    pos += 8 + size + (size & 1)
code, ch, rate, _, _, bits = fmt
if code == 3 or bits == 32 and code != 1: x = np.frombuffer(data[:len(data) // 4 * 4], '<f4').astype(np.float64)
elif bits == 16: x = np.frombuffer(data[:len(data) // 2 * 2], '<i2').astype(np.float64) / 32768.0
else: raise SystemExit(f'unsupported format code {code} bits {bits}')
x = x[:len(x) // ch * ch].reshape(-1, ch)
n = len(x); sec = n / rate
bad = ~np.isfinite(x); x = np.where(bad, 0.0, x)
mono = x.mean(1)
print(f"{sec:.1f} s, {rate} Hz, {ch} channel(s), {bits} bit; non-finite samples {int(bad.sum())}")
print(f"peak {np.abs(x).max():.3f}, rms {np.sqrt((x ** 2).mean()):.4f}, clipped (|x| >= 0.999) {100.0 * (np.abs(x) >= 0.999).mean():.3f} %")
w = rate // 50                                             # 20 ms windows
m = mono[:len(mono) // w * w].reshape(-1, w)
rms = np.sqrt((m ** 2).mean(1)); loud = rms > 0.01
print(f"windows with sound {100.0 * loud.mean():.0f} %; level percentiles (rms) 10/50/90: {np.percentile(rms, 10):.4f} / {np.percentile(rms, 50):.4f} / {np.percentile(rms, 90):.4f}")
gaps = 0
for i in range(2, len(rms) - 2):                           # a silent 20 ms inside sound = drop-out
    if rms[i] < 0.0005 and rms[i - 2] > 0.02 and rms[i + 2] > 0.02: gaps += 1
jump = np.abs(np.diff(mono)); big = jump > 0.5
print(f"drop-outs (silent 20 ms inside sound): {gaps}; clicks (sample-to-sample jump > 0.5 of full scale): {int(big.sum())} ({big.sum() / max(sec, 1e-9):.1f} per second)")
lvl = [f"{v:.2f}" for v in np.sqrt((mono[:len(mono) // rate * rate].reshape(-1, rate) ** 2).mean(1))]
print("rms per second:", ' '.join(lvl[:40]))
